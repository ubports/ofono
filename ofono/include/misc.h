/*
 *  oFono - Open Source Telephony
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

#ifndef __OFONO_MISC_H
#define __OFONO_MISC_H

/*
 * Miscellaneous utilities which do not fall into any other category.
 *
 * This file exists since mer/1.24+git2
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/netreg.h>

const char *ofono_netreg_status_to_string(enum ofono_netreg_status status);
const char *ofono_access_technology_to_string(enum ofono_access_technology t);

char *ofono_sim_string_to_utf8(const unsigned char *buffer, int length);
void ofono_sim_string_free(char *str);

void ofono_encode_hex(const void *in, unsigned int n, char out[/* 2*n+1 */]);

#define OFONO_UNPACK_7BIT_USSD (0x01) /* flags */
unsigned int ofono_unpack_7bit(const void *in, unsigned int len,
		unsigned int flags, void *out_buf, unsigned int out_buf_size);

#define OFONO_PHONE_NUMBER_BUFFER_SIZE (OFONO_MAX_PHONE_NUMBER_LENGTH + 2)
const char *ofono_phone_number_to_string(const struct ofono_phone_number *ph,
			char buffer[/* OFONO_PHONE_NUMBER_BUFFER_SIZE */]);

#define OFONO_EF_PATH_BUFFER_SIZE 6
unsigned int ofono_get_ef_path_2g(unsigned short id,
			unsigned char path[/* OFONO_EF_PATH_BUFFER_SIZE */]);
unsigned int ofono_get_ef_path_3g(unsigned short id,
			unsigned char path[/* OFONO_EF_PATH_BUFFER_SIZE */]);
ofono_bool_t ofono_parse_get_response_2g(const void *response, unsigned int len,
			unsigned int *file_len, unsigned int *record_len,
			unsigned int *structure, unsigned char *access,
			unsigned char *file_status);
ofono_bool_t ofono_parse_get_response_3g(const void *response, unsigned int len,
			unsigned int *file_len, unsigned int *record_len,
			unsigned int *structure, unsigned char *access,
			unsigned short *efid);
ofono_bool_t ofono_decode_cbs_dcs_charset(unsigned char dcs,
			enum ofono_sms_charset *charset);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_MISC_H */
