/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2019-2020 Jolla Ltd.
 *  Copyright (C) 2020 Open Mobile Platform LLC.
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

#include "ofono.h"

#include <errno.h>
#include <string.h>

static GSList *dbus_access_plugins = NULL;

const char *ofono_dbus_access_intf_name(enum ofono_dbus_access_intf intf)
{
	switch (intf) {
	case OFONO_DBUS_ACCESS_INTF_MESSAGE:
		return OFONO_MESSAGE_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_MESSAGEMGR:
		return OFONO_MESSAGE_MANAGER_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_VOICECALL:
		return OFONO_VOICECALL_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_VOICECALLMGR:
		return OFONO_VOICECALL_MANAGER_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_CONNCTX:
		return OFONO_CONNECTION_CONTEXT_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_CONNMGR:
		return OFONO_CONNECTION_MANAGER_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_SIMMGR:
		return OFONO_SIM_MANAGER_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_MODEM:
		return OFONO_MODEM_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_RADIOSETTINGS:
		return OFONO_RADIO_SETTINGS_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_STK:
		return OFONO_STK_INTERFACE;
	case OFONO_DBUS_ACCESS_INTF_OEMRAW:
		return "org.ofono.OemRaw";
	case OFONO_DBUS_ACCESS_INTF_COUNT:
		break;
	}
	return NULL;
}

const char *ofono_dbus_access_method_name(enum ofono_dbus_access_intf intf,
								int method)
{
	switch (intf) {
	case OFONO_DBUS_ACCESS_INTF_MESSAGE:
		switch ((enum ofono_dbus_access_message_method)method) {
		case OFONO_DBUS_ACCESS_MESSAGE_CANCEL:
			return "Cancel";
		case OFONO_DBUS_ACCESS_MESSAGE_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_MESSAGEMGR:
		switch ((enum ofono_dbus_access_messagemgr_method)method) {
		case OFONO_DBUS_ACCESS_MESSAGEMGR_SEND_MESSAGE:
			return "SendMessage";
		case OFONO_DBUS_ACCESS_MESSAGEMGR_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_VOICECALL:
		switch ((enum ofono_dbus_access_voicecall_method)method) {
		case OFONO_DBUS_ACCESS_VOICECALL_DEFLECT:
			return "Deflect";
		case OFONO_DBUS_ACCESS_VOICECALL_HANGUP:
			return "Hangup";
		case OFONO_DBUS_ACCESS_VOICECALL_ANSWER:
			return "Answer";
		case OFONO_DBUS_ACCESS_VOICECALL_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_VOICECALLMGR:
		switch ((enum ofono_dbus_access_voicecallmgr_method)method) {
		case OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL:
			return "Dial";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_TRANSFER:
			return "Transfer";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_SWAP_CALLS:
			return "SwapCalls";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_RELEASE_AND_ANSWER:
			return "ReleaseAndAnswer";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_RELEASE_AND_SWAP:
			return "ReleaseAndSwap";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_HOLD_AND_ANSWER:
			return "HoldAndAnswer";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_HANGUP_ALL:
			return "HangupAll";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_CREATE_MULTIPARTY:
			return "CreateMultiparty";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_HANGUP_MULTIPARTY:
			return "HangupMultiparty";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_SEND_TONES:
			return "SendTones";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_REGISTER_VOICECALL_AGENT:
			return "RegisterVoicecallAgent";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_UNREGISTER_VOICECALL_AGENT:
			return "UnregisterVoicecallAgent";
		case OFONO_DBUS_ACCESS_VOICECALLMGR_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_CONNCTX:
		switch ((enum ofono_dbus_access_connctx_method)method) {
		case OFONO_DBUS_ACCESS_CONNCTX_SET_PROPERTY:
			return "SetProperty";
		case OFONO_DBUS_ACCESS_CONNCTX_PROVISION_CONTEXT:
			return "ProvisionContext";
		case OFONO_DBUS_ACCESS_CONNCTX_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_CONNMGR:
		switch ((enum ofono_dbus_access_connmgr_method)method) {
		case OFONO_DBUS_ACCESS_CONNMGR_SET_PROPERTY:
			return "SetProperty";
		case OFONO_DBUS_ACCESS_CONNMGR_DEACTIVATE_ALL:
			return "DeactivateAll";
		case OFONO_DBUS_ACCESS_CONNMGR_RESET_CONTEXTS:
			return "ResetContexts";
		case OFONO_DBUS_ACCESS_CONNMGR_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_SIMMGR:
		switch ((enum ofono_dbus_access_simmgr_method)method) {
		case OFONO_DBUS_ACCESS_SIMMGR_SET_PROPERTY:
			return "SetProperty";
		case OFONO_DBUS_ACCESS_SIMMGR_CHANGE_PIN:
			return "ChangePin";
		case OFONO_DBUS_ACCESS_SIMMGR_ENTER_PIN:
			return "EnterPin";
		case OFONO_DBUS_ACCESS_SIMMGR_RESET_PIN:
			return "ResetPin";
		case OFONO_DBUS_ACCESS_SIMMGR_LOCK_PIN:
			return "LockPin";
		case OFONO_DBUS_ACCESS_SIMMGR_UNLOCK_PIN:
			return "UnlockPin";
		case OFONO_DBUS_ACCESS_SIMMGR_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_MODEM:
		switch ((enum ofono_dbus_access_modem_method)method) {
		case OFONO_DBUS_ACCESS_MODEM_SET_PROPERTY:
			return "SetProperty";
		case OFONO_DBUS_ACCESS_MODEM_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_RADIOSETTINGS:
		switch ((enum ofono_dbus_access_radiosettings_method)method) {
		case OFONO_DBUS_ACCESS_RADIOSETTINGS_SET_PROPERTY:
			return "SetProperty";
		case OFONO_DBUS_ACCESS_RADIOSETTINGS_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_STK:
		switch ((enum ofono_dbus_access_stk_method)method) {
		case OFONO_DBUS_ACCESS_STK_REGISTER_AGENT:
			return "RegisterAgent";
		case OFONO_DBUS_ACCESS_STK_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_OEMRAW:
		switch ((enum ofono_dbus_access_oemraw_method)method) {
		case OFONO_DBUS_ACCESS_OEMRAW_SEND:
			return "Send";
		case OFONO_DBUS_ACCESS_OEMRAW_METHOD_COUNT:
			break;
		}
		break;
	case OFONO_DBUS_ACCESS_INTF_COUNT:
		break;
	}
	return NULL;
}

gboolean __ofono_dbus_access_method_allowed(const char *sender,
					enum ofono_dbus_access_intf intf,
					int method, const char *arg)
{
	GSList *l = dbus_access_plugins;

	while (l) {
		GSList *next = l->next;
		const struct ofono_dbus_access_plugin *plugin = l->data;

		switch (plugin->method_access(sender, intf, method, arg)) {
		case OFONO_DBUS_ACCESS_DENY:
			return FALSE;
		case OFONO_DBUS_ACCESS_ALLOW:
			return TRUE;
		case OFONO_DBUS_ACCESS_DONT_CARE:
			break;
		}

		l = next;
	}

	return TRUE;
}

/**
 * Returns 0 if both are equal;
 * <0 if a comes before b;
 * >0 if a comes after b.
 */
static gint ofono_dbus_access_plugin_sort(gconstpointer a, gconstpointer b)
{
	const struct ofono_dbus_access_plugin *a_plugin = a;
	const struct ofono_dbus_access_plugin *b_plugin = b;

	if (a_plugin->priority > b_plugin->priority) {
		/* a comes before b */
		return -1;
	} else if (a_plugin->priority < b_plugin->priority) {
		/* a comes after b */
		return 1;
	} else {
		/* Whatever, as long as the sort is stable */
		return strcmp(a_plugin->name, b_plugin->name);
	}
}

int ofono_dbus_access_plugin_register
			(const struct ofono_dbus_access_plugin *plugin)
{
	if (!plugin || !plugin->name) {
		return -EINVAL;
	} else if (g_slist_find(dbus_access_plugins, plugin)) {
		return -EALREADY;
	} else {
		DBG("%s", plugin->name);
		dbus_access_plugins = g_slist_insert_sorted(dbus_access_plugins,
				(void*)plugin, ofono_dbus_access_plugin_sort);
		return 0;
	}
}

void ofono_dbus_access_plugin_unregister
			(const struct ofono_dbus_access_plugin *plugin)
{
	if (plugin) {
		DBG("%s", plugin->name);
		dbus_access_plugins = g_slist_remove(dbus_access_plugins,
								plugin);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
