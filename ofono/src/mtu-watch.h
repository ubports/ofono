/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016-2017 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef MTU_WATCH_H
#define MTU_WATCH_H

struct mtu_watch;

struct mtu_watch *mtu_watch_new(int max_mtu);
void mtu_watch_free(struct mtu_watch *mw);
void mtu_watch_set_ifname(struct mtu_watch *mw, const char *ifname);

#endif /* MTU_WATCH_H */
