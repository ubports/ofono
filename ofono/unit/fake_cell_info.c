/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2021 Jolla Ltd.
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

#include "fake_cell_info.h"

#include <ofono/log.h>

#include <gutil_macros.h>
#include <gutil_misc.h>

#include <glib-object.h>

typedef GObjectClass FakeCellInfoClass;
typedef struct fake_cell_info {
	GObject object;
	struct ofono_cell_info info;
	struct ofono_cell **cells;
	int interval;
	gboolean enabled;
} FakeCellInfo;

typedef struct fake_cell_info_signal_data {
	ofono_cell_info_cb_t cb;
	void *arg;
} FakeCellInfoSignalData;

enum fake_cell_info_signal {
	SIGNAL_CHANGED,
	SIGNAL_COUNT
};

static guint fake_cell_info_signals[SIGNAL_COUNT] = { 0 };

#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, FakeCellInfo)
#define THIS_TYPE fake_cell_info_get_type()
#define PARENT_TYPE G_TYPE_OBJECT
#define PARENT_CLASS fake_cell_info_parent_class

G_DEFINE_TYPE(FakeCellInfo, fake_cell_info, PARENT_TYPE)

#define SIGNAL_CHANGED_NAME  "fake-cell-info-changed"

static FakeCellInfo *fake_cell_info_cast(struct ofono_cell_info *info)
{
	return G_CAST(info, FakeCellInfo, info);
}

static void fake_cell_info_ref_proc(struct ofono_cell_info *info)
{
	g_object_ref(fake_cell_info_cast(info));
}

static void fake_cell_info_unref_proc(struct ofono_cell_info *info)
{
	g_object_unref(fake_cell_info_cast(info));
}

static void fake_cell_info_change_cb(FakeCellInfo *self, void *data)
{
	FakeCellInfoSignalData *signal_data = data;

	signal_data->cb(&self->info, signal_data->arg);
}

static void fake_cell_info_change_free(gpointer data, GClosure *closure)
{
	g_free(data);
}

static gulong fake_cell_info_add_change_handler_proc
				(struct ofono_cell_info *info,
					ofono_cell_info_cb_t cb, void *arg)
{
	if (cb) {
		FakeCellInfoSignalData *data =
			g_new0(FakeCellInfoSignalData, 1);

		data->cb = cb;
		data->arg = arg;
		return g_signal_connect_data(fake_cell_info_cast(info),
				SIGNAL_CHANGED_NAME,
				G_CALLBACK(fake_cell_info_change_cb),
				data, fake_cell_info_change_free,
				G_CONNECT_AFTER);
	} else {
		return 0;
	}
}

static void fake_cell_info_remove_handler_proc(struct ofono_cell_info *info,
								gulong id)
{
	if (id) {
		g_signal_handler_disconnect(fake_cell_info_cast(info), id);
	}
}

static void fake_cell_info_set_update_interval(struct ofono_cell_info *info,
	int ms)
{
	DBG("%d", ms);
	fake_cell_info_cast(info)->interval = ms;
}

static void fake_cell_info_set_enabled(struct ofono_cell_info *info,
	ofono_bool_t enabled)
{
	DBG("%d", enabled);
	fake_cell_info_cast(info)->enabled = enabled;
}

static void fake_cell_info_init(FakeCellInfo *self)
{
	self->info.cells = self->cells = g_new0(struct ofono_cell*, 1);
}

static void fake_cell_info_finalize(GObject *object)
{
	FakeCellInfo *self = THIS(object);

	gutil_ptrv_free((void**)self->cells);
	G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static void fake_cell_info_class_init(FakeCellInfoClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = fake_cell_info_finalize;
	fake_cell_info_signals[SIGNAL_CHANGED] =
		g_signal_new(SIGNAL_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

struct ofono_cell_info *fake_cell_info_new()
{
	static const struct ofono_cell_info_proc fake_cell_info_proc = {
		fake_cell_info_ref_proc,
		fake_cell_info_unref_proc,
		fake_cell_info_add_change_handler_proc,
		fake_cell_info_remove_handler_proc,
		fake_cell_info_set_update_interval,
		fake_cell_info_set_enabled
	};

	FakeCellInfo *self = g_object_new(THIS_TYPE, 0);

	self->info.proc = &fake_cell_info_proc;
	return &self->info;
}

void fake_cell_info_add_cell(struct ofono_cell_info *info,
				  const struct ofono_cell* c)
{
	FakeCellInfo *self = fake_cell_info_cast(info);
	gsize n = gutil_ptrv_length(self->cells);

	self->cells = g_renew(struct ofono_cell*, self->cells, n + 2);
	self->cells[n++] = g_memdup(c, sizeof(*c));
	self->cells[n] = NULL;
	info->cells = self->cells;
}

ofono_bool_t fake_cell_info_remove_cell(struct ofono_cell_info *info,
	const struct ofono_cell* cell)
{
	FakeCellInfo *self = fake_cell_info_cast(info);
	gsize i, n = gutil_ptrv_length(self->cells);

	for (i = 0; i < n; i++) {
		struct ofono_cell *known_cell = self->cells[i];

		if (!memcmp(cell, known_cell, sizeof(*cell))) {
			g_free(known_cell);
			memmove(self->cells + i, self->cells + i + 1,
				sizeof(struct ofono_cell*)*(n - i));
			self->cells = g_renew(struct ofono_cell*,
				self->cells, n);
			info->cells = self->cells;
			return TRUE;
		}
	}
	return FALSE;
}

void fake_cell_info_remove_all_cells(struct ofono_cell_info *info)
{
	FakeCellInfo *self = fake_cell_info_cast(info);

	if (gutil_ptrv_length(self->cells) > 0) {
		gutil_ptrv_free((void**)self->cells);
		self->info.cells = self->cells = g_new0(struct ofono_cell*, 1);
	}
}

void fake_cell_info_cells_changed(struct ofono_cell_info *info)
{
	g_signal_emit(fake_cell_info_cast(info), fake_cell_info_signals
		[SIGNAL_CHANGED], 0);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
