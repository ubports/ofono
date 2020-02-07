/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2016  Intel Corporation. All rights reserved.
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

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <ofono.h>
#include <simutil.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/sim.h>
#include <ofono/dbus.h>
#include <gdbus.h>

#define SIM_EFACL_FILEID	0x6f57

#define ALLOWED_ACCESS_POINTS_INTERFACE "org.ofono.AllowedAccessPoints"

guint modemwatch_id;
GSList *context_list;

struct allowed_apns_ctx {
	guint simwatch_id;
	guint atomwatch_id;
	struct ofono_modem *modem;
	struct ofono_sim *sim;
	struct ofono_sim_context *sim_context;
	DBusMessage *pending;
	DBusMessage *reply;
	bool registered;
};

static void context_destroy(gpointer data)
{
	struct allowed_apns_ctx *ctx = data;

	if (ctx->simwatch_id)
		ofono_sim_remove_state_watch(ctx->sim,
					ctx->simwatch_id);

	if (ctx->atomwatch_id)
		__ofono_modem_remove_atom_watch(ctx->modem,
					ctx->atomwatch_id);

	if (ctx->sim_context)
		ofono_sim_context_free(ctx->sim_context);

	g_free(ctx);
}

static void atomwatch_destroy(gpointer data)
{
	struct allowed_apns_ctx *ctx = data;

	ctx->atomwatch_id = 0;
}

static void sim_acl_read_cb(int ok, int total_length, int record,
			const unsigned char *data, int record_length,
			void *userdata)
{
	struct allowed_apns_ctx *ctx = userdata;
	DBusMessage *reply = ctx->reply;
	DBusMessageIter iter;
	DBusMessageIter array;
	struct simple_tlv_iter tlv_iter;
	char *apn;

	if (!ok) {
		reply = __ofono_error_failed(ctx->pending);
		__ofono_dbus_pending_reply(&ctx->pending, reply);
		return;
	}

	reply = dbus_message_new_method_return(ctx->pending);
	if (reply == NULL)
		return;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_STRING_AS_STRING,
					&array);

	if (data[0] == 0)
		goto done;

	simple_tlv_iter_init(&tlv_iter, &data[1], total_length - 1);

	while (simple_tlv_iter_next(&tlv_iter)) {
		if (simple_tlv_iter_get_tag(&tlv_iter) != 0xDD)
			continue;

		apn = g_strndup(
			(char *) simple_tlv_iter_get_data(&tlv_iter),
			simple_tlv_iter_get_length(&tlv_iter));

		dbus_message_iter_append_basic(&array,
					DBUS_TYPE_STRING,
					&apn);

		g_free(apn);
	}

done:
	dbus_message_iter_close_container(&iter, &array);

	__ofono_dbus_pending_reply(&ctx->pending, reply);
}

static DBusMessage *get_allowed_apns(DBusConnection *conn,
			DBusMessage *msg, void *data)
{
	struct allowed_apns_ctx *ctx = data;

	if (ctx->pending)
		return __ofono_error_busy(msg);

	ctx->pending = dbus_message_ref(msg);

	ofono_sim_read(ctx->sim_context, SIM_EFACL_FILEID,
		OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
		sim_acl_read_cb, ctx);

	return NULL;
}

static const GDBusMethodTable allowed_apns_methods[] = {
	{ GDBUS_ASYNC_METHOD("GetAllowedAccessPoints",
			NULL, GDBUS_ARGS({ "apnlist", "as" }),
			get_allowed_apns) },
	{ }
};

static void sim_state_watch(enum ofono_sim_state new_state, void *data)
{
	struct allowed_apns_ctx *ctx = data;
	DBusConnection *conn = ofono_dbus_get_connection();

	if (new_state != OFONO_SIM_STATE_READY) {
		if (!ctx->registered)
			return;

		g_dbus_unregister_interface(conn,
				ofono_modem_get_path(ctx->modem),
				ALLOWED_ACCESS_POINTS_INTERFACE);

		ofono_modem_remove_interface(ctx->modem,
				ALLOWED_ACCESS_POINTS_INTERFACE);

		ctx->registered = false;
		return;
	}

	if (!g_dbus_register_interface(conn,
				ofono_modem_get_path(ctx->modem),
				ALLOWED_ACCESS_POINTS_INTERFACE,
				allowed_apns_methods, NULL, NULL,
				ctx, NULL)) {
		ofono_error("Cannot create %s Interface\n",
			ALLOWED_ACCESS_POINTS_INTERFACE);

		return;
	}

	ctx->registered = true;
	ofono_modem_add_interface(ctx->modem,
			ALLOWED_ACCESS_POINTS_INTERFACE);
}

static void sim_watch(struct ofono_atom *atom,
		enum ofono_atom_watch_condition cond,
		void *data)
{
	struct allowed_apns_ctx *ctx = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		if (ctx->simwatch_id) {
			sim_state_watch(OFONO_SIM_STATE_NOT_PRESENT, data);
			ofono_sim_remove_state_watch(ctx->sim, ctx->simwatch_id);
			ctx->simwatch_id = 0;
		}

		if (ctx->sim_context) {
			ofono_sim_context_free(ctx->sim_context);
			ctx->sim_context = NULL;
		}

		return;
	}

	ctx->sim = __ofono_atom_get_data(atom);

	ctx->sim_context = ofono_sim_context_create(ctx->sim);

	ctx->simwatch_id = ofono_sim_add_state_watch(ctx->sim,
						sim_state_watch,
						ctx, NULL);
}

static gint context_list_modem_compare(gconstpointer data1,
				gconstpointer data2)
{
	const struct allowed_apns_ctx *ctx = data1;
	const struct ofono_modem *modem = data2;
	return (ctx->modem == modem);
}

static void modem_watch(struct ofono_modem *modem,
		gboolean added, void *userdata)
{
	struct allowed_apns_ctx *ctx;
	GSList *l;

	if (added == FALSE) {
		l = g_slist_find_custom(context_list,
				modem, context_list_modem_compare);

		if (l) {
			ctx = l->data;
			context_destroy(ctx);
			context_list = g_slist_delete_link(context_list, l);
		}

		return;
	}

	ctx = g_try_new0(struct allowed_apns_ctx, 1);
	if (ctx == NULL)
		return;

	context_list = g_slist_prepend(context_list, ctx);

	ctx->modem = modem;

	ctx->atomwatch_id = __ofono_modem_add_atom_watch(ctx->modem,
						OFONO_ATOM_TYPE_SIM,
						sim_watch, ctx,
						atomwatch_destroy);
}

static void call_modemwatch(struct ofono_modem *modem, void *userdata)
{
	modem_watch(modem, TRUE, userdata);
}

static int allowed_apns_init(void)
{
	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);

	__ofono_modem_foreach(call_modemwatch, NULL);

	return 0;
}

static void allowed_apns_exit(void)
{
	__ofono_modemwatch_remove(modemwatch_id);

	g_slist_free_full(context_list, context_destroy);
}

OFONO_PLUGIN_DEFINE(allowed_apns, "Plugin to read EFACL from SIM",
		VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
		allowed_apns_init, allowed_apns_exit)
