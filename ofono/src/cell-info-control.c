/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2021 Jolla Ltd.
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

#include "cell-info-control.h"

#include <ofono/cell-info.h>
#include <ofono/log.h>

#include <glib.h>

#include <limits.h>

typedef struct cell_info_control_object {
	CellInfoControl pub;
	int refcount;
	char* path;
	GHashTable *enabled;
	GHashTable *set_interval;
} CellInfoControlObject;

static GHashTable *cell_info_control_table = NULL;

static inline CellInfoControlObject *cell_info_control_object_cast
	(CellInfoControl *ctl)
{
	return ctl ? G_STRUCT_MEMBER_P(ctl,
		- G_STRUCT_OFFSET(CellInfoControlObject, pub)) : NULL;
}

static int cell_info_control_get_interval(CellInfoControlObject *self)
{
	int interval = INT_MAX;

	if (self->set_interval) {
		GHashTableIter it;
		gpointer value;

		g_hash_table_iter_init(&it, self->set_interval);
		while (g_hash_table_iter_next(&it, NULL, &value)) {
			/* All values are >=0 && < INT_MAX */
			interval = MIN(interval, GPOINTER_TO_INT(value));
		}
	}
	return interval;
}

static void cell_info_control_update_all(CellInfoControlObject *self)
{
	struct ofono_cell_info *cellinfo = self->pub.info;

	if (cellinfo) {
		if (self->enabled) {
			ofono_cell_info_set_update_interval(cellinfo,
				cell_info_control_get_interval(self));
			ofono_cell_info_set_enabled(cellinfo, TRUE);
		} else {
			ofono_cell_info_set_enabled(cellinfo, FALSE);
			ofono_cell_info_set_update_interval(cellinfo,
				cell_info_control_get_interval(self));
		}
	}
}

static void cell_info_control_drop_all_requests_internal
					(CellInfoControlObject *self)
{
	if (self->enabled) {
		g_hash_table_destroy(self->enabled);
		self->enabled = NULL;
	}
	if (self->set_interval) {
		g_hash_table_destroy(self->set_interval);
		self->set_interval = NULL;
	}
}

static void cell_info_control_free(CellInfoControlObject *self)
{
	/* Destroy the table when the last instance is done */
	g_hash_table_remove(cell_info_control_table, self->path);
	if (g_hash_table_size(cell_info_control_table) == 0) {
		g_hash_table_unref(cell_info_control_table);
		cell_info_control_table = NULL;
		DBG("%s gone", self->path);
	}

	cell_info_control_drop_all_requests_internal(self);
	ofono_cell_info_unref(self->pub.info);
	g_free(self->path);
	g_free(self);
}

CellInfoControl *cell_info_control_get(const char* path)
{
	if (path) {
		CellInfoControlObject *self = NULL;

		if (cell_info_control_table) {
			self = g_hash_table_lookup(cell_info_control_table,
				path);
		}
		if (self) {
			/* Already there */
			return cell_info_control_ref(&self->pub);
		} else {
			/* Create a new one */
			self = g_new0(CellInfoControlObject, 1);
			self->pub.path = self->path = g_strdup(path);
			self->refcount = 1;

			/* Create the table if necessary */
			if (!cell_info_control_table) {
				cell_info_control_table =
					g_hash_table_new(g_str_hash,
						g_str_equal);
			}
			g_hash_table_insert(cell_info_control_table,
				self->path, self);
			DBG("%s created", path);
			return &self->pub;
		}
	}
	return NULL;
}

CellInfoControl *cell_info_control_ref(CellInfoControl *ctl)
{
	CellInfoControlObject *self = cell_info_control_object_cast(ctl);

	if (self) {
		self->refcount++;
	}
	return ctl;
}

void cell_info_control_unref(CellInfoControl *ctl)
{
	CellInfoControlObject *self = cell_info_control_object_cast(ctl);

	if (self && !--self->refcount) {
		cell_info_control_free(self);
	}
}

void cell_info_control_set_cell_info(CellInfoControl *ctl,
				struct ofono_cell_info *ci)
{
	CellInfoControlObject *self = cell_info_control_object_cast(ctl);

	if (self && ctl->info != ci) {
		ofono_cell_info_unref(ctl->info);
		ctl->info = ofono_cell_info_ref(ci);
		cell_info_control_update_all(self);
	}
}

void cell_info_control_drop_all_requests(CellInfoControl *ctl)
{
	CellInfoControlObject *self = cell_info_control_object_cast(ctl);

	if (self) {
		cell_info_control_drop_all_requests_internal(self);
		cell_info_control_update_all(self);
	}
}

void cell_info_control_drop_requests(CellInfoControl *ctl, void *tag)
{
	CellInfoControlObject *self = cell_info_control_object_cast(ctl);

	if (self && tag) {
		if (self->enabled &&
			g_hash_table_remove(self->enabled, tag) &&
			!g_hash_table_size(self->enabled)) {
			g_hash_table_unref(self->enabled);
			self->enabled = NULL;
			ofono_cell_info_set_enabled(ctl->info, FALSE);
		}
		if (self->set_interval &&
			g_hash_table_remove(self->set_interval, tag)) {
			int ms;

			if (g_hash_table_size(self->set_interval)) {
				ms = cell_info_control_get_interval(self);
			} else {
				g_hash_table_unref(self->set_interval);
				self->set_interval = NULL;
				ms = INT_MAX;
			}
			ofono_cell_info_set_update_interval(ctl->info, ms);
		}
	}
}

void cell_info_control_set_enabled(CellInfoControl *ctl,
	void *tag, ofono_bool_t enabled)
{
	CellInfoControlObject *self = cell_info_control_object_cast(ctl);

	if (self && tag) {
		gboolean was_enabled = (self->enabled != NULL);
		gboolean is_enabled;

		if (enabled) {
			if (!self->enabled) {
				self->enabled = g_hash_table_new(g_direct_hash,
					g_direct_equal);
			}
			g_hash_table_add(self->enabled, tag);
		} else if (self->enabled) {
			g_hash_table_remove(self->enabled, tag);
			if (!g_hash_table_size(self->enabled)) {
				g_hash_table_unref(self->enabled);
				self->enabled = NULL;
			}
		}

		is_enabled = (self->enabled != NULL);
		if (is_enabled != was_enabled) {
			ofono_cell_info_set_enabled(ctl->info, is_enabled);
		}
	}
}

void cell_info_control_set_update_interval(CellInfoControl *ctl,
	void *tag, int ms)
{
	CellInfoControlObject *self = cell_info_control_object_cast(ctl);

	if (self && tag) {
		int old_interval = cell_info_control_get_interval(self);
		int new_interval;

		if (ms >= 0 && ms < INT_MAX) {
			if (!self->set_interval) {
				self->set_interval =
					g_hash_table_new(g_direct_hash,
						g_direct_equal);

			}
			g_hash_table_insert(self->set_interval, tag,
				GINT_TO_POINTER(ms));
		} else if (self->set_interval) {
			g_hash_table_remove(self->set_interval, tag);
			if (!g_hash_table_size(self->set_interval)) {
				g_hash_table_unref(self->set_interval);
				self->set_interval = NULL;
			}
		}

		new_interval = cell_info_control_get_interval(self);
		if (new_interval != old_interval) {
			if (new_interval == INT_MAX) {
				DBG("maximum");
			} else {
				DBG("%d ms", new_interval);
			}
			ofono_cell_info_set_update_interval(ctl->info,
				new_interval);
		}
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
