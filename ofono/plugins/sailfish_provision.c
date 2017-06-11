/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013-2017  Jolla Ltd.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <errno.h>
#include <string.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
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

static gint provision_match_strings(const char *s1, const char *s2)
{
	gint match = 0;

	/* Caller checks s2 for NULL */
	if (s1) {
		const gssize len1 = strlen(s1);
		const gssize len2 = strlen(s2);

		if (len1 == len2 && !strcmp(s1, s2)) {
			/* Best match ever */
			match = 3;
		} else if (g_utf8_validate(s1, len1, NULL) &&
					g_utf8_validate(s2, len2, NULL)) {
			char *d1 = g_utf8_strdown(s1, len1);
			char *d2 = g_utf8_strdown(s2, len2);

			if (len1 == len2 && !strcmp(d1, d2)) {
				/* Case insensitive match */
				match = 2;
			} else if ((len1 > len2 && strstr(d1, d2)) ||
					(len2 > len1 && strstr(d2, d1))) {
				/* Partial case insensitive match */
				match = 1;
			}

			g_free(d1);
			g_free(d2);
		}
	}

	return match;
}
static gint provision_match_spn(const struct ofono_gprs_provision_data *ap,
							const char *spn)
{
	return provision_match_strings(ap->provider_name, spn) * 4 +
				provision_match_strings(ap->name, spn);
}

static void provision_free_ap(gpointer data)
{
	mbpi_ap_free(data);
}

static gint provision_compare_ap(gconstpointer a, gconstpointer b,
							gpointer data)
{
	const struct ofono_gprs_provision_data *ap1 = a;
	const struct ofono_gprs_provision_data *ap2 = b;
	const char *spn = data;

	if (spn) {
		const gint result = provision_match_spn(ap2, spn) -
				provision_match_spn(ap1, spn);

		if (result) {
			return result;
		}
	}

	if (ap1->provider_primary && !ap2->provider_primary) {
		return -1;
	} else if (ap2->provider_primary && !ap1->provider_primary) {
		return 1;
	}

	return 0;
}

/* Picks best ap, deletes the rest. Creates one if necessary */
static GSList *provision_pick_best_ap(GSList *list, const char *spn,
	const enum ofono_gprs_proto default_proto,
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

		ap->proto = default_proto;
		ap->type = defaults->type;
		ap->name = g_strdup(defaults->name);
		ap->apn = g_strdup(defaults->apn);
		ap->auth_method = OFONO_GPRS_AUTH_METHOD_NONE;
		return g_slist_append(NULL, ap);
	}
}

/* Returns the list containing exactly one INTERNET and one MMS access point */
static GSList *provision_normalize_apn_list(GSList *apns, const char *spn)
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
		provision_pick_best_ap(internet_apns, spn,
			mbpi_default_internet_proto, &internet_defaults),
		provision_pick_best_ap(mms_apns, spn,
			mbpi_default_mms_proto, &mms_defaults));
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

	DBG("Found %d APs in MBPI", g_slist_length(apns));
	apns = provision_normalize_apn_list(apns, spn);
	ap_count = g_slist_length(apns);

	DBG("Provisioning %d APs", ap_count);
	*settings = g_new0(struct ofono_gprs_provision_data, ap_count);
	*count = ap_count;

	for (l = apns, i = 0; l; l = l->next, i++) {
		struct ofono_gprs_provision_data *ap = l->data;

		ofono_info("Name: '%s'", ap->name);
		ofono_info("  APN: '%s'", ap->apn);
		ofono_info("  Type: %s", mbpi_ap_type(ap->type));
		ofono_info("  Username: '%s'", ap->username);
		ofono_info("  Password: '%s'", ap->password);

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
	DBG("");
	return ofono_gprs_provision_driver_register(&provision_driver);
}

static void provision_exit(void)
{
	DBG("");
	ofono_gprs_provision_driver_unregister(&provision_driver);
}

OFONO_PLUGIN_DEFINE(provision, "Provisioning Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			provision_init, provision_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
