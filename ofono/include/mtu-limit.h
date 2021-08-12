/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016-2021 Jolla Ltd.
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

#ifndef __OFONO_MTU_LIMIT_H
#define __OFONO_MTU_LIMIT_H

/* This API exists since mer/1.24+git2 */

struct ofono_mtu_limit;

struct ofono_mtu_limit *ofono_mtu_limit_new(int max_mtu);
void ofono_mtu_limit_free(struct ofono_mtu_limit *ml);
void ofono_mtu_limit_set_ifname(struct ofono_mtu_limit *ml, const char *ifname);

#endif /* __OFONO_MTU_LIMIT_H */
