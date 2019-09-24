/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
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

#include "util.h"

enum MBIM_GROUP {
	SIM_GROUP = 1,
	NETREG_GROUP = 2,
	SMS_GROUP = 3,
	GPRS_GROUP = 4,
	GPRS_CONTEXT_GROUP = 101,
};

extern void mbim_devinfo_init(void);
extern void mbim_devinfo_exit(void);

extern void mbim_sim_init(void);
extern void mbim_sim_exit(void);

extern void mbim_netreg_init(void);
extern void mbim_netreg_exit(void);

extern void mbim_sms_init(void);
extern void mbim_sms_exit(void);

extern void mbim_gprs_init(void);
extern void mbim_gprs_exit(void);

extern void mbim_gprs_context_init(void);
extern void mbim_gprs_context_exit(void);
