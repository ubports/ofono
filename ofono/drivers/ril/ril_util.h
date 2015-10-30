/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
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
 */

#ifndef RIL_UTIL_H
#define RIL_UTIL_H

#include "ril_types.h"

#include <ofono/log.h>

struct ril_reg_data {
	int ril_status;
	int ril_tech;
	int status;        /* enum network_registration_status or -1 if none */
	int access_tech;   /* enum access_technology or -1 if none */
	int lac;
	int ci;
	int max_calls;
};

const char *ril_error_to_string(int error);
const char *ril_request_to_string(guint request);
const char *ril_unsol_event_to_string(guint event);
const char *ril_radio_state_to_string(int radio_state);
int ril_parse_tech(const char *stech, int *ril_tech);
int ril_address_family(const char *addr);
gboolean ril_util_parse_reg(const void *data, guint len,
						struct ril_reg_data *parsed);

#define ril_error_init_ok(err) \
	((err)->error = 0, (err)->type = OFONO_ERROR_TYPE_NO_ERROR)
#define ril_error_init_failure(err) \
	((err)->error = 0, (err)->type = OFONO_ERROR_TYPE_FAILURE)

#define ril_error_ok(err) (ril_error_init_ok(err), err)
#define ril_error_failure(err) (ril_error_init_failure(err), err)

#endif /* RIL_UTIL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
