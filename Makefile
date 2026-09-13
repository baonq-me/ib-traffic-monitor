# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

CC = gcc
CFLAGS = -g -Wall -Wextra -Wpedantic -Wconversion -Wdouble-promotion -Wunused -Wshadow -Wsign-conversion -fsanitize=undefined
INCLUDES = -I.
SRCS = ib-traffic-monitor.c infiniband.c utils.c ncurses_utils.c
OBJS = $(SRCS:.c=.o)
TARGET = ib-traffic-monitor
LDFLAGS = -lncurses

# same warning set as the development build, without the sanitizer and with
# the hardening flags expected from a distributed package
RELEASE_CFLAGS = -O2 -Wall -Wextra -Wpedantic -Wconversion -Wdouble-promotion -Wunused -Wshadow -Wsign-conversion -D_FORTIFY_SOURCE=2 -fstack-protector-strong
RELEASE_LDFLAGS = -lncurses -Wl,-z,relro,-z,now

DESTDIR ?=
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
SYSTEMDDIR ?= /lib/systemd/system
DEFAULTDIR ?= /etc/default
INSTALL ?= install

.PHONY: all clean release install

all: $(TARGET)

release: clean
	$(MAKE) CFLAGS="$(RELEASE_CFLAGS)" LDFLAGS="$(RELEASE_LDFLAGS)" $(TARGET)

install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SYSTEMDDIR) $(DESTDIR)$(DEFAULTDIR)
	$(INSTALL) -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	$(INSTALL) -m 0644 packaging/ib-traffic-monitor.service $(DESTDIR)$(SYSTEMDDIR)/ib-traffic-monitor.service
	$(INSTALL) -m 0644 packaging/ib-traffic-monitor.default $(DESTDIR)$(DEFAULTDIR)/ib-traffic-monitor

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
