/*
 *  Copyright (C) 2013-2017 Jolla Ltd.
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

#include <gutil_inotify.h>
#include <sys/inotify.h>
#include <wspcodec.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono.h>
#include <plugin.h>

/*
 * Push forwarder plugin is looking for configuration files in
 * /etc/ofono/push_forwarder.d directory. Confiration files are
 * glib key files that look like this:
 *
 *   [Jolla MMS Handler]
 *   ContentType = application/vnd.wap.mms-message
 *   Interface = com.jolla.MmsEngine.
 *   Service = com.jolla.MmsEngine
 *   Method = HandlePush
 *   Path = /
 *
 * Only files with .conf suffix are loaded. In addition to the keys
 * from the above example, SourcePort and DestinationPort port keys
 * are supported. All other keys are ignored. One file may describe
 * several push handlers. See pf_parse_config() function for details.
 *
 * When push fowarder receives a WAP push, it goes through the list
 * of registered handlers and invokes all of them that match content
 * type and/or port numbers. The rest is up to the D-Bus service
 * handling the call.
 */

#define PF_CONFIG_DIR CONFIGDIR "/push_forwarder.d"

struct pf_modem {
	struct ofono_modem *modem;
	struct ofono_sms *sms;
	struct ofono_sim *sim;
	unsigned int sim_watch_id;
	unsigned int sms_watch_id;
	unsigned int push_watch_id;
};

struct push_datagram_handler {
	char *name;
	char *content_type;
	char *interface;
	char *service;
	char *method;
	char *path;
	int dst_port;
	int src_port;
};

static GSList *handlers;
static GSList *modems;
static unsigned int modem_watch_id;
static GUtilInotifyWatchCallback *inotify_cb;

static void pf_notify_handler(struct push_datagram_handler *h,
		const char *imsi, const char *from, const struct tm *remote,
		const struct tm *local, int dst, int src,
		const char *ct, const void *data, unsigned int len)
{
	struct tm remote_tm = *remote;
	struct tm local_tm = *local;
	dbus_uint32_t remote_time_arg = mktime(&remote_tm);
	dbus_uint32_t local_time_arg = mktime(&local_tm);
	dbus_int32_t dst_arg = dst;
	dbus_int32_t src_arg = src;
	DBusMessageIter iter, array;
	DBusMessage *msg = dbus_message_new_method_call(h->service,
			h->path, h->interface, h->method);

	dbus_message_append_args(msg,
				 DBUS_TYPE_STRING, &imsi,
				 DBUS_TYPE_STRING, &from,
				 DBUS_TYPE_UINT32, &remote_time_arg,
				 DBUS_TYPE_UINT32, &local_time_arg,
				 DBUS_TYPE_INT32,  &dst_arg,
				 DBUS_TYPE_INT32,  &src_arg,
				 DBUS_TYPE_STRING, &ct,
				 DBUS_TYPE_INVALID);
	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_BYTE_AS_STRING, &array);
	dbus_message_iter_append_fixed_array(&array,
						DBUS_TYPE_BYTE, &data, len);
	dbus_message_iter_close_container(&iter, &array);
	dbus_message_set_no_reply(msg, TRUE);
	dbus_connection_send(ofono_dbus_get_connection(), msg, NULL);
	dbus_message_unref(msg);
}

static gboolean pf_match_port(int port, int expected_port)
{
	if (expected_port < 0)
		return TRUE;

	if (expected_port == port)
		return TRUE;

	return FALSE;
}

static gboolean pf_match_handler(struct push_datagram_handler *h,
	const char *ct, int dst, int src)
{
	if (pf_match_port(dst, h->dst_port) == FALSE)
		return FALSE;

	if (pf_match_port(src, h->src_port) == FALSE)
		return FALSE;

	if (h->content_type == NULL)
		return TRUE;

	if (strcmp(h->content_type, ct) == 0)
		return TRUE;

	return FALSE;
}

static void pf_handle_datagram(const char *from,
		const struct tm *remote, const struct tm *local, int dst,
		int src, const unsigned char *buffer, unsigned int len,
		void *userdata)
{
	struct pf_modem *pm = userdata;
	guint remain;
	const guint8 *data;
	unsigned int hdrlen;
	unsigned int off;
	const void *ct;
	const char *imsi;
	GSList *link;

	DBG("received push of size: %u", len);

	if (pm->sim == NULL)
		return;

	imsi = ofono_sim_get_imsi(pm->sim);
	if (len < 3)
		return;

	if (buffer[1] != 6)
		return;

	remain = len - 2;
	data = buffer + 2;

	if (wsp_decode_uintvar(data, remain, &hdrlen, &off) == FALSE)
		return;

	if ((off + hdrlen) > remain)
		return;

	data += off;
	remain -= off;

	DBG("  WAP header %u bytes", hdrlen);

	if (wsp_decode_content_type(data, hdrlen, &ct, &off, NULL) == FALSE)
		return;

	data += hdrlen;
	remain -= hdrlen;

	DBG("  content type %s", (char *)ct);
	DBG("  imsi %s", imsi);
	DBG("  data size %u", remain);

	link = handlers;

	while (link) {
		struct push_datagram_handler *h = link->data;

		if (pf_match_handler(h, ct, dst, src) != FALSE) {
			DBG("notifying %s", h->name);
			pf_notify_handler(h, imsi, from, remote, local, dst,
					src, ct, data, remain);
		}
		link = link->next;
	}
}

static void pf_sms_watch(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *userdata)
{
	struct pf_modem *pm = userdata;

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		DBG("registered");
		pm->sms = __ofono_atom_get_data(atom);
		pm->push_watch_id = __ofono_sms_datagram_watch_add(pm->sms,
			pf_handle_datagram, -1, -1, pm, NULL);
	} else if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		DBG("unregistered");
		pm->sms = NULL;
		pm->push_watch_id = 0;
	}
}

static void pf_sms_watch_done(void *userdata)
{
	struct pf_modem *pm = userdata;

	pm->sms_watch_id = 0;
}

static void pf_sim_watch(struct ofono_atom *atom,
			enum ofono_atom_watch_condition cond, void *userdata)
{
	struct pf_modem *pm = userdata;

	if (cond == OFONO_ATOM_WATCH_CONDITION_REGISTERED) {
		DBG("registered");
		pm->sim = __ofono_atom_get_data(atom);
	} else if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		DBG("unregistered");
		pm->sim = NULL;
	}
}

static void pf_sim_watch_done(void *userdata)
{
	struct pf_modem *pm = userdata;

	pm->sim_watch_id = 0;
}

static void pf_free_modem(struct pf_modem *pm)
{
	if (pm == NULL)
		return;

	if (pm->push_watch_id != 0)
		__ofono_sms_datagram_watch_remove(pm->sms, pm->push_watch_id);

	if (pm->sim_watch_id != 0)
		__ofono_modem_remove_atom_watch(pm->modem, pm->sim_watch_id);

	if (pm->sms_watch_id != 0)
		__ofono_modem_remove_atom_watch(pm->modem, pm->sms_watch_id);

	g_free(pm);
}

static void pf_modem_watch(struct ofono_modem *modem,
						gboolean added, void *userdata)
{
	DBG("modem: %p, added: %d", modem, added);
	if (added != FALSE) {
		struct pf_modem *pm;

		pm = g_new0(struct pf_modem, 1);
		pm->modem = modem;
		pm->sms_watch_id = __ofono_modem_add_atom_watch(modem,
			OFONO_ATOM_TYPE_SMS, pf_sms_watch, pm,
			pf_sms_watch_done);
		pm->sim_watch_id = __ofono_modem_add_atom_watch(modem,
			OFONO_ATOM_TYPE_SIM, pf_sim_watch, pm,
			pf_sim_watch_done);
		modems = g_slist_append(modems, pm);
	} else {
		GSList *link = modems;

		while (link) {
			struct pf_modem *pm = link->data;

			if (pm->modem == modem) {
				modems = g_slist_delete_link(modems, link);
				pf_free_modem(pm);
				break;
			}
			link = link->next;
		}
	}
}

static void pf_modem_init(struct ofono_modem *modem,
						void *userdata)
{
	pf_modem_watch(modem, TRUE, NULL);
}

static void pf_free_handler(void *data)
{
	struct push_datagram_handler *h = data;

	g_free(h->content_type);
	g_free(h->interface);
	g_free(h->service);
	g_free(h->method);
	g_free(h->path);
	g_free(h->name);
	g_free(h);
}

static void pf_parse_handler(GKeyFile *conf, const char *g)
{
	GError *err = NULL;
	struct push_datagram_handler *h;
	char *interface;
	char *service;
	char *method;
	char *path;

	interface = g_key_file_get_string(conf, g, "Interface", NULL);
	if (interface == NULL)
		goto no_interface;

	service = g_key_file_get_string(conf, g, "Service", NULL);
	if (service == NULL)
		goto no_service;

	method = g_key_file_get_string(conf, g, "Method", NULL);
	if (method == NULL)
		goto no_method;

	path = g_key_file_get_string(conf, g, "Path", NULL);
	if (path == NULL)
		goto no_path;

	h = g_new0(struct push_datagram_handler, 1);
	h->name = g_strdup(g);
	h->interface = interface;
	h->service = service;
	h->method = method;
	h->path = path;
	h->content_type = g_key_file_get_string(conf, g, "ContentType", NULL);
	h->dst_port = g_key_file_get_integer(conf, g, "DestinationPort", &err);
	if (h->dst_port == 0 && err != NULL) {
		h->dst_port = -1;
		g_error_free(err);
		err = NULL;
	}
	h->src_port = g_key_file_get_integer(conf, g, "SourcePort", &err);
	if (h->src_port == 0 && err != NULL) {
		h->src_port = -1;
		g_error_free(err);
		err = NULL;
	}
	DBG("registered %s", h->name);
	if (h->content_type != NULL)
		DBG("  ContentType: %s", h->content_type);
	if (h->dst_port >= 0)
		DBG("  DestinationPort: %d", h->dst_port);
	if (h->src_port >= 0)
		DBG("  SourcePort: %d", h->src_port);
	DBG("  Interface: %s", interface);
	DBG("  Service: %s", service);
	DBG("  Method: %s", method);
	DBG("  Path: %s", path);
	handlers = g_slist_append(handlers, h);
	return;

no_path:
	g_free(method);

no_method:
	g_free(service);

no_service:
	g_free(interface);

no_interface:
	return;
}

static void pf_parse_config(void)
{
	GDir *dir;
	const gchar *file;

	g_slist_free_full(handlers, pf_free_handler);
	handlers = NULL;

	dir = g_dir_open(PF_CONFIG_DIR, 0, NULL);
	if (dir == NULL) {
		DBG(PF_CONFIG_DIR " not found.");
		return;
	}

	DBG("loading configuration from " PF_CONFIG_DIR);
	while ((file = g_dir_read_name(dir)) != NULL) {
		GError *err;
		GKeyFile *conf;
		char *path;

		if (g_str_has_suffix(file, ".conf") == FALSE)
			continue;

		err = NULL;
		conf = g_key_file_new();
		path = g_strconcat(PF_CONFIG_DIR "/", file, NULL);
		DBG("reading %s", file);

		if (g_key_file_load_from_file(conf, path, 0, &err) != FALSE) {
			gsize i, n;
			char **names = g_key_file_get_groups(conf, &n);

			for (i = 0; i < n; i++)
				pf_parse_handler(conf, names[i]);
			g_strfreev(names);
		} else {
			ofono_warn("%s", err->message);
			g_error_free(err);
		}

		g_key_file_free(conf);
		g_free(path);
	}

	g_dir_close(dir);
}

static void pf_inotify(GUtilInotifyWatch *watch, guint mask, guint cookie,
					const char *name, void *user_data)
{
	DBG("'%s' changed (0x%04x)", name, mask);
	pf_parse_config();
}

static int pf_plugin_init(void)
{
	DBG("");
	pf_parse_config();
	modem_watch_id = __ofono_modemwatch_add(pf_modem_watch, NULL, NULL);
	__ofono_modem_foreach(pf_modem_init, NULL);
	inotify_cb = gutil_inotify_watch_callback_new(PF_CONFIG_DIR,
		IN_CLOSE_WRITE | IN_DELETE | IN_MOVE, pf_inotify, NULL);
	return 0;
}

static void pf_plugin_exit(void)
{
	DBG("");
	__ofono_modemwatch_remove(modem_watch_id);
	modem_watch_id = 0;
	g_slist_free_full(modems, (GDestroyNotify)pf_free_modem);
	modems = NULL;
	g_slist_free_full(handlers, pf_free_handler);
	handlers = NULL;
	gutil_inotify_watch_callback_free(inotify_cb);
	inotify_cb = NULL;
}

OFONO_PLUGIN_DEFINE(pushforwarder, "Push Forwarder Plugin", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, pf_plugin_init,
		pf_plugin_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
