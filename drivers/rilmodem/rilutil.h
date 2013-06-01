/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  Canonical Ltd.
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

#include <stdio.h>

#include "parcel.h"

/* TODO:
 *  Guard with #ifdef RIL_DEBUG
 *  Based on code from:
 *
 *  $AOSP/hardware/ril/libril/ril.cpp
 */
#define ril_start_request           sprintf(print_buf, "(")
#define ril_close_request           sprintf(print_buf, "%s)", print_buf)
#define ril_print_request(token, req)           \
        ofono_debug("[%04d]> %s %s", token, ril_request_id_to_string(req), print_buf)

#define ril_start_response          sprintf(print_buf, "%s {", print_buf)
#define ril_close_response          sprintf(print_buf, "%s}", print_buf)
#define ril_print_response          ofono_debug("%s", print_buf)

#define ril_clear_print_buf         print_buf[0] = 0
#define ril_remove_last_char        print_buf[strlen(print_buf)-1] = 0
#define ril_append_print_buf(x...)  sprintf(print_buf, x)

// request, response, and unsolicited msg print macro
#define PRINT_BUF_SIZE 8096

/* TODO: create a table lookup*/
#define PREFIX_30_NETMASK "255.255.255.252"
#define PREFIX_29_NETMASK "255.255.255.248"
#define PREFIX_28_NETMASK "255.255.255.240"
#define PREFIX_27_NETMASK "255.255.255.224"
#define PREFIX_26_NETMASK "255.255.255.192"
#define PREFIX_25_NETMASK "255.255.255.128"
#define PREFIX_24_NETMASK "255.255.255.0"

enum ril_util_sms_store {
	RIL_UTIL_SMS_STORE_SM =	0,
	RIL_UTIL_SMS_STORE_ME =	1,
	RIL_UTIL_SMS_STORE_MT =	2,
	RIL_UTIL_SMS_STORE_SR =	3,
	RIL_UTIL_SMS_STORE_BM =	4,
};

/* 3GPP TS 27.007 Release 8 Section 5.5 */
enum at_util_charset {
	RIL_UTIL_CHARSET_GSM =		0x1,
	RIL_UTIL_CHARSET_HEX =		0x2,
	RIL_UTIL_CHARSET_IRA =		0x4,
	RIL_UTIL_CHARSET_PCCP437 =	0x8,
	RIL_UTIL_CHARSET_PCDN =		0x10,
	RIL_UTIL_CHARSET_UCS2 =		0x20,
	RIL_UTIL_CHARSET_UTF8 =		0x40,
	RIL_UTIL_CHARSET_8859_1 =	0x80,
	RIL_UTIL_CHARSET_8859_2 =	0x100,
	RIL_UTIL_CHARSET_8859_3 =	0x200,
	RIL_UTIL_CHARSET_8859_4 =	0x400,
	RIL_UTIL_CHARSET_8859_5 =	0x800,
	RIL_UTIL_CHARSET_8859_6 =	0x1000,
	RIL_UTIL_CHARSET_8859_C =	0x2000,
	RIL_UTIL_CHARSET_8859_A =	0x4000,
	RIL_UTIL_CHARSET_8859_G =	0x8000,
	RIL_UTIL_CHARSET_8859_H =	0x10000,
};

struct data_call {
    int             status;
    int             retry;
    int             cid;
    int             active;
    char *          type;
    char *          ifname;
    char *          addresses;
    char *          dnses;
    char *          gateways;
};

struct sim_app {
	char *app_id;
	guint app_type;
};

typedef void (*ril_util_sim_inserted_cb_t)(gboolean present, void *userdata);

void decode_ril_error(struct ofono_error *error, const char *final);
gint ril_util_call_compare_by_status(gconstpointer a, gconstpointer b);
gint ril_util_call_compare_by_phone_number(gconstpointer a, gconstpointer b);
gint ril_util_call_compare_by_id(gconstpointer a, gconstpointer b);
gint ril_util_call_compare(gconstpointer a, gconstpointer b);
gchar *ril_util_get_netmask(const char *address);
void ril_util_init_parcel(struct ril_msg *message, struct parcel *rilp);

struct ril_util_sim_state_query *ril_util_sim_state_query_new(GRil *ril,
						guint interval, guint num_times,
						ril_util_sim_inserted_cb_t cb,
						void *userdata,
						GDestroyNotify destroy);
void ril_util_sim_state_query_free(struct ril_util_sim_state_query *req);

GSList *ril_util_parse_clcc(struct ril_msg *message);
GSList *ril_util_parse_data_call_list(struct ril_msg *message);
char *ril_util_parse_sim_io_rsp(struct ril_msg *message,
				int *sw1, int *sw2,
				int *hex_len);
gboolean ril_util_parse_sim_status(struct ril_msg *message, struct sim_app *app);
gboolean ril_util_parse_reg(struct ril_msg *message, int *status,
				int *lac, int *ci, int *tech, int *max_calls);

gint ril_util_parse_sms_response(struct ril_msg *message);

gint ril_util_get_signal(struct ril_msg *message);

struct cb_data {
	void *cb;
	void *data;
	void *user;
};

static inline struct cb_data *cb_data_new(void *cb, void *data)
{
	struct cb_data *ret;

	ret = g_new0(struct cb_data, 1);
	ret->cb = cb;
	ret->data = data;

	return ret;
}

static inline int ril_util_convert_signal_strength(int strength)
{
	int result;

	if (strength == 99)
		result = -1;
	else
		result = (strength * 100) / 31;

	return result;
}

#define DECLARE_FAILURE(e) 			\
	struct ofono_error e;			\
	e.type = OFONO_ERROR_TYPE_FAILURE;	\
	e.error = 0				\

#define CALLBACK_WITH_FAILURE(cb, args...)		\
	do {						\
		struct ofono_error cb_e;		\
		cb_e.type = OFONO_ERROR_TYPE_FAILURE;	\
		cb_e.error = 0;				\
							\
		cb(&cb_e, ##args);			\
	} while (0)					\

#define CALLBACK_WITH_SUCCESS(f, args...)		\
	do {						\
		struct ofono_error e;			\
		e.type = OFONO_ERROR_TYPE_NO_ERROR;	\
		e.error = 0;				\
		f(&e, ##args);				\
	} while (0)
