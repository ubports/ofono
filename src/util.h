/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <stdbool.h>

enum gsm_dialect {
	GSM_DIALECT_DEFAULT = 0,
	GSM_DIALECT_TURKISH,
	GSM_DIALECT_SPANISH,
	GSM_DIALECT_PORTUGUESE,
	GSM_DIALECT_BENGALI,
	GSM_DIALECT_GUJARATI,
	GSM_DIALECT_HINDI,
	GSM_DIALECT_KANNADA,
	GSM_DIALECT_MALAYALAM,
	GSM_DIALECT_ORIYA,
	GSM_DIALECT_PUNJABI,
	GSM_DIALECT_TAMIL,
	GSM_DIALECT_TELUGU,
	GSM_DIALECT_URDU,
};

char *convert_gsm_to_utf8(const unsigned char *text, long len, long *items_read,
				long *items_written, unsigned char terminator);

char *convert_gsm_to_utf8_with_lang(const unsigned char *text, long len,
					long *items_read, long *items_written,
					unsigned char terminator,
					enum gsm_dialect locking_shift_lang,
					enum gsm_dialect single_shift_lang);

unsigned char *convert_utf8_to_gsm(const char *text, long len, long *items_read,
				long *items_written, unsigned char terminator);

unsigned char *convert_utf8_to_gsm_with_lang(const char *text, long len,
					long *items_read, long *items_written,
					unsigned char terminator,
					enum gsm_dialect locking_shift_lang,
					enum gsm_dialect single_shift_lang);

unsigned char *convert_utf8_to_gsm_best_lang(const char *utf8, long len,
					long *items_read, long *items_written,
					unsigned char terminator,
					enum gsm_dialect hint,
					enum gsm_dialect *used_locking,
					enum gsm_dialect *used_single);

unsigned char *decode_hex_own_buf(const char *in, long len, long *items_written,
					unsigned char terminator,
					unsigned char *buf);

char *encode_hex_own_buf(const unsigned char *in, long len,
				unsigned char terminator, char *buf);

unsigned char *unpack_7bit_own_buf(const unsigned char *in, long len,
					int byte_offset, bool ussd,
					long max_to_unpack, long *items_written,
					unsigned char terminator,
					unsigned char *buf);

unsigned char *unpack_7bit(const unsigned char *in, long len, int byte_offset,
				bool ussd, long max_to_unpack,
				long *items_written, unsigned char terminator);

unsigned char *pack_7bit_own_buf(const unsigned char *in, long len,
					int byte_offset, bool ussd,
					long *items_written,
					unsigned char terminator,
					unsigned char *buf);

unsigned char *pack_7bit(const unsigned char *in, long len, int byte_offset,
				bool ussd,
				long *items_written, unsigned char terminator);

char *sim_string_to_utf8(const unsigned char *buffer, int length);

unsigned char *utf8_to_sim_string(const char *utf,
					int max_length, int *out_length);

unsigned char *convert_ucs2_to_gsm_with_lang(const unsigned char *text,
						long len, long *items_read,
						long *items_written,
						unsigned char terminator,
						enum gsm_dialect locking_lang,
						enum gsm_dialect single_lang);

unsigned char *convert_ucs2_to_gsm(const unsigned char *text, long len,
					long *items_read, long *items_written,
					unsigned char terminator);
