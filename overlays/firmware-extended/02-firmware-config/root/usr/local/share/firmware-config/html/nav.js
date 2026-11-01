// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

// Loaded by Fluidd's index.html (see inject_extended_nav.sh). Adds an
// "Extended Firmware" section to the drawer behind Fluidd's three-dots button,
// listing Firmware Config and the pages enabled in it (/firmware-config/api/links).
// Fluidd is minified with hashed file names, so this hooks the rendered
// Vuetify markup instead of patching the bundle, and does nothing if the
// markup is not found. Mainsail gets the same links from .theme/navi.json.
(function () {
    'use strict';

    const MARKER = 'data-extended-nav';
    const CACHE_MS = 30000;
    let links = null;
    let fetchedAt = 0;
    let fetching = false;

    function getJWT() {
        const instanceKey = `user-token-${window.location.host.replace(/[^a-zA-Z0-9]/g, '_')}`;
        let token = localStorage.getItem(instanceKey);
        if (token) return token;
        for (let i = 0; i < localStorage.length; i++) {
            const key = localStorage.key(i);
            if (key && key.startsWith('user-token-')) {
                token = localStorage.getItem(key);
                if (token) return token;
            }
        }
        return null;
    }

    async function fetchLinks() {
        if (fetching || Date.now() - fetchedAt < CACHE_MS) return;
        fetching = true;
        try {
            const jwt = getJWT();
            const resp = await fetch('/firmware-config/api/links', {
                headers: jwt ? { 'Authorization': `Bearer ${jwt}` } : {}
            });
            // Firmware Config disabled or stopped: show nothing
            links = resp.ok
                ? [{ url: '/firmware-config/', icon: '⚙️', label: 'Firmware Config' },
                   ...(await resp.json()).filter(l => l.url !== '/')]
                : [];
            fetchedAt = Date.now();
        } catch (e) {
            links = [];
        } finally {
            // Retry failures (e.g. before logging in) sooner
            if (!links.length) fetchedAt = Date.now() - CACHE_MS + 5000;
            fetching = false;
        }
        render();
    }

    function el(tag, className, text) {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (text) node.textContent = text;
        return node;
    }

    function render() {
        // Host / Power / Services groups in the right-hand tools drawer
        const groups = document.querySelectorAll('.v-navigation-drawer--right .v-list-group');
        if (!groups.length) return;
        const anchor = groups[groups.length - 1];
        const parent = anchor.parentElement;
        const theme = anchor.closest('.theme--light') ? 'theme--light' : 'theme--dark';

        const old = parent.querySelector(`[${MARKER}]`);
        if (old && old.dataset.extendedNav === String(fetchedAt)) return;
        if (old) old.remove();
        if (!links || !links.length) return;

        const section = el('div');
        section.setAttribute(MARKER, String(fetchedAt));
        section.appendChild(el('div', `v-subheader ${theme}`, 'Extended Firmware'));
        for (const link of links) {
            const item = el('a', `v-list-item v-list-item--link ${theme}`);
            item.href = link.url;
            item.target = '_blank';
            item.rel = 'noopener';
            const icon = el('div', 'v-list-item__icon', link.icon);
            icon.style.justifyContent = 'center';
            const content = el('div', 'v-list-item__content');
            content.appendChild(el('div', 'v-list-item__title', link.label));
            item.append(icon, content);
            section.appendChild(item);
        }
        anchor.after(section);
    }

    function update() {
        render();
        if (document.querySelector('.v-navigation-drawer--right.v-navigation-drawer--open')) {
            fetchLinks();
        }
    }

    let scheduled = false;
    new MutationObserver(() => {
        if (scheduled) return;
        scheduled = true;
        requestAnimationFrame(() => { scheduled = false; update(); });
    }).observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ['class'] });

    fetchLinks();
})();
