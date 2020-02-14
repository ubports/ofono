/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (C) 2019  Norrbonn AB
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include "gatchat.h"
#include "gatresult.h"

#include "common.h"
#include "ubloxmodem.h"
#include "drivers/atmodem/vendor.h"

#include "drivers/atmodem/network-registration.h"

static const char *none_prefix[] = { NULL };
static const char *cmer_prefix[] = { "+CMER:", NULL };
static const char *ureg_prefix[] = { "+UREG:", NULL };

struct netreg_data {
	struct at_netreg_data at_data;

	const struct ublox_model *model;
	bool updating_status : 1;
};

struct tech_query {
	int status;
	int lac;
	int ci;
	int tech;
	struct ofono_netreg *netreg;
};

static void ciev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct at_netreg_data *nd = ofono_netreg_get_data(netreg);
	int strength, ind;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIEV:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &ind))
		return;

	if (ind != nd->signal_index)
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	if (strength == nd->signal_invalid)
		strength = -1;
	else
		strength = (strength * 100) / (nd->signal_max - nd->signal_min);

	ofono_netreg_strength_notify(netreg, strength);
}

static gboolean notify_time(gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct at_netreg_data *nd = ofono_netreg_get_data(netreg);

	nd->nitz_timeout = 0;

	ofono_netreg_time_notify(netreg, &nd->time);

	return FALSE;
}

static void ctzdst_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct at_netreg_data *nd = ofono_netreg_get_data(netreg);
	int dst;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CTZDST:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &dst))
		return;

	DBG("dst %d", dst);

	nd->time.dst = dst;

	if (nd->nitz_timeout > 0) {
		g_source_remove(nd->nitz_timeout);
		nd->nitz_timeout = 0;
	}

	ofono_netreg_time_notify(netreg, &nd->time);
}

static void ctzv_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct at_netreg_data *nd = ofono_netreg_get_data(netreg);
	int year, mon, mday, hour, min, sec;
	const char *tz, *time;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CTZV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &tz))
		return;

	if (!g_at_result_iter_next_string(&iter, &time))
		return;

	DBG("tz %s time %s", tz, time);

	if (sscanf(time, "%u/%u/%u,%u:%u:%u", &year, &mon, &mday,
						&hour, &min, &sec) != 6)
		return;

	nd->time.sec = sec;
	nd->time.min = min;
	nd->time.hour = hour;
	nd->time.mday = mday;
	nd->time.mon = mon;
	nd->time.year = 2000 + year;

	nd->time.utcoff = atoi(tz) * 15 * 60;

	/* Delay notification in case there's a DST update coming */
	if (nd->nitz_timeout > 0)
		g_source_remove(nd->nitz_timeout);

	nd->nitz_timeout = g_timeout_add_seconds(1, notify_time, user_data);
}

static void ctze_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct at_netreg_data *nd = ofono_netreg_get_data(netreg);
	int year, mon, mday, hour, min, sec;
	int dst;
	const char *tz, *time;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CTZE:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &tz))
		return;

	if (!g_at_result_iter_next_number(&iter, &dst))
		return;

	if (!g_at_result_iter_next_string(&iter, &time))
		return;

	DBG("tz %s dst %d time %s", tz, dst, time);

	if (sscanf(time, "%u/%u/%u,%u:%u:%u", &year, &mon, &mday,
						&hour, &min, &sec) != 6)
		return;

	nd->time.sec = sec;
	nd->time.min = min;
	nd->time.hour = hour;
	nd->time.mday = mday;
	nd->time.mon = mon;
	nd->time.year = 2000 + year;

	nd->time.utcoff = atoi(tz) * 15 * 60;
	nd->time.dst = dst;

	ofono_netreg_time_notify(netreg, &nd->time);
}

static int ublox_ureg_state_to_tech(int state)
{
	switch (state) {
	case 1:
		return ACCESS_TECHNOLOGY_GSM;
	case 2:
		return ACCESS_TECHNOLOGY_GSM_EGPRS;
	case 3:
		return ACCESS_TECHNOLOGY_UTRAN;
	case 4:
		return ACCESS_TECHNOLOGY_UTRAN_HSDPA;
	case 5:
		return ACCESS_TECHNOLOGY_UTRAN_HSUPA;
	case 6:
		return ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;
	case 7:
		return ACCESS_TECHNOLOGY_EUTRAN;
	case 8:
		return ACCESS_TECHNOLOGY_GSM;
	case 9:
		return ACCESS_TECHNOLOGY_GSM_EGPRS;
	default:
		/* Not registered for PS (0) or something unknown (>9)... */
		return -1;
	}
}

static gboolean is_registered(int status)
{
	return status == NETWORK_REGISTRATION_STATUS_REGISTERED ||
		status == NETWORK_REGISTRATION_STATUS_ROAMING;
}

static void registration_status_cb(const struct ofono_error *error,
				   int status, int lac, int ci, int tech,
				   void *user_data)
{
	struct tech_query *tq = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(tq->netreg);
	struct ofono_netreg *netreg = tq->netreg;

	/* The query provided a tech, use that */
	if (is_registered(status) && tq->tech != -1)
		tech = tq->tech;

	g_free(tq);

	nd->updating_status = false;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during registration status query");
		return;
	}

	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void ublox_ureg_cb(gboolean ok, GAtResult *result,
							gpointer user_data)
{
	struct tech_query *tq = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(tq->netreg);
	GAtResultIter iter;
	gint enabled, state;
	int tech = -1;

	nd->updating_status = false;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+UREG:")) {
		if (!g_at_result_iter_next_number(&iter, &enabled))
			return;

		/* Sometimes we get an unsolicited UREG here, skip it */
		if (!g_at_result_iter_next_number(&iter, &state))
			continue;

		tech = ublox_ureg_state_to_tech(state);
		break;
	}

error:
	if (tech < 0)
		/* No valid UREG status, we have to trust CREG... */
		tech = tq->tech;

	ofono_netreg_status_notify(tq->netreg,
			tq->status, tq->lac, tq->ci, tech);
}

static void ureg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct tech_query *tq;
	GAtResultIter iter;
	int state;

	if (nd->updating_status)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+UREG:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	tq = g_new0(struct tech_query, 1);

	tq->tech = ublox_ureg_state_to_tech(state);
	tq->netreg = netreg;

	nd->updating_status = true;
	at_registration_status(netreg, registration_status_cb, tq);
}

static void creg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct tech_query *tq;
	int status;
	int lac;
	int ci;
	int tech;

	if (nd->updating_status)
		return;

	if (at_util_parse_reg_unsolicited(result, "+CREG:", &status,
			&lac, &ci, &tech, OFONO_VENDOR_GENERIC) == FALSE)
		return;

	if (!is_registered(status))
		goto notify;

	if (ublox_is_toby_l4(nd->model) || ublox_is_toby_l2(nd->model)) {
		tq = g_new0(struct tech_query, 1);

		tq->status = status;
		tq->lac = lac;
		tq->ci = ci;
		tq->tech = tech;
		tq->netreg = netreg;

		if (g_at_chat_send(nd->at_data.chat, "AT+UREG?", ureg_prefix,
				ublox_ureg_cb, tq, g_free) > 0) {
			nd->updating_status = true;
			return;
		}

		g_free(tq);
	}

	if (tech == -1)
		tech = nd->at_data.tech;

notify:
	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void at_cmer_not_supported(struct ofono_netreg *netreg)
{
	ofono_error("+CMER not supported by this modem.  If this is an error"
			" please submit patches to support this hardware");

	ofono_netreg_remove(netreg);
}

static void ublox_finish_registration(struct ofono_netreg *netreg)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (ublox_is_toby_l4(nd->model) || ublox_is_toby_l2(nd->model))
		g_at_chat_register(nd->at_data.chat, "+UREG:",
					ureg_notify, FALSE, netreg, NULL);

	g_at_chat_register(nd->at_data.chat, "+CIEV:",
			ciev_notify, FALSE, netreg, NULL);

	g_at_chat_register(nd->at_data.chat, "+CREG:",
				creg_notify, FALSE, netreg, NULL);

	ofono_netreg_register(netreg);
}

static void ublox_ureg_set_cb(gboolean ok,
				GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;

	if (!ok) {
		ofono_error("Unable to initialize Network Registration");
		ofono_netreg_remove(netreg);
		return;
	}

	ublox_finish_registration(netreg);
}

static void ublox_cmer_set_cb(gboolean ok,
				GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (!ok) {
		at_cmer_not_supported(netreg);
		return;
	}

	if (ublox_is_toby_l4(nd->model) || ublox_is_toby_l2(nd->model)) {
		g_at_chat_send(nd->at_data.chat, "AT+UREG=1", none_prefix,
			ublox_ureg_set_cb, netreg, NULL);

		return;
	}

	ublox_finish_registration(netreg);
}

static void ublox_creg_set_cb(gboolean ok,
				GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (!ok) {
		ofono_error("Unable to initialize Network Registration");
		ofono_netreg_remove(netreg);
		return;
	}

	if (ublox_is_toby_l4(nd->model))
		/* FIXME */
		ofono_error("TOBY L4 requires polling of ECSQ");

	/* Register for network time update reports */
	if (ublox_is_toby_l2(nd->model)) {
		/* TOBY L2 does not support CTZDST */
		g_at_chat_register(nd->at_data.chat, "+CTZE:", ctze_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->at_data.chat, "AT+CTZR=2", none_prefix,
						NULL, NULL, NULL);
	} else {
		g_at_chat_register(nd->at_data.chat, "+CTZV:", ctzv_notify,
						FALSE, netreg, NULL);
		g_at_chat_register(nd->at_data.chat, "+CTZDST:", ctzdst_notify,
						FALSE, netreg, NULL);
		g_at_chat_send(nd->at_data.chat, "AT+CTZR=1", none_prefix,
						NULL, NULL, NULL);
	}

	/* AT+CMER NOTES:
	 * - For all u-blox models, mode 3 is equivalent to mode 1;
	 * since some models do not support setting modes 2 nor 3
	 * (see UBX-13002752), we prefer mode 1 for all models.
	 * - The TOBY L4 does not support ind=2
	 */
	g_at_chat_send(nd->at_data.chat, "AT+CMER=1,0,0,1", cmer_prefix,
			ublox_cmer_set_cb, netreg, NULL);
}

/*
 * uBlox netreg atom probe.
 * - takes uBlox model ID parameter instead of AT vendor ID
 */
static int ublox_netreg_probe(struct ofono_netreg *netreg,
				unsigned int model_id,
				void *data)
{
	GAtChat *chat = data;
	struct netreg_data *nd;

	nd = g_new0(struct netreg_data, 1);

	nd->model = ublox_model_from_id(model_id);

	/* There should be no uBlox-specific quirks in the 'generic'
	 * AT driver
	 */
	nd->at_data.vendor = OFONO_VENDOR_GENERIC;

	nd->at_data.chat = g_at_chat_clone(chat);
	nd->at_data.tech = -1;
	nd->at_data.time.sec = -1;
	nd->at_data.time.min = -1;
	nd->at_data.time.hour = -1;
	nd->at_data.time.mday = -1;
	nd->at_data.time.mon = -1;
	nd->at_data.time.year = -1;
	nd->at_data.time.dst = 0;
	nd->at_data.time.utcoff = 0;
	ofono_netreg_set_data(netreg, nd);

	/* All uBlox devices support n=2 so no need to query this */
	g_at_chat_send(nd->at_data.chat, "AT+CREG=2", none_prefix,
			ublox_creg_set_cb, netreg, NULL);

	return 0;
}

static const struct ofono_netreg_driver driver = {
	.name				= "ubloxmodem",
	.probe				= ublox_netreg_probe,
	.remove				= at_netreg_remove,
	.registration_status		= at_registration_status,
	.current_operator		= at_current_operator,
	.list_operators			= at_list_operators,
	.register_auto			= at_register_auto,
	.register_manual		= at_register_manual,
	.strength			= at_signal_strength,
};

void ublox_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void ublox_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
