/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  Endocode AG. All rights reserved.
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

#include <drivers/atmodem/atutil.h>

#define UBLOXMODEM "ubloxmodem"

enum ublox_flags {
	UBLOX_F_TOBY_L2		= (1 << 0),
	UBLOX_F_TOBY_L4		= (1 << 1),
	UBLOX_F_LARA_R2		= (1 << 2),
	UBLOX_F_HAVE_USBCONF	= (1 << 3),
};

struct ublox_model {
	char *name;
	int flags;
};

const struct ublox_model *ublox_model_from_name(const char *name);
const struct ublox_model *ublox_model_from_id(int id);
int ublox_model_to_id(const struct ublox_model *model);
int ublox_is_toby_l2(const struct ublox_model *model);
int ublox_is_toby_l4(const struct ublox_model *model);

extern void ublox_gprs_context_init(void);
extern void ublox_gprs_context_exit(void);

void ublox_netreg_init(void);
void ublox_netreg_exit(void);

extern void ublox_netmon_init(void);
extern void ublox_netmon_exit(void);

extern void ublox_lte_init(void);
extern void ublox_lte_exit(void);
