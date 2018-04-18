/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2018 Jolla Ltd.
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

#ifndef RIL_SIM_CARD_H
#define RIL_SIM_CARD_H

#include "ril_types.h"

#include <glib-object.h>

struct ril_sim_card_app {
	enum ril_app_type app_type;
	enum ril_app_state app_state;
	enum ril_perso_substate perso_substate;
	char *aid;
	char *label;
	guint pin_replaced;
	enum ril_pin_state pin1_state;
	enum ril_pin_state pin2_state;
};

struct ril_sim_card_status {
	enum ril_card_state card_state;
	enum ril_pin_state pin_state;
	int gsm_umts_index;
	int cdma_index;
	int ims_index;
	int num_apps;
	struct ril_sim_card_app *apps;
};

struct ril_sim_card {
	GObject object;
	struct ril_sim_card_priv *priv;
	struct ril_sim_card_status *status;
	const struct ril_sim_card_app *app;
	gboolean sim_io_active;
	guint slot;
};

typedef void (*ril_sim_card_cb_t)(struct ril_sim_card *sc, void *arg);

/* Flags for ril_sim_card_new */
#define RIL_SIM_CARD_V9_UICC_SUBSCRIPTION_WORKAROUND    (0x01)

struct ril_sim_card *ril_sim_card_new(GRilIoChannel *io, guint slot, int flags);
struct ril_sim_card *ril_sim_card_ref(struct ril_sim_card *sc);
void ril_sim_card_unref(struct ril_sim_card *sc);
void ril_sim_card_reset(struct ril_sim_card *sc);
void ril_sim_card_request_status(struct ril_sim_card *sc);
void ril_sim_card_sim_io_started(struct ril_sim_card *sc, guint id);
void ril_sim_card_sim_io_finished(struct ril_sim_card *sc, guint id);
gboolean ril_sim_card_ready(struct ril_sim_card *sc);
gulong ril_sim_card_add_status_received_handler(struct ril_sim_card *sc,
					ril_sim_card_cb_t cb, void *arg);
gulong ril_sim_card_add_status_changed_handler(struct ril_sim_card *sc,
					ril_sim_card_cb_t cb, void *arg);
gulong ril_sim_card_add_state_changed_handler(struct ril_sim_card *sc,
					ril_sim_card_cb_t cb, void *arg);
gulong ril_sim_card_add_app_changed_handler(struct ril_sim_card *sc,
					ril_sim_card_cb_t cb, void *arg);
gulong ril_sim_card_add_sim_io_active_changed_handler(struct ril_sim_card *sc,
					ril_sim_card_cb_t cb, void *arg);
void ril_sim_card_remove_handler(struct ril_sim_card *sc, gulong id);
void ril_sim_card_remove_handlers(struct ril_sim_card *sc, gulong *ids, int n);

/* Inline wrappers */
static inline enum ril_app_type
ril_sim_card_app_type(struct ril_sim_card *sc)
	{ return (sc && sc->app) ? sc->app->app_type : RIL_APPTYPE_UNKNOWN; }

#define ril_sim_card_remove_all_handlers(net, ids) \
	ril_sim_card_remove_handlers(net, ids, G_N_ELEMENTS(ids))

#endif /* RIL_SIM_CARD_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
