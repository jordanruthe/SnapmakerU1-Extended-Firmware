# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

TARGET = libgui-vendors.so

CFLAGS = -Wall -Wextra -O2 -fPIC
LDFLAGS = -shared

SRCS = main.c core.c patch.c
OBJS = $(SRCS:.c=.o)

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c gui_vendors.h jsmn.h
	$(CC) $(CFLAGS) -c $< -o $@

strip: $(TARGET)
	$(STRIP) $(TARGET)

# Host unit tests: always native, never the cross compiler.
test: test/test_main
	./test/test_main

test/test_main: test/test_main.c core.c patch.c gui_vendors.h jsmn.h
	gcc -Wall -Wextra -O1 -g -fsanitize=address,undefined -I. -o $@ test/test_main.c core.c patch.c

clean:
	rm -f $(OBJS) $(TARGET) test/test_main

install: $(TARGET)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(LIBDIR)/

.PHONY: all clean strip install test
