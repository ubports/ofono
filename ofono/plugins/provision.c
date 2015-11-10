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

#define _GNU_SOURCE
#include <errno.h>
#include <string.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/types.h>
#include <ofono/log.h>
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/gprs-provision.h>

#include "provision.h"
#include "mbpi.h"

struct provision_ap_defaults {
	enum ofono_gprs_context_type type;
	const char *name;
	const char *apn;
};

static gboolean provision_match_name(const struct ofono_gprs_provision_data *ap,
							const char* spn)
{
	return (ap->provider_name && strcasestr(ap->provider_name, spn)) ||
		(ap->name && strcasestr(ap->name, spn)) ||
		(ap->apn && strcasestr(ap->apn, spn));
}

static void provision_free_ap(gpointer data)
{
	mbpi_ap_free(data);
}

static gint provision_compare_ap(gconstpointer a, gconstpointer b, gpointer data)
{
	const struct ofono_gprs_provision_data *ap1 = a;
	const struct ofono_gprs_provision_data *ap2 = b;
	const char* spn = data;

	if (spn) {
		const gboolean match1 = provision_match_name(ap1, spn);
		const gboolean match2 = provision_match_name(ap2, spn);
		if (match1 && !match2) {
			return -1;
		} else if (match2 && !match1) {
			return 1;
		}
	}

	if (ap1->provider_primary && !ap2->provider_primary) {
		return -1;
	} else if (ap2->provider_primary && !ap1->provider_primary) {
		return 1;
	} else {
		return 0;
	}
}

/* Picks best ap, deletes the rest. Creates one if necessary */
static GSList *provision_pick_best_ap(GSList *list, const char* spn,
	const struct provision_ap_defaults *defaults)
{
	/* Sort the list */
	list = g_slist_sort_with_data(list, provision_compare_ap, (void*)spn);
	if (list) {
		/* Pick the best one, delete the rest */
		GSList *best = list;
		g_slist_free_full(g_slist_remove_link(list, best),
							provision_free_ap);
		return best;
	} else {
		/* or create one from the default data */
		struct ofono_gprs_provision_data *ap =
			g_new0(struct ofono_gprs_provision_data, 1);

		ap->type = defaults->type;
		ap->name = g_strdup(defaults->name);
		ap->apn = g_strdup(defaults->apn);
		return g_slist_append(NULL, ap);
	}
}

/* Returns the list containing exactly one INTERNET and one MMS access point */
static GSList *provision_normalize_apn_list(GSList *apns, const char* spn)
{
	static const struct provision_ap_defaults internet_defaults =
		{ OFONO_GPRS_CONTEXT_TYPE_INTERNET, "Internet", "internet" };
	static const struct provision_ap_defaults mms_defaults =
		{ OFONO_GPRS_CONTEXT_TYPE_MMS, "MMS", "mms" };

	GSList *internet_apns = NULL;
	GSList *mms_apns = NULL;

	/* Split internet and mms apns, delete all others */
	while (apns) {
		GSList *link = apns;
		struct ofono_gprs_provision_data *ap = link->data;

		apns = g_slist_remove_link(apns, link);
		if (ap->type == OFONO_GPRS_CONTEXT_TYPE_INTERNET) {
			internet_apns = g_slist_concat(internet_apns, link);
		} else if (ap->type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
			mms_apns = g_slist_concat(mms_apns, link);
		} else {
			g_slist_free_full(link, provision_free_ap);
		}
	}

	/* Pick the best ap of each type and concatenate them */
	return g_slist_concat(
		provision_pick_best_ap(internet_apns, spn, &internet_defaults),
		provision_pick_best_ap(mms_apns, spn, &mms_defaults));
}

int provision_get_settings(const char *mcc, const char *mnc,
				const char *spn,
				struct ofono_gprs_provision_data **settings,
				int *count)
{
	GSList *l;
	GSList *apns;
	GError *error = NULL;
	int ap_count;
	int i;

	ofono_info("Provisioning for MCC %s, MNC %s, SPN '%s'", mcc, mnc, spn);

	/*
	 * Passing FALSE to mbpi_lookup_apn() would return
	 * an empty list if duplicates are found.
	 */
	apns = mbpi_lookup_apn(mcc, mnc, TRUE, &error);
	if (error != NULL) {
		ofono_error("%s", error->message);
		g_error_free(error);
	}

	ofono_info("Found %d APs in MBPI", g_slist_length(apns));
	apns = provision_normalize_apn_list(apns, spn);
	if (apns == NULL)
		return -ENOENT;

	ap_count = g_slist_length(apns);

	ofono_info("Provisioning %d APs", ap_count);

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

		ofono_info("Name: '%s'", ap->name);
		ofono_info("APN: '%s'", ap->apn);
		ofono_info("Type: %s", mbpi_ap_type(ap->type));
		ofono_info("Username: '%s'", ap->username);
		ofono_info("Password: '%s'", ap->password);

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
