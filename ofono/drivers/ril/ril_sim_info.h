/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016 Jolla Ltd.
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

#ifndef RIL_SIM_INFO_H
#define RIL_SIM_INFO_H

#include "ril_types.h"

struct ril_sim_info {
	GObject object;
	struct ril_sim_info_priv *priv;
	const char *iccid;
	const char *imsi;
	const char *spn;
};

struct ofono_sim;
typedef void (*ril_sim_info_cb_t)(struct ril_sim_info *info, void *arg);

struct ril_sim_info *ril_sim_info_new(const char *log_prefix);
struct ril_sim_info *ril_sim_info_ref(struct ril_sim_info *info);
void ril_sim_info_unref(struct ril_sim_info *si);
void ril_sim_info_set_ofono_sim(struct ril_sim_info *si, struct ofono_sim *sim);
void ril_sim_info_set_network(struct ril_sim_info *si, struct ril_network *net);
gulong ril_sim_info_add_iccid_changed_handler(struct ril_sim_info *si,
					ril_sim_info_cb_t cb, void *arg);
gulong ril_sim_info_add_imsi_changed_handler(struct ril_sim_info *si,
					ril_sim_info_cb_t cb, void *arg);
gulong ril_sim_info_add_spn_changed_handler(struct ril_sim_info *si,
					ril_sim_info_cb_t cb, void *arg);
void ril_sim_info_remove_handler(struct ril_sim_info *si, gulong id);

#endif /* RIL_SIM_INFO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
