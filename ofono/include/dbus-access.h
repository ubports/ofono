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

#ifndef __OFONO_DBUS_ACCESS_H
#define __OFONO_DBUS_ACCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

enum ofono_dbus_access {
	OFONO_DBUS_ACCESS_DENY,       /* Deny access */
	OFONO_DBUS_ACCESS_ALLOW,      /* Allow access */
	OFONO_DBUS_ACCESS_DONT_CARE   /* No decision */
};

enum ofono_dbus_access_intf {
	OFONO_DBUS_ACCESS_INTF_MESSAGE,       /* org.ofono.Message */
	OFONO_DBUS_ACCESS_INTF_MESSAGEMGR,    /* org.ofono.MessageManager */
	OFONO_DBUS_ACCESS_INTF_VOICECALL,     /* org.ofono.VoiceCall */
	OFONO_DBUS_ACCESS_INTF_VOICECALLMGR,  /* org.ofono.VoiceCallManager */
	OFONO_DBUS_ACCESS_INTF_CONNCTX,       /* org.ofono.ConnectionContext */
	OFONO_DBUS_ACCESS_INTF_CONNMGR,       /* org.ofono.ConnectionManager */
	OFONO_DBUS_ACCESS_INTF_SIMMGR,        /* org.ofono.SimManager */
	OFONO_DBUS_ACCESS_INTF_MODEM,         /* org.ofono.Modem */
	OFONO_DBUS_ACCESS_INTF_RADIOSETTINGS, /* org.ofono.RadioSettings */
	OFONO_DBUS_ACCESS_INTF_STK,           /* org.ofono.SimToolkit */
	OFONO_DBUS_ACCESS_INTF_OEMRAW,        /* org.ofono.OemRaw */
	OFONO_DBUS_ACCESS_INTF_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_MESSAGE */
enum ofono_dbus_access_message_method {
	OFONO_DBUS_ACCESS_MESSAGE_CANCEL,
	OFONO_DBUS_ACCESS_MESSAGE_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_MESSAGEMGR */
enum ofono_dbus_access_messagemgr_method {
	OFONO_DBUS_ACCESS_MESSAGEMGR_SEND_MESSAGE,
	OFONO_DBUS_ACCESS_MESSAGEMGR_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_VOICECALL */
enum ofono_dbus_access_voicecall_method {
	OFONO_DBUS_ACCESS_VOICECALL_DEFLECT,
	OFONO_DBUS_ACCESS_VOICECALL_HANGUP,
	OFONO_DBUS_ACCESS_VOICECALL_ANSWER,
	OFONO_DBUS_ACCESS_VOICECALL_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_VOICECALLMGR */
enum ofono_dbus_access_voicecallmgr_method {
	OFONO_DBUS_ACCESS_VOICECALLMGR_DIAL,
	OFONO_DBUS_ACCESS_VOICECALLMGR_TRANSFER,
	OFONO_DBUS_ACCESS_VOICECALLMGR_SWAP_CALLS,
	OFONO_DBUS_ACCESS_VOICECALLMGR_RELEASE_AND_ANSWER,
	OFONO_DBUS_ACCESS_VOICECALLMGR_RELEASE_AND_SWAP,
	OFONO_DBUS_ACCESS_VOICECALLMGR_HOLD_AND_ANSWER,
	OFONO_DBUS_ACCESS_VOICECALLMGR_HANGUP_ALL,
	OFONO_DBUS_ACCESS_VOICECALLMGR_CREATE_MULTIPARTY,
	OFONO_DBUS_ACCESS_VOICECALLMGR_HANGUP_MULTIPARTY,
	OFONO_DBUS_ACCESS_VOICECALLMGR_SEND_TONES,
	OFONO_DBUS_ACCESS_VOICECALLMGR_REGISTER_VOICECALL_AGENT,
	OFONO_DBUS_ACCESS_VOICECALLMGR_UNREGISTER_VOICECALL_AGENT,
	OFONO_DBUS_ACCESS_VOICECALLMGR_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_CONNCTX */
enum ofono_dbus_access_connctx_method {
	OFONO_DBUS_ACCESS_CONNCTX_SET_PROPERTY,
	OFONO_DBUS_ACCESS_CONNCTX_PROVISION_CONTEXT,
	OFONO_DBUS_ACCESS_CONNCTX_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_CONNMGR */
enum ofono_dbus_access_connmgr_method {
	OFONO_DBUS_ACCESS_CONNMGR_SET_PROPERTY,
	OFONO_DBUS_ACCESS_CONNMGR_DEACTIVATE_ALL,
	OFONO_DBUS_ACCESS_CONNMGR_RESET_CONTEXTS,
	OFONO_DBUS_ACCESS_CONNMGR_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_SIMMGR */
enum ofono_dbus_access_simmgr_method {
	OFONO_DBUS_ACCESS_SIMMGR_SET_PROPERTY,
	OFONO_DBUS_ACCESS_SIMMGR_CHANGE_PIN,
	OFONO_DBUS_ACCESS_SIMMGR_ENTER_PIN,
	OFONO_DBUS_ACCESS_SIMMGR_RESET_PIN,
	OFONO_DBUS_ACCESS_SIMMGR_LOCK_PIN,
	OFONO_DBUS_ACCESS_SIMMGR_UNLOCK_PIN,
	OFONO_DBUS_ACCESS_SIMMGR_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_MODEM */
enum ofono_dbus_access_modem_method {
	OFONO_DBUS_ACCESS_MODEM_SET_PROPERTY,
	OFONO_DBUS_ACCESS_MODEM_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_RADIOSETTINGS */
enum ofono_dbus_access_radiosettings_method {
	OFONO_DBUS_ACCESS_RADIOSETTINGS_SET_PROPERTY,
	OFONO_DBUS_ACCESS_RADIOSETTINGS_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_STK */
enum ofono_dbus_access_stk_method {
	OFONO_DBUS_ACCESS_STK_REGISTER_AGENT,
	OFONO_DBUS_ACCESS_STK_METHOD_COUNT
};

/* OFONO_DBUS_ACCESS_INTF_OEMRAW */
enum ofono_dbus_access_oemraw_method {
	OFONO_DBUS_ACCESS_OEMRAW_SEND,
	OFONO_DBUS_ACCESS_OEMRAW_METHOD_COUNT
};

#define OFONO_DBUS_ACCESS_PRIORITY_LOW     (-100)
#define OFONO_DBUS_ACCESS_PRIORITY_DEFAULT (0)
#define OFONO_DBUS_ACCESS_PRIORITY_HIGH    (100)

struct ofono_dbus_access_plugin {
	const char *name;
	int priority;
	enum ofono_dbus_access (*method_access)(const char *sender,
				enum ofono_dbus_access_intf intf,
				int method, const char *arg);

	void (*_reserved[10])(void);

	/* api_level will remain zero (and ignored) until we run out of
	 * the above placeholders. */
	int api_level;
};

int ofono_dbus_access_plugin_register
			(const struct ofono_dbus_access_plugin *plugin);
void ofono_dbus_access_plugin_unregister
			(const struct ofono_dbus_access_plugin *plugin);

const char *ofono_dbus_access_intf_name(enum ofono_dbus_access_intf intf);
const char *ofono_dbus_access_method_name(enum ofono_dbus_access_intf intf,
								int method);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_DBUS_ACCESS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
