/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2019 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define OFONO_API_SUBJECT_TO_CHANGE

#include <ofono/dbus-access.h>
#include <ofono/plugin.h>
#include <ofono/log.h>

#include <dbusaccess_policy.h>
#include <dbusaccess_peer.h>

struct sailfish_access_intf {
	const char *name;
};

struct sailfish_access_intf_policy {
	const char* intf;
	int n_methods;
	DAPolicy* policy[1];
};

#define OFONO_BUS DA_BUS_SYSTEM

#define COMMON_GROUP "Common"
#define DEFAULT_POLICY "DefaultAccess"
#define DEFAULT_INTF_POLICY "*"

/* File name is external for unit testing */
const char *sailfish_access_config_file = "/etc/ofono/dbusaccess.conf";
static GHashTable* access_table = NULL;
static const char *default_access_policy = DA_POLICY_VERSION "; "
	"* = deny; "
	"group(sailfish-radio) | group(privileged) = allow";

/*
 * Configuration is loaded from /etc/ofono/dbusaccess.conf
 * If configuration is missing, default access rules are used.
 * Syntax goes like this:
 *
 * [Common]
 * DefaultAccess = <default rules for all controlled interfaces/methods>
 *
 * [InterfaceX]
 * * = <default access rules for all methods in this interface>
 * MethodY = <access rule for this method>
 */

static void sailfish_access_policy_free(gpointer user_data)
{
	da_policy_unref((DAPolicy*)user_data);
}

static void sailfish_access_load_config_intf(GKeyFile *config,
		enum ofono_dbus_access_intf intf, DAPolicy* default_policy)
{
	struct sailfish_access_intf_policy *intf_policy;
	const char *group = ofono_dbus_access_intf_name(intf);
	const char *method;
	DAPolicy *default_intf_policy = NULL;
	char *default_intf_policy_spec = g_key_file_get_string(config, group,
						DEFAULT_INTF_POLICY, NULL);
	GPtrArray *policies = g_ptr_array_new_with_free_func
						(sailfish_access_policy_free);
	int i = 0;

	/* Parse the default policy for this interface */
	if (default_intf_policy_spec) {
		default_intf_policy = da_policy_new(default_intf_policy_spec);
		if (default_intf_policy) {
			default_policy = default_intf_policy;
		} else {
			ofono_warn("Failed to parse default %s rule \"%s\"",
					group, default_intf_policy_spec);
		}
		g_free(default_intf_policy_spec);
	}

	/* Parse individual policies for each method */
	while ((method = ofono_dbus_access_method_name(intf, i++)) != NULL) {
		DAPolicy* policy;
		char *spec = g_key_file_get_string(config, group, method, NULL);

		if (spec) {
			policy = da_policy_new(spec);
			if (!policy) {
				ofono_warn("Failed to parse %s.%s rule \"%s\"",
							group, method, spec);
				policy = da_policy_ref(default_policy);
			}
		} else {
			policy = da_policy_ref(default_policy);
		}
		g_ptr_array_add(policies, policy);
		g_free(spec);
	}

	/* Allocate storage for interface policy information */
	intf_policy = g_malloc0(
		G_STRUCT_OFFSET(struct sailfish_access_intf_policy, policy) +
		sizeof(DAPolicy*) * policies->len);
	intf_policy->intf = group;
	intf_policy->n_methods = policies->len;

	for (i = 0; i < intf_policy->n_methods; i++) {
		intf_policy->policy[i] = da_policy_ref(policies->pdata[i]);
	}

	da_policy_unref(default_intf_policy);
	g_hash_table_insert(access_table, GINT_TO_POINTER(intf), intf_policy);
	g_ptr_array_free(policies, TRUE);
}

static void sailfish_access_load_config()
{
	GKeyFile *config = g_key_file_new();
	char *default_policy_spec;
	DAPolicy* default_policy;
	int i;

	/*
	 * Try to load config file, in case of error just make sure
	 * that it config is empty.
	 */
	if (g_file_test(sailfish_access_config_file, G_FILE_TEST_EXISTS)) {
		if (g_key_file_load_from_file(config,
					sailfish_access_config_file,
					G_KEY_FILE_NONE, NULL)) {
			DBG("Loading D-Bus access rules from %s",
						sailfish_access_config_file);
		} else {
			g_key_file_unref(config);
			config = g_key_file_new();
		}
	}

	default_policy_spec = g_key_file_get_string(config, COMMON_GROUP,
						DEFAULT_POLICY, NULL);
	default_policy = da_policy_new(default_policy_spec);

	if (!default_policy) {
		default_policy = da_policy_new(default_access_policy);
		if (!default_policy) {
			ofono_warn("Failed to parse default D-Bus policy "
						"\"%s\" (missing group?)",
						default_access_policy);
		}
	}

	for (i = 0; i < OFONO_DBUS_ACCESS_INTF_COUNT; i++) {
		sailfish_access_load_config_intf(config, i, default_policy);
	}

	da_policy_unref(default_policy);
	g_free(default_policy_spec);
	g_key_file_unref(config);
}

static void sailfish_access_intf_free(gpointer user_data)
{
	struct sailfish_access_intf_policy* intf = user_data;
	int i;

	for (i = 0; i < intf->n_methods; i++) {
		da_policy_unref(intf->policy[i]);
	}
	g_free(intf);
}

static enum ofono_dbus_access sailfish_access_method_access(const char *sender,
				enum ofono_dbus_access_intf intf,
				int method, const char *arg)
{
	struct sailfish_access_intf_policy *intf_policy = g_hash_table_lookup
		(access_table, GINT_TO_POINTER(intf));

	if (intf_policy && method >= 0 && method < intf_policy->n_methods) {
		DAPeer *peer = da_peer_get(OFONO_BUS, sender);

		if (peer) {
			switch (da_policy_check(intf_policy->policy[method],
				&peer->cred, 0, arg, DA_ACCESS_ALLOW)) {
			case DA_ACCESS_ALLOW:
				return OFONO_DBUS_ACCESS_ALLOW;
			case DA_ACCESS_DENY:
				return OFONO_DBUS_ACCESS_DENY;
			}
		} else {
			/*
			 * Deny access to unknown peers. Those are
			 * already gone from the bus and won't be
			 * able to receive our reply anyway.
			 */
			return OFONO_DBUS_ACCESS_DENY;
		}
	}
	return OFONO_DBUS_ACCESS_DONT_CARE;
}

static const struct ofono_dbus_access_plugin sailfish_access_plugin = {
	.name = "Sailfish D-Bus access",
	.priority = OFONO_DBUS_ACCESS_PRIORITY_DEFAULT,
	.method_access = sailfish_access_method_access
};

static int sailfish_access_init(void)
{
	int ret;

	DBG("");
	ret = ofono_dbus_access_plugin_register(&sailfish_access_plugin);
	if (ret == 0) {
		access_table = g_hash_table_new_full(g_direct_hash,
			g_direct_equal, NULL, sailfish_access_intf_free);
		sailfish_access_load_config();
	}
	return ret;
}

static void sailfish_access_exit(void)
{
	DBG("");
	ofono_dbus_access_plugin_unregister(&sailfish_access_plugin);
	da_peer_flush(OFONO_BUS, NULL);
	if (access_table) {
		g_hash_table_destroy(access_table);
		access_table = NULL;
	}
}

OFONO_PLUGIN_DEFINE(sailfish_access, "Sailfish D-Bus access plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			sailfish_access_init, sailfish_access_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
