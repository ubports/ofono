/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016 Jolla Ltd.
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

#include "ril_ecclist.h"
#include "ril_log.h"

#include <gutil_strv.h>
#include <gutil_inotify.h>

#include <sys/inotify.h>

typedef GObjectClass RilEccListClass;
typedef struct ril_ecclist RilEccList;

struct ril_ecclist_priv {
	struct ofono_sim *sim;
	GUtilInotifyWatchCallback *dir_watch;
	GUtilInotifyWatchCallback *file_watch;
	char *dir;
	char *path;
	char *name;
};

enum ril_ecclist_signal {
	SIGNAL_LIST_CHANGED,
	SIGNAL_COUNT
};

#define SIGNAL_LIST_CHANGED_NAME        "ril-ecclist-changed"

static guint ril_ecclist_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(RilEccList, ril_ecclist, G_TYPE_OBJECT)
#define RIL_ECCLIST_TYPE (ril_ecclist_get_type())
#define RIL_ECCLIST(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	RIL_ECCLIST_TYPE, RilEccList))

static char **ril_ecclist_read(struct ril_ecclist *self)
{
	struct ril_ecclist_priv *priv = self->priv;
	char **list = NULL;

	if (g_file_test(priv->path, G_FILE_TEST_EXISTS)) {
		gsize len = 0;
		gchar *content = NULL;
		GError *error = NULL;

		if (g_file_get_contents(priv->path, &content, &len, &error)) {
			char **ptr;

			DBG("%s = %s", priv->name, content);
			list = g_strsplit(content, ",", 0);
			for (ptr = list; *ptr; ptr++) {
				*ptr = g_strstrip(*ptr);
			}

			gutil_strv_sort(list, TRUE);
		} else if (error) {
			DBG("%s: %s", priv->path, GERRMSG(error));
			g_error_free(error);
		}

		g_free (content);
	} else {
		DBG("%s doesn't exist", priv->path);
	}

	return list;
}

static void ril_ecclist_update(struct ril_ecclist *self)
{
	struct ril_ecclist_priv *priv = self->priv;
	char **list = ril_ecclist_read(self);

	if (!gutil_strv_equal(self->list, list)) {
		DBG("%s changed", priv->name);
		g_strfreev(self->list);
		self->list = list;
		g_signal_emit(self, ril_ecclist_signals[SIGNAL_LIST_CHANGED], 0);
	} else {
		g_strfreev(list);
	}
}

static void ril_ecclist_changed(GUtilInotifyWatch *watch, guint mask,
			guint cookie, const char *name, void *user_data)
{
	struct ril_ecclist *self = RIL_ECCLIST(user_data);
	struct ril_ecclist_priv *priv = self->priv;

	ril_ecclist_update(self);

	if (mask & IN_IGNORED) {
		DBG("file %s is gone", priv->path);
		gutil_inotify_watch_callback_free(priv->file_watch);
		priv->file_watch = NULL;
	}
}

static void ril_ecclist_dir_changed(GUtilInotifyWatch *watch, guint mask,
			guint cookie, const char *name, void *user_data)
{
	struct ril_ecclist *self = RIL_ECCLIST(user_data);
	struct ril_ecclist_priv *priv = self->priv;

	DBG("0x%04x %s", mask, name);
	if (!priv->file_watch && !g_strcmp0(name, priv->name)) {
		priv->file_watch = gutil_inotify_watch_callback_new(priv->path,
					IN_MODIFY | IN_CLOSE_WRITE,
					ril_ecclist_changed, self);
		if (priv->file_watch) {
			DBG("watching %s", priv->path);
			ril_ecclist_update(self);
		}
	}

	if (mask & IN_IGNORED) {
		DBG("%s is gone", priv->dir);
		gutil_inotify_watch_callback_free(priv->dir_watch);
		priv->dir_watch = NULL;
	}
}

gulong ril_ecclist_add_list_changed_handler(struct ril_ecclist *self,
					ril_ecclist_cb_t cb, void *arg)
{
	return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
		SIGNAL_LIST_CHANGED_NAME, G_CALLBACK(cb), arg) : 0;
}

void ril_ecclist_remove_handler(struct ril_ecclist *self, gulong id)
{
	if (G_LIKELY(self) && G_LIKELY(id)) {
		g_signal_handler_disconnect(self, id);
	}
}

struct ril_ecclist *ril_ecclist_new(const char *path)
{
	if (path) {
		struct ril_ecclist *self = g_object_new(RIL_ECCLIST_TYPE, 0);
		struct ril_ecclist_priv *priv = self->priv;

		DBG("%s", path);
		priv->path = g_strdup(path);
		priv->name = g_path_get_basename(path);
		priv->dir = g_path_get_dirname(path);
		priv->dir_watch = gutil_inotify_watch_callback_new(priv->dir,
				IN_MODIFY|IN_MOVED_FROM|IN_MOVED_TO|IN_DELETE|
				IN_CREATE|IN_DELETE_SELF|IN_CLOSE_WRITE,
				ril_ecclist_dir_changed, self);
		if (priv->dir_watch) {
			DBG("watching %s", priv->dir);
		}

		self->list = ril_ecclist_read(self);
		priv->file_watch = gutil_inotify_watch_callback_new(priv->path,
				IN_MODIFY | IN_CLOSE_WRITE,
				ril_ecclist_changed, self);
		if (priv->file_watch) {
			DBG("watching %s", priv->path);
		}

		return self;
	}

	return NULL;
}

struct ril_ecclist *ril_ecclist_ref(struct ril_ecclist *self)
{
	if (G_LIKELY(self)) {
		g_object_ref(RIL_ECCLIST(self));
		return self;
	} else {
		return NULL;
	}
}

void ril_ecclist_unref(struct ril_ecclist *self)
{
	if (G_LIKELY(self)) {
		g_object_unref(RIL_ECCLIST(self));
	}
}

static void ril_ecclist_init(struct ril_ecclist *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self, RIL_ECCLIST_TYPE,
						struct ril_ecclist_priv);
}

static void ril_ecclist_dispose(GObject *object)
{
	struct ril_ecclist *self = RIL_ECCLIST(object);
	struct ril_ecclist_priv *priv = self->priv;

	if (priv->dir_watch) {
		gutil_inotify_watch_callback_free(priv->dir_watch);
		priv->dir_watch = NULL;
	}

	if (priv->file_watch) {
		gutil_inotify_watch_callback_free(priv->file_watch);
		priv->file_watch = NULL;
	}

	G_OBJECT_CLASS(ril_ecclist_parent_class)->dispose(object);
}

static void ril_ecclist_finalize(GObject *object)
{
	struct ril_ecclist *self = RIL_ECCLIST(object);
	struct ril_ecclist_priv *priv = self->priv;

	GASSERT(!priv->dir_watch);
	GASSERT(!priv->file_watch);
	g_free(priv->dir);
	g_free(priv->path);
	g_free(priv->name);
	g_strfreev(self->list);

	G_OBJECT_CLASS(ril_ecclist_parent_class)->finalize(object);
}

static void ril_ecclist_class_init(RilEccListClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ril_ecclist_dispose;
	object_class->finalize = ril_ecclist_finalize;
	g_type_class_add_private(klass, sizeof(struct ril_ecclist_priv));
	ril_ecclist_signals[SIGNAL_LIST_CHANGED] =
		g_signal_new(SIGNAL_LIST_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
