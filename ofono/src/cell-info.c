/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2021 Jolla Ltd.
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

#include "ofono.h"

int ofono_cell_compare_location(const struct ofono_cell *c1,
	const struct ofono_cell *c2)
{
	if (c1 && c2) {
		if (c1->type != c2->type) {
			return c1->type - c2->type;
		} else if (c1->type == OFONO_CELL_TYPE_GSM) {
			const struct ofono_cell_info_gsm *g1;
			const struct ofono_cell_info_gsm *g2;

			g1 = &c1->info.gsm;
			g2 = &c2->info.gsm;
			if (g1->mcc != g2->mcc) {
				return g1->mcc - g2->mcc;
			} else if (g1->mnc != g2->mnc) {
				return g1->mnc - g2->mnc;
			} else if (g1->lac != g2->lac) {
				return g1->lac - g2->lac;
			} else {
				return g1->cid - g2->cid;
			}
		} else if (c1->type == OFONO_CELL_TYPE_WCDMA) {
			const struct ofono_cell_info_wcdma *w1;
			const struct ofono_cell_info_wcdma *w2;

			w1 = &c1->info.wcdma;
			w2 = &c2->info.wcdma;
			if (w1->mcc != w2->mcc) {
				return w1->mcc - w2->mcc;
			} else if (w1->mnc != w2->mnc) {
				return w1->mnc - w2->mnc;
			} else if (w1->lac != w2->lac) {
				return w1->lac - w2->lac;
			} else {
				return w1->cid - w2->cid;
			}
		} else if (c1->type == OFONO_CELL_TYPE_LTE) {
			const struct ofono_cell_info_lte *l1 =
				&c1->info.lte;
			const struct ofono_cell_info_lte *l2 =
				&c2->info.lte;

			l1 = &c1->info.lte;
			l2 = &c2->info.lte;
			if (l1->mcc != l2->mcc) {
				return l1->mcc - l2->mcc;
			} else if (l1->mnc != l2->mnc) {
				return l1->mnc - l2->mnc;
			} else if (l1->ci != l2->ci) {
				return l1->ci - l2->ci;
			} else if (l1->pci != l2->pci) {
				return l1->pci - l2->pci;
			} else {
				return l1->tac - l2->tac;
			}
		} else {
			ofono_warn("Unexpected cell type");
			return 0;
		}
	} else if (c1) {
		return 1;
	} else if (c2) {
		return -1;
	} else {
		return 0;
	}
}

struct ofono_cell_info *ofono_cell_info_ref(struct ofono_cell_info *ci)
{
	if (ci && ci->proc->ref) {
		ci->proc->ref(ci);
	}
	return ci;
}

void ofono_cell_info_unref(struct ofono_cell_info *ci)
{
	if (ci && ci->proc->unref) {
		ci->proc->unref(ci);
	}
}

unsigned long ofono_cell_info_add_change_handler(struct ofono_cell_info *ci,
	ofono_cell_info_cb_t cb, void *data)
{
	return (ci && ci->proc->add_change_handler && cb) ?
		ci->proc->add_change_handler(ci, cb, data) : 0;
}

void ofono_cell_info_remove_handler(struct ofono_cell_info *ci,
	unsigned long id)
{
	if (ci && ci->proc->remove_handler && id) {
		ci->proc->remove_handler(ci, id);
	}
}

void ofono_cell_info_set_update_interval(struct ofono_cell_info *ci, int ms)
{
	if (ci && ci->proc->set_update_interval) {
		ci->proc->set_update_interval(ci, ms);
	}
}

void ofono_cell_info_set_enabled(struct ofono_cell_info *ci,
	ofono_bool_t enabled)
{
	if (ci && ci->proc->set_enabled) {
		ci->proc->set_enabled(ci, enabled);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
