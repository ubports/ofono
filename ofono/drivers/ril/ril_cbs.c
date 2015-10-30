/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015 Jolla Ltd.
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
#include "ril_util.h"
#include "ril_log.h"
#include "ril_constants.h"

struct ril_cbs {
	struct ofono_cbs *cbs;
	GRilIoChannel *io;
	guint timer_id;
	gulong event_id;
};

static void ril_set_topics(struct ofono_cbs *cbs, const char *topics,
				ofono_cbs_set_cb_t cb, void *data)
{
	struct ofono_error error;
	cb(ril_error_ok(&error), data);
}

static void ril_clear_topics(struct ofono_cbs *cbs,
				ofono_cbs_set_cb_t cb, void *data)
{
	struct ofono_error error;
	cb(ril_error_ok(&error), data);
}

static void ril_cbs_notify(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	struct ril_cbs *cd = user_data;
	GRilIoParser rilp;
	char* pdu;

	GASSERT(code == RIL_UNSOL_ON_USSD);
	grilio_parser_init(&rilp, data, len);
	pdu = grilio_parser_get_utf8(&rilp);
	DBG("%s", pdu);
	if (pdu) {
		ofono_cbs_notify(cd->cbs, (const guchar *)pdu, strlen(pdu));
		g_free(pdu);
	}
}

static gboolean ril_cbs_register(gpointer user_data)
{
	struct ril_cbs *cd = user_data;

	DBG("");
	GASSERT(cd->timer_id);
	cd->timer_id = 0;
	ofono_cbs_register(cd->cbs);

	cd->event_id = grilio_channel_add_unsol_event_handler(cd->io,
		ril_cbs_notify, RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS, cd);

	/* Single-shot */
	return FALSE;
}

static int ril_cbs_probe(struct ofono_cbs *cbs, unsigned int vendor,
								void *data)
{
	struct ril_modem *modem = data;
	struct ril_cbs *cd = g_try_new0(struct ril_cbs, 1);

	DBG("");
	cd->cbs = cbs;
	cd->io = grilio_channel_ref(ril_modem_io(modem));
	cd->timer_id = g_idle_add(ril_cbs_register, cd);
	ofono_cbs_set_data(cbs, cd);
	return 0;
}

static void ril_cbs_remove(struct ofono_cbs *cbs)
{
	struct ril_cbs *cd = ofono_cbs_get_data(cbs);

	DBG("");
	ofono_cbs_set_data(cbs, NULL);

	if (cd->timer_id > 0) {
		g_source_remove(cd->timer_id);
	}

	grilio_channel_remove_handler(cd->io, cd->event_id);
	grilio_channel_unref(cd->io);
	g_free(cd);
}

const struct ofono_cbs_driver ril_cbs_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_cbs_probe,
	.remove         = ril_cbs_remove,
	.set_topics     = ril_set_topics,
	.clear_topics   = ril_clear_topics
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
