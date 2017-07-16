/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2017 Jolla Ltd.
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

#include "ril_plugin.h"
#include "ril_sim_settings.h"
#include "ril_util.h"
#include "ril_log.h"

struct ril_radio_settings {
	struct ofono_radio_settings *rs;
	struct ril_sim_settings *settings;
	const char *log_prefix;
	char *allocated_log_prefix;
	guint source_id;
};

struct ril_radio_settings_cbd {
	struct ril_radio_settings *rsd;
	union _ofono_radio_settings_cb {
		ofono_radio_settings_rat_mode_set_cb_t rat_mode_set;
		ofono_radio_settings_rat_mode_query_cb_t rat_mode_query;
		ofono_radio_settings_available_rats_query_cb_t available_rats;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define DBG_(rsd,fmt,args...) DBG("%s" fmt, (rsd)->log_prefix, ##args)

static inline struct ril_radio_settings *ril_radio_settings_get_data(
					struct ofono_radio_settings *rs)
{
	return ofono_radio_settings_get_data(rs);
}

static void ril_radio_settings_later(struct ril_radio_settings *rsd,
				GSourceFunc fn, void *cb, void *data)
{
	struct ril_radio_settings_cbd *cbd;

	cbd = g_new0(struct ril_radio_settings_cbd, 1);
	cbd->rsd = rsd;
	cbd->cb.ptr = cb;
	cbd->data = data;

	GASSERT(!rsd->source_id);
	rsd->source_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
							fn, cbd, g_free);
}

static gboolean ril_radio_settings_set_rat_mode_cb(gpointer user_data)
{
	struct ofono_error error;
	struct ril_radio_settings_cbd *cbd = user_data;
	struct ril_radio_settings *rsd = cbd->rsd;

	GASSERT(rsd->source_id);
	rsd->source_id = 0;
	cbd->cb.rat_mode_set(ril_error_ok(&error), cbd->data);
	return G_SOURCE_REMOVE;
}

static void ril_radio_settings_set_rat_mode(struct ofono_radio_settings *rs,
		enum ofono_radio_access_mode mode,
		ofono_radio_settings_rat_mode_set_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);
	DBG_(rsd, "%s", ofono_radio_access_mode_to_string(mode));
	ril_sim_settings_set_pref_mode(rsd->settings, mode);
	ril_radio_settings_later(rsd, ril_radio_settings_set_rat_mode_cb,
								cb, data);
}

static gboolean ril_radio_settings_query_rat_mode_cb(gpointer user_data)
{
	struct ril_radio_settings_cbd *cbd = user_data;
	struct ril_radio_settings *rsd = cbd->rsd;
	enum ofono_radio_access_mode mode = rsd->settings->pref_mode;
	struct ofono_error error;

	DBG_(rsd, "rat mode %s", ofono_radio_access_mode_to_string(mode));
	GASSERT(rsd->source_id);
	rsd->source_id = 0;
	cbd->cb.rat_mode_query(ril_error_ok(&error), mode, cbd->data);
	return G_SOURCE_REMOVE;
}

static void ril_radio_settings_query_rat_mode(struct ofono_radio_settings *rs,
		ofono_radio_settings_rat_mode_query_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG_(rsd, "");
	ril_radio_settings_later(rsd, ril_radio_settings_query_rat_mode_cb,
								cb, data);
}

static gboolean ril_radio_settings_query_available_rats_cb(gpointer data)
{
	struct ofono_error error;
	struct ril_radio_settings_cbd *cbd = data;
	struct ril_radio_settings *rsd = cbd->rsd;

	GASSERT(rsd->source_id);
	rsd->source_id = 0;
	cbd->cb.available_rats(ril_error_ok(&error), rsd->settings->techs,
								cbd->data);
	return G_SOURCE_REMOVE;
}

static void ril_radio_settings_query_available_rats(
		struct ofono_radio_settings *rs,
		ofono_radio_settings_available_rats_query_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG_(rsd, "");
	ril_radio_settings_later(rsd,
			ril_radio_settings_query_available_rats_cb, cb, data);
}

static gboolean ril_radio_settings_register(gpointer user_data)
{
	struct ril_radio_settings *rsd = user_data;
	GASSERT(rsd->source_id);
	rsd->source_id = 0;
	ofono_radio_settings_register(rsd->rs);
	return G_SOURCE_REMOVE;
}

static int ril_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_radio_settings *rsd = g_new0(struct ril_radio_settings, 1);

	DBG("%s", modem->log_prefix);
	rsd->rs = rs;
	rsd->settings = ril_sim_settings_ref(modem->sim_settings);
	rsd->source_id = g_idle_add(ril_radio_settings_register, rsd);

	if (modem->log_prefix && modem->log_prefix[0]) {
		rsd->log_prefix = rsd->allocated_log_prefix =
			g_strconcat(modem->log_prefix, " ", NULL);
	} else {
		rsd->log_prefix = "";
	}

	ofono_radio_settings_set_data(rs, rsd);
	return 0;
}

static void ril_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG_(rsd, "");
	ofono_radio_settings_set_data(rs, NULL);
	if (rsd->source_id) {
		g_source_remove(rsd->source_id);
        }
	ril_sim_settings_unref(rsd->settings);
	g_free(rsd->allocated_log_prefix);
	g_free(rsd);
}

const struct ofono_radio_settings_driver ril_radio_settings_driver = {
	.name                 = RILMODEM_DRIVER,
	.probe                = ril_radio_settings_probe,
	.remove               = ril_radio_settings_remove,
	.query_rat_mode       = ril_radio_settings_query_rat_mode,
	.set_rat_mode         = ril_radio_settings_set_rat_mode,
	.query_available_rats = ril_radio_settings_query_available_rats
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
