/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2016-2018 Jolla Ltd.
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

#ifndef RIL_DATA_H
#define RIL_DATA_H

#include "ril_types.h"

#include <ofono/gprs-context.h>

#include <glib-object.h>

enum ril_data_call_active {
	RIL_DATA_CALL_INACTIVE = 0,
	RIL_DATA_CALL_LINK_DOWN = 1,
	RIL_DATA_CALL_ACTIVE = 2
};

struct ril_data_call {
	int cid;
	enum ril_data_call_fail_cause status;
	enum ril_data_call_active active;
	enum ofono_gprs_proto prot;
	int retry_time;
	int mtu;
	char *ifname;
	char **dnses;
	char **gateways;
	char **addresses;
};

struct ril_data_call_list {
	guint version;
	guint num;
	GSList *calls;
};

struct ril_data {
	GObject object;
	struct ril_data_priv *priv;
	struct ril_data_call_list *data_calls;
};

enum ril_data_manager_flags {
	RIL_DATA_MANAGER_3GLTE_HANDOVER = 0x01
};

enum ril_data_allow_data_opt {
	RIL_ALLOW_DATA_AUTO,
	RIL_ALLOW_DATA_ENABLED,
	RIL_ALLOW_DATA_DISABLED
};

enum ril_data_call_format {
	RIL_DATA_CALL_FORMAT_AUTO,
	RIL_DATA_CALL_FORMAT_6 = 6,
	RIL_DATA_CALL_FORMAT_9 = 9,
	RIL_DATA_CALL_FORMAT_11 = 11
};

struct ril_data_options {
	enum ril_data_allow_data_opt allow_data;
	enum ril_data_call_format data_call_format;
	unsigned int data_call_retry_limit;
	unsigned int data_call_retry_delay_ms;
};

enum ril_data_role {
	RIL_DATA_ROLE_NONE,    /* Data not allowed */
	RIL_DATA_ROLE_MMS,     /* Data is allowed at any speed */
	RIL_DATA_ROLE_INTERNET /* Data is allowed at full speed */
};

struct ril_data_manager;
struct ril_data_manager *ril_data_manager_new(enum ril_data_manager_flags flg);
struct ril_data_manager *ril_data_manager_ref(struct ril_data_manager *dm);
void ril_data_manager_unref(struct ril_data_manager *dm);
void ril_data_manager_assert_data_on(struct ril_data_manager *dm);

typedef void (*ril_data_cb_t)(struct ril_data *data, void *arg);
typedef void (*ril_data_call_setup_cb_t)(struct ril_data *data,
			int ril_status, const struct ril_data_call *call,
			void *arg);
typedef void (*ril_data_call_deactivate_cb_t)(struct ril_data *data,
			int ril_status, void *arg);

struct ril_data *ril_data_new(struct ril_data_manager *dm, const char *name,
		struct ril_radio *radio, struct ril_network *network,
		GRilIoChannel *io, const struct ril_data_options *options,
		const struct ril_slot_config *config,
		struct ril_vendor_hook *vendor_hook);
struct ril_data *ril_data_ref(struct ril_data *data);
void ril_data_unref(struct ril_data *data);
gboolean ril_data_allowed(struct ril_data *data);
void ril_data_poll_call_state(struct ril_data *data);

gulong ril_data_add_allow_changed_handler(struct ril_data *data,
					ril_data_cb_t cb, void *arg);
gulong ril_data_add_calls_changed_handler(struct ril_data *data,
					ril_data_cb_t cb, void *arg);
void ril_data_remove_handler(struct ril_data *data, gulong id);

void ril_data_allow(struct ril_data *data, enum ril_data_role role);

struct ril_data_request;
struct ril_data_request *ril_data_call_setup(struct ril_data *data,
				const struct ofono_gprs_primary_context *ctx,
				ril_data_call_setup_cb_t cb, void *arg);
struct ril_data_request *ril_data_call_deactivate(struct ril_data *data,
			int cid, ril_data_call_deactivate_cb_t cb, void *arg);
void ril_data_request_detach(struct ril_data_request *req);
void ril_data_request_cancel(struct ril_data_request *req);

void ril_data_call_free(struct ril_data_call *call);
struct ril_data_call *ril_data_call_dup(const struct ril_data_call *call);
struct ril_data_call *ril_data_call_find(struct ril_data_call_list *list,
								int cid);

const char *ril_data_ofono_protocol_to_ril(enum ofono_gprs_proto proto);
int ril_data_protocol_to_ofono(const gchar *str);

/* Constructors of various kinds of RIL requests */
GRilIoRequest *ril_request_allow_data_new(gboolean allow);
GRilIoRequest *ril_request_deactivate_data_call_new(int cid);

#endif /* RIL_DATA_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
