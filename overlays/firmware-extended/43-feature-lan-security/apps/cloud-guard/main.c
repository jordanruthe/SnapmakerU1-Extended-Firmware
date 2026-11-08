// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <dlfcn.h>
#include <syslog.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_ALLOWED 64
#define MAX_LOGGED 64
#define MAX_HOSTS 16

typedef int (*connect_fn)(int, const struct sockaddr *, socklen_t);
typedef ssize_t (*sendto_fn)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
typedef ssize_t (*sendmsg_fn)(int, const struct msghdr *, int);
typedef int (*sendmmsg_fn)(int, struct mmsghdr *, unsigned int, int);
typedef int (*getaddrinfo_fn)(const char *, const char *, const struct addrinfo *, struct addrinfo **);

static connect_fn real_connect;
static sendto_fn real_sendto;
static sendmsg_fn real_sendmsg;
static sendmmsg_fn real_sendmmsg;
static getaddrinfo_fn real_getaddrinfo;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_active = false;

static char g_hosts[MAX_HOSTS][256];
static int g_nhosts = 0;

struct ip_entry {
    int family;
    unsigned char addr[16];
};
static struct ip_entry g_allowed[MAX_ALLOWED];
static int g_nallowed = 0;
static struct ip_entry g_logged[MAX_LOGGED];
static int g_nlogged = 0;

static void init_once(void)
{
    real_connect = (connect_fn)dlsym(RTLD_NEXT, "connect");
    real_sendto = (sendto_fn)dlsym(RTLD_NEXT, "sendto");
    real_sendmsg = (sendmsg_fn)dlsym(RTLD_NEXT, "sendmsg");
    real_sendmmsg = (sendmmsg_fn)dlsym(RTLD_NEXT, "sendmmsg");
    real_getaddrinfo = (getaddrinfo_fn)dlsym(RTLD_NEXT, "getaddrinfo");

    const char *block = getenv("CLOUD_GUARD_BLOCK");
    if (!block || !atoi(block))
        return;

    /* Guard unisrv, and anything it starts (they inherit CLOUD_GUARD_ACTIVE) */
    bool is_unisrv = false;
    char self_path[256];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len >= 0) {
        self_path[len] = '\0';
        const char *base = strrchr(self_path, '/');
        base = base ? base + 1 : self_path;
        is_unisrv = strcmp(base, "unisrv") == 0;
    }
    const char *inherited = getenv("CLOUD_GUARD_ACTIVE");
    if (!is_unisrv && !(inherited && atoi(inherited)))
        return;
    setenv("CLOUD_GUARD_ACTIVE", "1", 1);

    const char *hosts = getenv("CLOUD_GUARD_ALLOW_HOSTS");
    if (hosts) {
        char *copy = strdup(hosts);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ", ", &save); tok && g_nhosts < MAX_HOSTS; tok = strtok_r(NULL, ", ", &save)) {
            strncpy(g_hosts[g_nhosts], tok, sizeof(g_hosts[0]) - 1);
            g_nhosts++;
        }
        free(copy);
    }

    openlog("cloud-guard", LOG_PID, LOG_DAEMON);
    g_active = true;
    syslog(LOG_INFO, "Active in pid %d: blocking non-local connections", getpid());
}

static bool host_allowed(const char *host)
{
    size_t hl = strlen(host);
    for (int i = 0; i < g_nhosts; i++) {
        size_t pl = strlen(g_hosts[i]);
        if (hl == pl && strcasecmp(host, g_hosts[i]) == 0)
            return true;
        if (hl > pl && host[hl - pl - 1] == '.' && strcasecmp(host + hl - pl, g_hosts[i]) == 0)
            return true;
    }
    return false;
}

static bool v4_local(const unsigned char *a)
{
    return a[0] == 127 ||                                  /* loopback */
           a[0] == 10 ||                                   /* RFC1918 */
           (a[0] == 172 && (a[1] & 0xf0) == 16) ||
           (a[0] == 192 && a[1] == 168) ||
           (a[0] == 169 && a[1] == 254) ||                 /* link-local */
           (a[0] == 100 && (a[1] & 0xc0) == 64) ||         /* CGNAT (Tailscale) */
           (a[0] & 0xf0) == 224 ||                         /* multicast */
           (a[0] == 255 && a[1] == 255 && a[2] == 255 && a[3] == 255) ||
           (a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0);
}

static bool v6_local(const unsigned char *a)
{
    static const unsigned char zero[16];
    static const unsigned char loop[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (!memcmp(a, zero, 16) || !memcmp(a, loop, 16))
        return true;
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80)             /* fe80::/10 */
        return true;
    if ((a[0] & 0xfe) == 0xfc)                             /* fc00::/7 */
        return true;
    if (a[0] == 0xff)                                      /* multicast */
        return true;
    static const unsigned char mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    if (!memcmp(a, mapped, 12))
        return v4_local(a + 12);
    return false;
}

static bool in_table(const struct ip_entry *tab, int n, int family, const unsigned char *addr)
{
    int sz = family == AF_INET ? 4 : 16;
    for (int i = 0; i < n; i++)
        if (tab[i].family == family && !memcmp(tab[i].addr, addr, sz))
            return true;
    return false;
}

static void remember(struct ip_entry *tab, int *n, int max, int family, const unsigned char *addr)
{
    if (*n >= max || in_table(tab, *n, family, addr))
        return;
    tab[*n].family = family;
    memset(tab[*n].addr, 0, 16);
    memcpy(tab[*n].addr, addr, family == AF_INET ? 4 : 16);
    (*n)++;
}

/* Returns true when the datagram/connection may go ahead */
static bool dest_ok(const struct sockaddr *sa, socklen_t len)
{
    if (!g_active || !sa)
        return true;

    const unsigned char *addr;
    int family = sa->sa_family;
    int port;
    if (family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        addr = (const unsigned char *)&in->sin_addr;
        port = ntohs(in->sin_port);
        if (v4_local(addr))
            return true;
    } else if (family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        addr = (const unsigned char *)&in6->sin6_addr;
        port = ntohs(in6->sin6_port);
        if (v6_local(addr))
            return true;
    } else {
        return true; /* AF_UNIX, AF_NETLINK, ... */
    }

    pthread_mutex_lock(&g_lock);
    bool ok = in_table(g_allowed, g_nallowed, family, addr);
    bool first = false;
    if (!ok && !in_table(g_logged, g_nlogged, family, addr)) {
        remember(g_logged, &g_nlogged, MAX_LOGGED, family, addr);
        first = true;
    }
    pthread_mutex_unlock(&g_lock);

    if (!ok && first) {
        char buf[INET6_ADDRSTRLEN];
        inet_ntop(family, addr, buf, sizeof(buf));
        syslog(LOG_WARNING, "Blocked connection to %s port %d", buf, port);
    }
    return ok;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    pthread_once(&g_once, init_once);
    int rc = real_getaddrinfo(node, service, hints, res);
    if (rc != 0 || !g_active || !node || !host_allowed(node))
        return rc;

    pthread_mutex_lock(&g_lock);
    for (struct addrinfo *ai = *res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET)
            remember(g_allowed, &g_nallowed, MAX_ALLOWED, AF_INET,
                     (const unsigned char *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr);
        else if (ai->ai_family == AF_INET6)
            remember(g_allowed, &g_nallowed, MAX_ALLOWED, AF_INET6,
                     (const unsigned char *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    pthread_once(&g_once, init_once);
    if (!dest_ok(addr, len)) {
        errno = ENETUNREACH;
        return -1;
    }
    return real_connect(fd, addr, len);
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *addr, socklen_t len)
{
    pthread_once(&g_once, init_once);
    if (!dest_ok(addr, len)) {
        errno = ENETUNREACH;
        return -1;
    }
    return real_sendto(fd, buf, n, flags, addr, len);
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    pthread_once(&g_once, init_once);
    if (msg && !dest_ok((const struct sockaddr *)msg->msg_name, msg->msg_namelen)) {
        errno = ENETUNREACH;
        return -1;
    }
    return real_sendmsg(fd, msg, flags);
}

int sendmmsg(int fd, struct mmsghdr *msgs, unsigned int vlen, int flags)
{
    pthread_once(&g_once, init_once);
    for (unsigned int i = 0; i < vlen; i++) {
        if (!dest_ok((const struct sockaddr *)msgs[i].msg_hdr.msg_name, msgs[i].msg_hdr.msg_namelen)) {
            errno = ENETUNREACH;
            return -1;
        }
    }
    return real_sendmmsg(fd, msgs, vlen, flags);
}
