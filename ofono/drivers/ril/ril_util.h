/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2021 Jolla Ltd.
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

#ifndef RIL_UTIL_H
#define RIL_UTIL_H

#include "ril_types.h"

#include <ofono/gprs-context.h>

struct ofono_network_operator;

const char *ril_error_to_string(int error);
const char *ril_request_to_string(guint request);
const char *ril_unsol_event_to_string(guint event);
const char *ril_radio_state_to_string(int radio_state);
const char *ril_protocol_from_ofono(enum ofono_gprs_proto proto);
int ril_protocol_to_ofono(const char *str);
enum ril_auth ril_auth_method_from_ofono(enum ofono_gprs_auth_method auth);
enum ofono_access_technology ril_parse_tech(const char *stech, int *ril_tech);
gboolean ril_parse_mcc_mnc(const char *str, struct ofono_network_operator *op);

#define ril_error_init_ok(err) \
	((err)->error = 0, (err)->type = OFONO_ERROR_TYPE_NO_ERROR)
#define ril_error_init_failure(err) \
	((err)->error = 0, (err)->type = OFONO_ERROR_TYPE_FAILURE)
#define ril_error_init_sim_error(err,sw1,sw2) \
	((err)->error = ((sw1) << 8)|(sw2), (err)->type = OFONO_ERROR_TYPE_SIM)

#define ril_error_ok(err) (ril_error_init_ok(err), err)
#define ril_error_failure(err) (ril_error_init_failure(err), err)
#define ril_error_sim(err,sw1,sw2) (ril_error_init_sim_error(err,sw1,sw2), err)

char *ril_encode_hex(const void *in, guint size);
void *ril_decode_hex(const char *hex, int len, guint *out_size);

#endif /* RIL_UTIL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
