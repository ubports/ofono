/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Jolla Ltd.
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

#include "fake_sailfish_cell_info.h"

#include <gutil_macros.h>

#include <glib-object.h>

typedef GObjectClass FakeCellInfoClass;
typedef struct fake_cell_info {
	GObject object;
	struct sailfish_cell_info info;
} FakeCellInfo;

typedef struct fake_cell_info_signal_data {
	sailfish_cell_info_cb_t cb;
	void *arg;
} FakeCellInfoSignalData;

enum fake_cell_info_signal {
	SIGNAL_CELLS_CHANGED,
	SIGNAL_COUNT
};

static guint fake_cell_info_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(FakeCellInfo, fake_cell_info, G_TYPE_OBJECT)
#define FAKE_CELL_INFO_TYPE (fake_cell_info_get_type())
#define FAKE_CELL_INFO(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	FAKE_CELL_INFO_TYPE, FakeCellInfo))

#define SIGNAL_CELLS_CHANGED_NAME   "fake-cell-info-cells-changed"

static FakeCellInfo *fake_cell_info_cast(struct sailfish_cell_info *info)
{
	return G_CAST(info, FakeCellInfo, info);
}

static void fake_cell_info_ref_proc(struct sailfish_cell_info *info)
{
	g_object_ref(fake_cell_info_cast(info));
}

static void fake_cell_info_unref_proc(struct sailfish_cell_info *info)
{
	g_object_unref(fake_cell_info_cast(info));
}

static void fake_cell_info_cells_changed_cb(FakeCellInfo *self, void *data)
{
	FakeCellInfoSignalData *signal_data = data;

	signal_data->cb(&self->info, signal_data->arg);
}

static void fake_cell_info_cells_disconnect_notify(gpointer data,
							GClosure *closure)
{
	g_free(data);
}

static gulong fake_cell_info_add_cells_changed_handler_proc
				(struct sailfish_cell_info *info,
					sailfish_cell_info_cb_t cb, void *arg)
{
	if (cb) {
		FakeCellInfoSignalData *data =
			g_new0(FakeCellInfoSignalData, 1);

		data->cb = cb;
		data->arg = arg;
		return g_signal_connect_data(fake_cell_info_cast(info),
				SIGNAL_CELLS_CHANGED_NAME,
				G_CALLBACK(fake_cell_info_cells_changed_cb),
				data, fake_cell_info_cells_disconnect_notify,
				G_CONNECT_AFTER);
	} else {
		return 0;
	}
}

static void fake_cell_info_remove_handler_proc(struct sailfish_cell_info *info,
								gulong id)
{
	if (id) {
		g_signal_handler_disconnect(fake_cell_info_cast(info), id);
	}
}

static void fake_cell_info_init(FakeCellInfo *self)
{
}

static void fake_cell_info_finalize(GObject *object)
{
	FakeCellInfo *self = FAKE_CELL_INFO(object);

	fake_cell_info_remove_all_cells(&self->info);
	G_OBJECT_CLASS(fake_cell_info_parent_class)->finalize(object);
}

static void fake_cell_info_class_init(FakeCellInfoClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = fake_cell_info_finalize;
	fake_cell_info_signals[SIGNAL_CELLS_CHANGED] =
		g_signal_new(SIGNAL_CELLS_CHANGED_NAME,
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
			0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

struct sailfish_cell_info *fake_cell_info_new()
{
	static const struct sailfish_cell_info_proc fake_cell_info_proc = {
		fake_cell_info_ref_proc,
		fake_cell_info_unref_proc,
		fake_cell_info_add_cells_changed_handler_proc,
		fake_cell_info_remove_handler_proc
	};

	FakeCellInfo *self = g_object_new(FAKE_CELL_INFO_TYPE, 0);

	self->info.proc = &fake_cell_info_proc;
	return &self->info;
}

void fake_cell_info_add_cell(struct sailfish_cell_info *info,
				  const struct sailfish_cell* cell)
{
	info->cells = g_slist_append(info->cells,
				g_memdup(cell, sizeof(*cell)));
}

gboolean fake_cell_info_remove_cell(struct sailfish_cell_info *info,
				  const struct sailfish_cell* cell)
{
	GSList *l;

	for (l = info->cells; l; l = l->next) {
		struct sailfish_cell *known_cell = l->data;

		if (!memcmp(cell, known_cell, sizeof(*cell))) {
			info->cells = g_slist_remove(info->cells, known_cell);
			g_free(known_cell);
			return TRUE;
		}
	}
	return FALSE;
}

void fake_cell_info_remove_all_cells(struct sailfish_cell_info *info)
{
	g_slist_free_full(info->cells, g_free);
	info->cells = NULL;
}

void fake_cell_info_cells_changed(struct sailfish_cell_info *info)
{
	g_signal_emit(fake_cell_info_cast(info), fake_cell_info_signals
			[SIGNAL_CELLS_CHANGED], 0);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
