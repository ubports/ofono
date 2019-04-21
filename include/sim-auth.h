/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef __OFONO_SIM_AUTH_H
#define __OFONO_SIM_AUTH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include <ofono/types.h>

struct ofono_sim_auth;

struct ofono_sim_auth *ofono_sim_auth_create(struct ofono_modem *modem);
void ofono_sim_auth_remove(struct ofono_sim_auth *sa);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_SIM_AUTH_H */
