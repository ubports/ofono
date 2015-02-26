/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2014 Canonical Ltd.
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
#include <ctype.h>
#include <stdlib.h>

#include <glib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/inotify.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>

#include "rildev.h"

#define EVENT_SIZE (sizeof(struct inotify_event))
 /*
  * As a best guess use a buffer size of 100 inotify events.
  * NAME_MAX+1 from inotify documentation.
  */
#define IBUF_LEN (100*(EVENT_SIZE + NAME_MAX + 1))

static int inotify_fd = -1;
static int inotify_watch_id = -1;
static guint inotify_watch_source_id;
static GIOChannel *inotify_watch_channel;

static GSList *modem_list;
static int watch_for_rild_socket(void);
static void detect_rild(void);

static struct ofono_modem *find_ril_modem(int slot)
{
	GSList *list;

	for (list = modem_list; list; list = list->next) {
		struct ofono_modem *modem = list->data;
		int ril_slot = ofono_modem_get_integer(modem, "Slot");

		if (ril_slot == slot)
			return modem;
	}

	return NULL;
}

static void remove_watchers(void)
{
	DBG("");
	if (inotify_watch_channel == NULL)
		return;

	g_source_remove(inotify_watch_source_id);
	inotify_watch_source_id = 0;
	g_io_channel_unref(inotify_watch_channel);
	inotify_watch_channel = NULL;
	inotify_rm_watch(inotify_fd, inotify_watch_id);
	inotify_watch_id = -1;
	close(inotify_fd);
	inotify_fd = -1;
}

/* Removes a RIL modem and initiates a sequence to create a new one */
void ril_modem_remove(struct ofono_modem *modem)
{
	DBG("modem: %p", modem);
	struct ofono_modem *list_modem;
	int slot = -1;
	list_modem = NULL;

	if (modem)
		slot = ofono_modem_get_integer(modem, "Slot");

	if (slot >= 0)
		list_modem = find_ril_modem(slot);

	if (list_modem) {
		ofono_modem_remove(modem);
		modem_list = g_slist_remove(modem_list, list_modem);
	}

	detect_rild();
}

/* return: 0 if successful or modem already exists, otherwise and error */
static int create_rilmodem(const char *ril_type, int slot)
{
	struct ofono_modem *modem;
	char dev_name[64];
	int retval;
	DBG("");
	snprintf(dev_name, sizeof(dev_name), "ril_%d", slot);

	/* Check that not created already */
	if (find_ril_modem(slot))
		return 0;

	/* Currently there is only one ril implementation, create always */
	modem = ofono_modem_create(dev_name, ril_type);
	if (modem == NULL) {
		DBG("ofono_modem_create failed for type: %s", ril_type);
		return -ENODEV;
	}
	DBG("created modem: %p", modem);

	modem_list = g_slist_prepend(modem_list, modem);

	ofono_modem_set_integer(modem, "Slot", slot);

	/* This causes driver->probe() to be called */
	retval = ofono_modem_register(modem);
	if (retval != 0) {
		ofono_error("%s: ofono_modem_register error: %d",
				__func__, retval);
		return retval;
	}

	return 0;
}

/*
 * Try creating a ril modem
 * return: false if failed, true successful or modem already exists.
 */
static gboolean try_create_modem()
{
	gboolean result = FALSE;
	int ares = access(RILD_CMD_SOCKET, F_OK);
	if (ares != -1)
		result = !create_rilmodem("ril", 0);
	else
		DBG("problems accessing rild socket: %d", ares);

	return result;
}

static gboolean rild_inotify(GIOChannel *gio, GIOCondition c,
		gpointer data)
{
	DBG("");
	struct inotify_event *event = 0;
	int i = 0;
	int length = 0;
	char *ievents = 0; /* inotify event buffer */
	gboolean result = TRUE;

	ievents = g_try_malloc(IBUF_LEN);
	if (!ievents) {
		/* Continue observing so don't set "result" false here */
		goto end;
	}

	length = read(inotify_fd, ievents, IBUF_LEN);
	/*
	 * If iNotify fd read returns an error, just keep on watching for
	 * read events.
	 */
	while (i < length) {
		event = (struct inotify_event *) &ievents[i];

		if (event->len && (event->mask & IN_CREATE)
			&& (!(event->mask & IN_ISDIR))) {

			DBG("File created: %s", event->name);
			if (!strcmp(event->name, RILD_SOCKET_FILE)) {
				result = !try_create_modem();
				/*
				 * On modem create fail continue observing
				 * events so don't set result false here.
				 */
				goto end;
			}
		}
		i += EVENT_SIZE + event->len;
	}

end:
	/* "if" works around potential glib runtime warning */
	if (ievents)
		g_free(ievents);

	if (!result)
		remove_watchers();

	return result;
}

/* return 0 if successful, otherwise an error */
static int watch_for_rild_socket(void)
{
	DBG("");
	inotify_fd = inotify_init();
	if (inotify_fd < 0)
		return -EIO;

	inotify_watch_channel = g_io_channel_unix_new(inotify_fd);
	if (inotify_watch_channel == NULL) {
		ofono_error("%s: rildev gio chan creation fail!", __func__);
		close(inotify_fd);
		inotify_fd = -1;
		return -EIO;
	}

	inotify_watch_id = inotify_add_watch(inotify_fd, RILD_SOCKET_DIR,
						IN_CREATE);
	if (inotify_watch_id < 0) {
		ofono_error("%s: inotify says: %d, errno: %d",
				__func__, inotify_watch_id, errno);
		g_io_channel_unref(inotify_watch_channel);
		inotify_watch_channel = NULL;
		close(inotify_fd);
		inotify_fd = -1;
		return -EIO;
	}

	inotify_watch_source_id = g_io_add_watch(inotify_watch_channel,
							G_IO_IN,
							rild_inotify, NULL);
	if (inotify_watch_source_id <= 0) {
		ofono_error("%s: rildev add gio watch fail!", __func__);
		g_io_channel_unref(inotify_watch_channel);
		inotify_watch_channel = NULL;
		inotify_rm_watch(inotify_fd, inotify_watch_id);
		inotify_watch_id = -1;
		close(inotify_fd);
		inotify_fd = -1;
		return -EIO;
	}

	return 0;
}

static void detect_rild(void)
{
	DBG("");
	gboolean created = try_create_modem();
	if (!created)
		watch_for_rild_socket();

	/* Let's re-check if we just missed the notification */
	if (!created && try_create_modem())
		remove_watchers();
}

static int detect_init(void)
{
	DBG("");
	detect_rild();
	return 0;
}

static void detect_exit(void)
{
	GSList *list;

	DBG("");

	for (list = modem_list; list; list = list->next) {
		struct ofono_modem *modem = list->data;

		ofono_modem_remove(modem);
	}

	g_slist_free(modem_list);
	modem_list = NULL;
	remove_watchers();
}

OFONO_PLUGIN_DEFINE(rildev, "RIL type detection", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, detect_init, detect_exit)
