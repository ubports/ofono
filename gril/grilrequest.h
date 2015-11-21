/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014  Canonical Ltd.
 *  Copyright (C) 2015 Ratchanan Srirattanamet.
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

#ifndef __GRILREQUEST_H
#define __GRILREQUEST_H

#include <ofono/types.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

struct req_deactivate_data_call {
	gint cid;
	guint reason;
};

struct req_setup_data_call {
	guint tech;
	guint data_profile;
	gchar *apn;
	gchar *username;
	gchar *password;
	guint auth_type;
	guint protocol;
	unsigned req_cid;
};


struct req_sim_read_record {
	guint app_type;
	gchar *aid_str;
	int fileid;
	const unsigned char *path;
	unsigned int path_len;
	int record;
	int length;
};

struct req_sim_write_binary {
	guint app_type;
	gchar *aid_str;
	int fileid;
	const unsigned char *path;
	unsigned int path_len;
	int start;
	int length;
	const unsigned char *data;
};

enum req_record_access_mode {
	GRIL_REC_ACCESS_MODE_CURRENT,
	GRIL_REC_ACCESS_MODE_ABSOLUTE,
	GRIL_REC_ACCESS_MODE_NEXT,
	GRIL_REC_ACCESS_MODE_PREVIOUS,
};

struct req_sim_write_record {
	guint app_type;
	gchar *aid_str;
	int fileid;
	const unsigned char *path;
	unsigned int path_len;
	enum req_record_access_mode mode;
	int record;
	int length;
	const unsigned char *data;
};

gboolean g_ril_request_deactivate_data_call(GRil *gril,
				const struct req_deactivate_data_call *req,
				struct parcel *rilp,
				struct ofono_error *error);

void g_ril_request_set_net_select_manual(GRil *gril,
					const char *mccmnc,
					struct parcel *rilp);

gboolean g_ril_request_setup_data_call(GRil *gril,
					const struct req_setup_data_call *req,
					struct parcel *rilp,
					struct ofono_error *error);

gboolean g_ril_request_sim_read_record(GRil *gril,
					const struct req_sim_read_record *req,
					struct parcel *rilp);

gboolean g_ril_request_sim_write_binary(GRil *gril,
					const struct req_sim_write_binary *req,
					struct parcel *rilp);

gboolean g_ril_request_sim_write_record(GRil *gril,
					const struct req_sim_write_record *req,
					struct parcel *rilp);

void g_ril_request_oem_hook_raw(GRil *gril, const void *payload, size_t length,
					struct parcel *rilp);

void g_ril_request_oem_hook_strings(GRil *gril, const char **strs, int num_str,
							struct parcel *rilp);

void g_ril_request_set_initial_attach_apn(GRil *gril, const char *apn,
						int proto,
						const char *user,
						const char *passwd,
						const char *mccmnc,
						struct parcel *rilp);

void g_ril_request_set_uicc_subscription(GRil *gril, int slot_id,
						int app_index,
						int sub_id,
						int sub_status,
						struct parcel *rilp);

#ifdef __cplusplus
}
#endif

#endif /* __GRILREQUEST_H */
