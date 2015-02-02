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

static int inotify_fd = -1;
static int inotify_watch_id = -1;
static guint inotify_watch_source_id = 0;
static GIOChannel *inotify_watch_channel = NULL;

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

void ril_modem_remove(struct ofono_modem *modem)
{
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
		DBG("ofono_modem_create failed for type %s", ril_type);
		return -ENODEV;
	}

	modem_list = g_slist_prepend(modem_list, modem);

	ofono_modem_set_integer(modem, "Slot", slot);

	/* This causes driver->probe() to be called... */
	if ((retval = ofono_modem_register(modem)) != 0) {
		ofono_error("%s: ofono_modem_register returned: %d",
				__func__, retval);
		return retval;
	}

	return 0;
}

static gboolean rild_inotify(GIOChannel *gio, GIOCondition c, gpointer data)
{
	DBG("");

	if (access(RILD_CMD_SOCKET, F_OK) != -1){
		create_rilmodem("ril", 0);
		return FALSE;
	}

	return TRUE;
}

static int watch_for_rild_socket(void)
{
	inotify_fd = inotify_init();
	if (inotify_fd < 0)
		return -EIO;

	inotify_watch_channel = g_io_channel_unix_new(inotify_fd);
	if (inotify_watch_channel == NULL) {
		close(inotify_fd);
		inotify_fd = -1;
		return -EIO;
	}

	inotify_watch_id = inotify_add_watch(inotify_fd,
					RILD_SOCKET_DIR,
					IN_CREATE);

	if (inotify_watch_id < 0) {
		g_io_channel_unref(inotify_watch_channel);
		inotify_watch_channel = NULL;
		close(inotify_fd);
		inotify_fd = -1;
		return -EIO;
	}

	inotify_watch_source_id = g_io_add_watch(inotify_watch_channel, G_IO_IN,
							rild_inotify, NULL);
	if (inotify_watch_source_id <= 0) {
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
	if (rild_inotify(NULL,0,NULL))
		watch_for_rild_socket();

	/* Let's recheck if we just missed the rild */
	if (!rild_inotify(NULL,0,NULL))
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

OFONO_PLUGIN_DEFINE(rildev, "ril type detection", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, detect_init, detect_exit)
