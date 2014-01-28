/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd.
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

#include <errno.h>
#include <string.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/types.h>
#include <ofono/log.h>
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/gprs-provision.h>

#include "mbpi.h"

/* Returns the list containing exactly one INTERNET and one MMS access point */
static GSList *provision_normalize_apn_list(GSList *apns)
{
	struct ofono_gprs_provision_data *internet = NULL;
	struct ofono_gprs_provision_data *mms = NULL;
	GSList *l = apns;

	while (l != NULL) {
		GSList *next = l->next;
		struct ofono_gprs_provision_data *ap = l->data;

		if (ap->type == OFONO_GPRS_CONTEXT_TYPE_INTERNET && !internet) {
			internet = ap;
		} else if (ap->type == OFONO_GPRS_CONTEXT_TYPE_MMS && !mms) {
			mms = ap;
		} else {
			/* Remove duplicate and unnecessary access points */
			DBG("Discarding APN: '%s' Name: '%s' Type: %s",
				ap->apn, ap->name, mbpi_ap_type(ap->type));
			mbpi_ap_free(ap);
			apns = g_slist_remove_link(apns, l);
		}
		l = next;
	}

	if (!internet) {
		internet = g_try_new0(struct ofono_gprs_provision_data, 1);
		if (internet) {
			internet->type = OFONO_GPRS_CONTEXT_TYPE_INTERNET;
			internet->name = g_strdup("Internet");
			internet->apn = g_strdup("internet");
			apns = g_slist_append(apns, internet);
		}
	}

	if (!mms) {
		mms = g_try_new0(struct ofono_gprs_provision_data, 1);
		if (mms) {
			mms->type = OFONO_GPRS_CONTEXT_TYPE_MMS;
			mms->name = g_strdup("MMS");
			mms->apn = g_strdup("mms");
			apns = g_slist_append(apns, mms);
		}
	}

	return apns;
}

static int provision_get_settings(const char *mcc, const char *mnc,
				const char *spn,
				struct ofono_gprs_provision_data **settings,
				int *count)
{
	GSList *l;
	GSList *apns;
	GError *error = NULL;
	int ap_count;
	int i;

	DBG("Provisioning for MCC %s, MNC %s, SPN '%s'", mcc, mnc, spn);

	/*
	 * Passing FALSE to mbpi_lookup_apn() would return
	 * an empty list if duplicates are found.
	 */
	apns = mbpi_lookup_apn(mcc, mnc, TRUE, &error);
	if (error != NULL) {
		ofono_error("%s", error->message);
		g_error_free(error);
	}

	apns = provision_normalize_apn_list(apns);
	if (apns == NULL)
		return -ENOENT;

	ap_count = g_slist_length(apns);

	DBG("Found %d APs", ap_count);

	*settings = g_try_new0(struct ofono_gprs_provision_data, ap_count);
	if (*settings == NULL) {
		ofono_error("Provisioning failed: %s", g_strerror(errno));

		for (l = apns; l; l = l->next)
			mbpi_ap_free(l->data);

		g_slist_free(apns);

		return -ENOMEM;
	}

	*count = ap_count;

	for (l = apns, i = 0; l; l = l->next, i++) {
		struct ofono_gprs_provision_data *ap = l->data;

		DBG("Name: '%s'", ap->name);
		DBG("APN: '%s'", ap->apn);
		DBG("Type: %s", mbpi_ap_type(ap->type));
		DBG("Username: '%s'", ap->username);
		DBG("Password: '%s'", ap->password);

		memcpy(*settings + i, ap,
			sizeof(struct ofono_gprs_provision_data));

		g_free(ap);
	}

	g_slist_free(apns);

	return 0;
}

static struct ofono_gprs_provision_driver provision_driver = {
	.name		= "Provisioning",
	.get_settings	= provision_get_settings
};

static int provision_init(void)
{
	return ofono_gprs_provision_driver_register(&provision_driver);
}

static void provision_exit(void)
{
	ofono_gprs_provision_driver_unregister(&provision_driver);
}

OFONO_PLUGIN_DEFINE(provision, "Provisioning Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			provision_init, provision_exit)
