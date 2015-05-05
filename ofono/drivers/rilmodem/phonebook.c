/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) ST-Ericsson SA 2010.
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/phonebook.h>
#include <sim.h>
#include <simfs.h>
#include <util.h>

#include "gril.h"
#include "grilutil.h"
#include "simutil.h"
#include "common.h"

#include "rilmodem.h"

#include "ril_constants.h"

/* File info parameters */
#define FCP_TEMPLATE				0x62
#define FCP_FILE_SIZE				0x80
#define FCP_FILE_DESC				0x82
#define FCP_FILE_ID					0x83
#define FCP_FILE_LIFECYCLE			0x8A
#define FCP_FILE_SECURITY_ARR		0x8B
#define FCP_FILE_SECURITY_COMPACT	0x8C
#define FCP_FILE_SECURITY_EXPANDED	0xAB

#define SIM_EFPBR_FILEID			0x4F30

#define UNUSED						0xff

#define EXT1_CP_SUBADDRESS			1
#define EXT1_ADDITIONAL_DATA		2

#define NAME_SIZE					64
#define NUMBER_SIZE					256
#define EMAIL_SIZE					128
#define EXT_NUMBER_SIZE				24
#define SNE_SIZE					64

/* TON (Type Of Number) See TS 24.008 */
#define TON_MASK					0x70
#define TON_INTERNATIONAL			0x10

enum constructed_tag {
	TYPE_1_TAG = 0xA8,
	TYPE_2_TAG = 0xA9,
	TYPE_3_TAG = 0xAA
};

enum file_type_tag {
	TYPE_ADN = 0xC0,
	TYPE_IAD = 0xC1,
	TYPE_EXT1 = 0xC2,
	TYPE_SNE = 0xC3,
	TYPE_ANR = 0xC4,
	TYPE_PBC = 0xC5,
	TYPE_GPR = 0xC6,
	TYPE_AAS = 0xC7,
	TYPE_GAS = 0xC8,
	TYPE_UID = 0xC9,
	TYPE_EMAIL = 0xCA,
	TYPE_CCP1 = 0xCB
};

struct pb_file_info {
	int file_id;
	uint8_t file_type;
	uint8_t structure;
	int file_length;
	int record_length;
	int record;
	gboolean handled;
};

struct file_info {
	int fileid;
	int length;
	int structure;
	int record_length;
	unsigned char access[3];
};

struct phonebook_entry {
	int entry;
	char *name;
	char *number;
	char *email;
	char *anr;
	char *sne;
};

unsigned char sim_path[4] = {0x3F, 0x00, 0x7F, 0x10};
unsigned char usim_path[6] = {0x3F, 0x00, 0x7F, 0x10, 0x5F, 0x3A};
static const char digit_to_utf8[] = "0123456789*#pwe\0";

struct pb_data {
	GRil *ril;
	struct ofono_sim_driver *sim_driver;
	gint pb_entry;
	struct pb_file_info pb_reference_file_info;
	struct pb_file_info *extension_file_info;
	uint8_t ext1_to_type;
	uint8_t ext1_to_entry;
	guint timer_id;
};

static GSList *pb_files;
static GSList *pb_next;

static GSList *phonebook_entry_start;
static GSList *phonebook_entry_current;

static void pb_reference_info_cb(const struct ofono_error *error,
					int filelength,
					enum ofono_sim_file_structure structure,
					int recordlength,
					const unsigned char access[3],
					unsigned char file_status, void *data);

static void pb_content_data_read(struct pb_data *pbd,
					struct pb_file_info *file_info,
					struct cb_data *cbd);

void handle_adn(size_t len, char *name, const unsigned char *msg,
		char *number, struct pb_file_info *next_file,
		struct pb_data *pbd)
{
	uint8_t name_length;
	uint8_t number_start;
	uint8_t number_length = 0;
	uint8_t extension_record = UNUSED;
	uint8_t i, prefix;

	if (len < 14)
		return;

	name_length = len - 14;
	number_start = name_length;

	name = sim_string_to_utf8(msg, name_length);
	/* Length contains also TON&NPI */
	number_length = msg[number_start];

	if ((number_length != UNUSED) && (number_length != 0)) {
		number = g_try_malloc0(NUMBER_SIZE);
		number_length--;

		if (number) {
			prefix = 0;

			if ((msg[number_start + 1] & TON_MASK)
					== TON_INTERNATIONAL) {
				number[0] = '+';
				prefix = 1;
			}

			for (i = 0; i < number_length; i++) {

				number[2 * i + prefix] =
					digit_to_utf8[msg
						[number_start
						+ 2 +
						i] & 0x0f];
					number[2 * i + 1 + prefix] =
					digit_to_utf8[(msg
						[number_start
						+ 2 +
						i] >> 4) &
							0x0f];
			}

			extension_record = msg[len - 1];
		}
	}

	DBG("ADN name %s, number %s ", name, number);
	DBG("length %d extension_record %d",
		number_length, extension_record);

	if (extension_record != UNUSED) {
		next_file = g_try_new0(struct pb_file_info, 1);
		if (next_file) {
			if (pbd->extension_file_info) {
				memmove(next_file,
					pbd->
					extension_file_info,
					sizeof(struct
						pb_file_info));
			} else {
				next_file->file_type =
					TYPE_EXT1;
				next_file->file_id =
					SIM_EFEXT1_FILEID;
			}

			next_file->record = extension_record;
			pbd->ext1_to_type = TYPE_ADN;
			pbd->ext1_to_entry = pbd->pb_entry;
		}
	}

	if (name || number) {
		struct phonebook_entry *new_entry =
			g_try_new0(struct phonebook_entry, 1);

		if (new_entry) {
			new_entry->name = name;
			new_entry->number = number;

			DBG("Creating PB entry %d with", pbd->pb_entry);
			DBG("name %s and number %s",
				new_entry->name, new_entry->number);

			phonebook_entry_current =
				g_slist_insert
				(phonebook_entry_start,
					new_entry,
					pbd->pb_entry);

			if (!phonebook_entry_start)
				phonebook_entry_start =
				phonebook_entry_current;

			pbd->pb_entry++;
		}
	}
}

void handle_sne(size_t len, const unsigned char *msg, char *sne)
{
	uint8_t sne_length;
	uint8_t phonebook_entry_nbr;

	DBG("SNE");

	if (len < 2)
		return;

	sne_length = len - 2;
	phonebook_entry_nbr = msg[len - 1];

	sne = sim_string_to_utf8(msg, sne_length);

	if (sne) {
		/* GSlist nth counts from 0,
			PB entries from 1 */
		GSList *list_entry =
			g_slist_nth(phonebook_entry_start,
				phonebook_entry_nbr - 1);

		DBG("SNE \'%s\' to PB entry %d", sne,
			phonebook_entry_nbr);

		if (list_entry) {
			struct phonebook_entry *entry =
				list_entry->data;

			if (entry) {
				DBG("Adding SNE to entry %d",
					phonebook_entry_nbr);
				DBG("name %s", entry->name);

				g_free(entry->sne);
				entry->sne = sne;
				return;
			}
		}

		g_free(sne);
	}
}

void handle_anr(size_t len,const unsigned char *msg,char *anr,
			struct pb_file_info *next_file, struct pb_data *pbd)
{
	uint8_t number_length = 0;
	uint8_t extension_record = UNUSED;
	uint8_t aas_record = UNUSED;
	uint8_t i, prefix;
	uint8_t phonebook_entry_nbr;
	GSList *list_entry;

	DBG("ANR");

	if (!msg)
		return;

	if (len < 1)
		return;

	phonebook_entry_nbr = msg[len - 1];

	if (msg[0] == UNUSED)
		return;

	aas_record = msg[0];
	/* Length contains also TON&NPI */
	number_length = msg[1];

	if (number_length) {
		number_length--;
		anr = g_try_malloc0(NUMBER_SIZE);

		if (anr) {
			prefix = 0;

			if ((msg[2] & TON_MASK) ==
				TON_INTERNATIONAL) {
				anr[0] = '+';
				prefix = 1;
			}

			for (i = 0; i < number_length; i++) {
				anr[2 * i + prefix] =
					digit_to_utf8[msg[3 + i] &
							0x0f];
				anr[2 * i + 1 + prefix] =
					digit_to_utf8[(msg[3 + i] >>
						4) & 0x0f];
			}

			extension_record = msg[len - 3];
		}
	}

	DBG("ANR to entry %d number %s number length %d",
		phonebook_entry_nbr, anr, number_length);
	DBG("extension_record %d aas %d",
			extension_record, aas_record);

	if (extension_record != UNUSED) {
		next_file = g_try_new0(struct pb_file_info, 1);

		if (next_file) {
			if (pbd->extension_file_info) {
				memmove(next_file,
					pbd->
					extension_file_info,
					sizeof(struct
						pb_file_info));
			} else {
				next_file->file_type =
					TYPE_EXT1;
				next_file->file_id =
					SIM_EFEXT1_FILEID;
			}

			next_file->record = extension_record;
			pbd->ext1_to_type = TYPE_ANR;
			pbd->ext1_to_entry =
				phonebook_entry_nbr;
		}
	}

	/* GSlist nth counts from 0, PB entries from 1 */
	list_entry =
		g_slist_nth(phonebook_entry_start,
			phonebook_entry_nbr - 1);

	if (list_entry) {
		struct phonebook_entry *entry =
			list_entry->data;

		if (entry) {
			/* if one already exists, delete it */
			if (entry->anr)
				g_free(entry->anr);
				DBG("Adding ANR to entry %d, name %s",
					phonebook_entry_nbr,
						entry->name);
			entry->anr = anr;
		}
	} else {
		g_free(anr);
	}
}

void handle_email(size_t len, const unsigned char *msg, char *email)
{
	uint8_t phonebook_entry_nbr;

	if (!msg)
		return;

	if (len < 1)
		return;

	phonebook_entry_nbr = msg[len - 1];

	email = sim_string_to_utf8(msg, len - 2);

	/* GSlist nth counts from 0, PB entries from 1 */
	if (email) {
		GSList *list_entry =
			g_slist_nth(phonebook_entry_start,
				phonebook_entry_nbr - 1);

		DBG("Email \'%s\' to PB entry %d", email,
			phonebook_entry_nbr);
		if (list_entry) {
			struct phonebook_entry *entry =
				list_entry->data;

			/* if one already exists, delete it */
			if (entry) {
				if (entry->email)
					g_free(entry->email);

				DBG("Adding email to entry %d",
					phonebook_entry_nbr);
				DBG("name %s", entry->name);

				entry->email = email;
			}
		} else {
			g_free(email);
		}
	}
}

void handle_ext1(struct pb_data *pbd, const unsigned char *msg,
			char *ext_number, struct pb_file_info *next_file)
{
	uint8_t number_length, i, next_extension_record;

	if (!msg)
		return;

	number_length = msg[1];

	for (i = 0; i < number_length; i++) {
		ext_number[2 * i] =
			digit_to_utf8[msg[2 + i] &
					0x0f];
		ext_number[2 * i + 1] =
			digit_to_utf8[(msg[2 + i] >>
					4) & 0x0f];
	}

	next_extension_record =
		msg[number_length + 2];

	DBG("Number extension %s", ext_number);
	DBG("number length %d", number_length);
	DBG("extension_record %d",
		next_extension_record);

	/* pb_entry is already incremented
		& g_slist_nth counts from 0 */
	if (pbd->ext1_to_type == TYPE_ADN) {
		GSList *list_entry =
			g_slist_nth
			(phonebook_entry_start,
			pbd->ext1_to_entry - 1);
		DBG("Looking for ADN entry %d",
			pbd->ext1_to_entry);

		if (list_entry) {
			struct phonebook_entry
				*entry =
				list_entry->data;
			if (entry) {
				strcat(entry->
					number,
					ext_number);
			}
		}
	} else if (pbd->ext1_to_type == TYPE_ANR) {
		GSList *list_entry =
			g_slist_nth
			(phonebook_entry_start,
			pbd->ext1_to_entry - 1);
		DBG("Looking for ANR entry %d",
			pbd->ext1_to_entry);
			if (list_entry) {
				struct phonebook_entry
					*entry =
						list_entry->data;
				if (entry) {
					strcat(entry->anr,
						ext_number);
			}
		}
	}

	g_free(ext_number);

	/* Check if there is
	more extension data */
	if (next_extension_record != UNUSED) {
		next_file =
			g_try_new0(struct
				pb_file_info, 1);

		if (next_file) {
			if (pbd->extension_file_info) {
				memmove
				(next_file,
				pbd->
				extension_file_info,
				sizeof
				(struct
				pb_file_info));
			} else {
				next_file->
					file_type =
					TYPE_EXT1;
				next_file->
					file_id =
					SIM_EFEXT1_FILEID;
				}

			next_file->record =
				next_extension_record;
		}
	}
}

static struct pb_file_info *decode_read_response(struct pb_file_info *file_info,
						 const unsigned char *msg,
						 size_t len,
						 struct ofono_phonebook *pb)
{

	char *name = NULL;
	char *number = NULL;
	char *ext_number = NULL;
	char *email = NULL;
	char *sne = NULL;
	char *anr = NULL;

	struct pb_file_info *next_file = NULL;
	int type = file_info->file_type;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);

	switch (type) {
	case TYPE_ADN:{
		handle_adn(len, name, msg, number, next_file, pbd);
		break;
		}
	case TYPE_SNE:{
		handle_sne(len, msg, sne);
		break;
		}
	case TYPE_ANR:{
		handle_anr(len, msg, anr, next_file, pbd);
		break;
		}
	case TYPE_AAS:{
		DBG("AAS");
		break;
	}
	case TYPE_EMAIL:{
		handle_email(len, msg, email);
		break;
	}
	case TYPE_EXT1:{
		DBG("EXT1 to type=%02X, entry=%d", pbd->ext1_to_type,
		pbd->ext1_to_entry);

		if (msg[0] == EXT1_ADDITIONAL_DATA) {
			ext_number = g_try_malloc0(EXT_NUMBER_SIZE);

			if (ext_number)
				handle_ext1(pbd, msg, ext_number, next_file);
		}
		break;
	}
	default:{
		DBG("Skipping type %02X", type);
		break;
	}
}
	return next_file;
}

struct pb_file_info *extension_file_info;

static void pb_adn_sim_data_cb(const struct ofono_error *error,
				const unsigned char *sdata,
				int length, void *data)
{
	struct cb_data *cbd_outer = data;
	struct cb_data *cbd = NULL;
	struct pb_file_info *file_info;
	struct ofono_phonebook *pb;
	ofono_phonebook_cb_t cb;
	struct pb_data *pbd;

	DBG("");
	if (!cbd_outer)
		return;

	file_info = cbd_outer->user;
	cbd = cbd_outer->data;

	if (!cbd) {
		g_free(cbd_outer);
		return;
		}

	pb = cbd->user;
	cb = cbd->cb;
	pbd = ofono_phonebook_get_data(pb);

	if (extension_file_info)
		file_info =
			decode_read_response(extension_file_info, sdata, length,
						pb);
	else
		file_info = decode_read_response(file_info, sdata, length, pb);

	if (file_info) {
		DBG("Reading extension file %04X, record %d",
			file_info->file_id, file_info->record);
		pbd->sim_driver->read_file_linear(get_sim(), file_info->file_id,
						file_info->record,
						file_info->record_length,
						sim_path, sizeof(sim_path),
						pb_adn_sim_data_cb, cbd_outer);

		/* Delete if there is a previous one */
		g_free(extension_file_info);
		extension_file_info = file_info;
		return;
	} else {
		g_free(extension_file_info);
		extension_file_info = NULL;
		file_info = cbd_outer->user;

		if (file_info->record <
			(file_info->file_length / file_info->record_length)) {

			file_info->record++;
			DBG("Same file, next record %d", file_info->record);
			pbd->sim_driver->read_file_linear(get_sim(),
						file_info->file_id,
						file_info->record,
						file_info->record_length,
						sim_path, sizeof(sim_path),
						pb_adn_sim_data_cb,
						cbd_outer);
		} else {
			GSList *list_entry =
				g_slist_nth(phonebook_entry_start, 0);
			DBG("All data requested, start vCard creation");
			g_free(file_info);

			while (list_entry) {
				struct phonebook_entry *entry =
					list_entry->data;

				if (entry) {
					DBG("vCard:\nname=%s\nnumber=%s",
						entry->name, entry->number);
					DBG("email=%s\nanr=%s\nsne=%s",
						entry->email,
						entry->anr,
						entry->sne);

					ofono_phonebook_entry(pb, -1,
							entry->number, -1,
							entry->name, -1,
							NULL,
							entry->anr, -1,
							entry->sne,
							entry->email,
							NULL, NULL);
					g_free(entry->number);
					g_free(entry->name);
					g_free(entry->anr);
					g_free(entry->sne);
					g_free(entry->email);
					g_free(entry);
				}

				list_entry = g_slist_next(list_entry);
			}

			g_slist_free(phonebook_entry_start);
			g_slist_free(pb_files);
			g_free(cbd_outer);
			void *pb = cbd->data;
			g_free(cbd);
			DBG("Finally all PB data read");
			CALLBACK_WITH_SUCCESS(cb, pb);
			return;
		}
	}
}

static void pb_adn_sim_info_cb(const struct ofono_error *error,
				int filelength,
				enum ofono_sim_file_structure structure,
				int recordlength,
				const unsigned char access[3],
				unsigned char file_status, void *data)
{
	struct cb_data *cbd = data;
	struct ofono_phonebook *pb = cbd->user;
	ofono_phonebook_cb_t cb = cbd->cb;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	struct pb_file_info *file_info = NULL;
	struct cb_data *cbd_outer;
	int records = 0;

	DBG("");
	if (!cbd)
		goto error;

	file_info = NULL;

	if (!pbd)
		goto error;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto error;

	if (structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		goto error;

	if (!pbd->sim_driver->read_file_linear)
		goto error;

	records = filelength / recordlength;

	if (!records)
		goto error;

	file_info = g_try_new0(struct pb_file_info, 1);

	if (!file_info)
		goto error;

	file_info->file_id = SIM_EFADN_FILEID;
	file_info->file_type = TYPE_ADN;
	file_info->structure = structure;
	file_info->file_length = filelength;
	file_info->record_length = recordlength;
	file_info->record = 1;
	/* Regenerate cbd (include file_info) */
	cbd_outer = cb_data_new2(file_info, cb, cbd);

	pbd->sim_driver->read_file_linear(get_sim(),
					file_info->file_id,
					file_info->record,
					file_info->record_length,
					sim_path, sizeof(sim_path),
					pb_adn_sim_data_cb, cbd_outer);
	return;
error:

	if (cbd){
		void *pb = cbd->data;
		g_free(cbd);
		if(cb)
			CALLBACK_WITH_FAILURE(cb, pb);
	}
}

static gboolean is_reading_required(uint8_t file_type)
{
	switch (file_type) {
	case TYPE_ADN:
	case TYPE_EMAIL:
	case TYPE_SNE:
	case TYPE_ANR:
		return TRUE;
	default:
		return FALSE;
	}
}

static void pb_content_data_cb(const struct ofono_error *error,
				const unsigned char *sdata,
				int length, void *data)
{
	struct cb_data *cbd = data;
	struct ofono_phonebook *pb;
	ofono_phonebook_cb_t cb;
	struct pb_data *pbd;
	struct pb_file_info *file_info = NULL;

	pb = cbd->user;
	cb = cbd->cb;
	pbd = ofono_phonebook_get_data(pb);

	if (extension_file_info)
		file_info = decode_read_response(extension_file_info, sdata,
						 length, pb);
	else {
	/*
	 * These checks are crash hacks.
	 * AFAIK there's a possibility that we end up here and pb_next is NULL
	 * in case remove has been called while phonebook reading is in
	 * process. If you find better solution to this issue feel free to
	 * change this.
	 */
		if (pb_next == NULL) {
			ofono_error("phonebook reading failed");
			if (cbd){
				void *pb = cbd->data;
				g_free(cbd);
				if(cb && pbd)
					CALLBACK_WITH_FAILURE(cb, pb);
				}
			return;
		}

		file_info =
			decode_read_response(pb_next->data, sdata, length, pb);
	}

	if (file_info) {
		DBG("Reading extension file %04X, record %d, structure %d",
			file_info->file_id, file_info->record,
			file_info->structure);
		pb_content_data_read(pbd, file_info, cbd);
		/* Delete if there is a previous one */
		g_free(extension_file_info);
		extension_file_info = file_info;
		return;
	} else {
		g_free(extension_file_info);
		extension_file_info = NULL;
		file_info = pb_next->data;

		if (((file_info->structure ==
			OFONO_SIM_FILE_STRUCTURE_FIXED) ||
			(file_info->structure ==
			 OFONO_SIM_FILE_STRUCTURE_CYCLIC))
			&& (file_info->record <
			(file_info->file_length / file_info->record_length))) {

			file_info->record++;
			DBG("Same file, next record %d", file_info->record);
		} else {
			g_free(file_info);
			pb_next = g_slist_next(pb_next);
			DBG("Next file in list");

			if (pb_next) {
				file_info = pb_next->data;

				while (pb_next
						&&
						(!is_reading_required
						(file_info->file_type))) {
					DBG("Skipping file type %02X",
						file_info->file_type);
					g_free(file_info);
					pb_next = g_slist_next(pb_next);

					if (pb_next)
						file_info = pb_next->data;
				}
			}

			if (pb_next == NULL) {
				GSList *list_entry =
					g_slist_nth(phonebook_entry_start, 0);

				DBG("All data requested, start vCard creation");
				while (list_entry) {
					struct phonebook_entry *entry =
							list_entry->data;

					if (entry) {
						DBG("vCard:\nname=%s\n",
							entry->name);
						DBG("number=%s\nemail=%s\n",
							entry->number,
							entry->email);
						DBG("anr=%s\nsne=%s",
							entry->anr, entry->sne);

						ofono_phonebook_entry(pb, -1,
								entry->number,
								-1,
								entry->name,
								-1,
								NULL,
								entry->anr,
								-1,
								entry->sne,
								entry->email,
								NULL,
								NULL);

						g_free(entry->number);
						g_free(entry->name);
						g_free(entry->anr);
						g_free(entry->sne);
						g_free(entry->email);
						g_free(entry);
					}

					list_entry = g_slist_next(list_entry);
				}

				g_slist_free(phonebook_entry_start);
				g_slist_free(pb_files);
				void *pb = cbd->data;
				g_free(cbd);
				DBG("Finally all PB data read");
				CALLBACK_WITH_SUCCESS(cb, pb);
				return;
			}

			file_info = pb_next->data;
		}
	}

	pb_content_data_read(pbd, file_info, cbd);
}

static void pb_content_data_read(struct pb_data *pbd,
					struct pb_file_info *file_info,
					struct cb_data *cbd)
{
	ofono_phonebook_cb_t cb;

	if (!pbd || !file_info || !cbd)
		goto out;

	cb = cbd->cb;
	DBG("Reading content of file type=%02X, file ID=%04X, structure=%d",
		file_info->file_type, file_info->file_id, file_info->structure);

	switch (file_info->structure) {
	case OFONO_SIM_FILE_STRUCTURE_FIXED:

		if (!pbd->sim_driver->read_file_linear)
			goto error;

		pbd->sim_driver->read_file_linear(get_sim(), file_info->file_id,
						file_info->record,
						file_info->record_length,
						usim_path, sizeof(usim_path),
						pb_content_data_cb, cbd);
		break;
	case OFONO_SIM_FILE_STRUCTURE_CYCLIC:

		if (!pbd->sim_driver->read_file_cyclic)
			goto error;

		pbd->sim_driver->read_file_cyclic(get_sim(), file_info->file_id,
						file_info->record,
						file_info->record_length,
						NULL, 0,
						pb_content_data_cb, cbd);
		break;
	case OFONO_SIM_FILE_STRUCTURE_TRANSPARENT:

		if (!pbd->sim_driver->read_file_transparent)
			goto error;

		pbd->sim_driver->read_file_transparent(get_sim(),
						file_info->file_id, 0,
						file_info->file_length,
						usim_path, sizeof(usim_path),
						pb_content_data_cb,
						cbd);

		break;
	}

	return;
error:

	if (cbd){
		void *pb = cbd->data;
		g_free(cbd);
		if(cb)
			CALLBACK_WITH_FAILURE(cb, pb);
	}

out:
	DBG("Exiting");
}

static void pb_content_info_cb(const struct ofono_error *error,
				int filelength,
				enum ofono_sim_file_structure structure,
				int recordlength,
				const unsigned char access[3],
				unsigned char file_status, void *data)
{
	struct cb_data *cbd = data;
	struct ofono_phonebook *pb;
	ofono_phonebook_cb_t cb;
	struct pb_data *pbd;
	struct pb_file_info *file_info = NULL;

	if (!cbd)
		goto error;

	pb = cbd->user;
	cb = cbd->cb;
	pbd = ofono_phonebook_get_data(pb);

	if (!pbd)
		goto error;

	file_info = pb_next->data;

	if (!file_info)
		goto error;

	file_info->structure = structure;
	file_info->file_length = filelength;
	file_info->record_length = recordlength;
	file_info->record = 1;

	DBG("File type=%02X, File ID=%04X, Struct=%d, File len=%d, Rec len=%d",
		file_info->file_type, file_info->file_id, file_info->structure,
		file_info->file_length, file_info->record_length);

	if (file_info->file_type == TYPE_EXT1)
		/* Save for quick access */
		pbd->extension_file_info = file_info;

	pb_next = g_slist_next(pb_next);

	if (pb_next == NULL) {
		DBG("All info requested, start content reading");

		/* Re-start from beginning */
		pb_next = g_slist_nth(pb_files, 0);
		file_info = pb_next->data;

		DBG("Calling pb_content_data_read pb=%p, list=%p, type=%02X",
			cbd->user, pb_next, file_info->file_type);

		pb_content_data_read(pbd, file_info, cbd);
		return;
	}

	file_info = pb_next->data;

	DBG("Reading next content info %04X", file_info->file_id);

	pbd->sim_driver->read_file_info(get_sim(), file_info->file_id,
					usim_path, sizeof(usim_path),
					pb_content_info_cb, cbd);
	return;
error:

	if (cbd){
		void *pb = cbd->data;
		g_free(cbd);
		if(cb)
			CALLBACK_WITH_FAILURE(cb, pb);
	}
}

static void pb_reference_data_cb(const struct ofono_error *error,
					const unsigned char *sdata,
					int length, void *data)
{
	struct cb_data *cbd = data;
	struct ofono_phonebook *pb;
	ofono_phonebook_cb_t cb;
	struct pb_data *pbd;
	const unsigned char *ptr = sdata;
	int typelen = 0;
	int i = 0;
	int file_id = 0;
	gboolean finished = FALSE;

	if (!cbd)
		goto error;

	pb = cbd->user;
	cb = cbd->cb;

	pbd = ofono_phonebook_get_data(pb);

	if (!pbd)
		goto error;

	while ((ptr < sdata + length) && (finished == FALSE)) {
		switch (*ptr) {
		case TYPE_1_TAG:
		case TYPE_2_TAG:
		case TYPE_3_TAG:
			typelen = *(ptr + 1);
			DBG("File type=%02X, len=%d", *ptr, typelen);
			ptr += 2;
			i = 0;

			while (i < typelen) {
				struct pb_file_info *file_info =
					g_try_new0(struct pb_file_info, 1);
				file_id = (ptr[i + 2] << 8) + ptr[i + 3];

				DBG("creating file info for File type=%02X",
					ptr[i]);
				DBG("File ID=%04X", file_id);

				if (!file_info)
					goto error;

				file_info->file_type = ptr[i];
				file_info->file_id = file_id;
				pb_files =
					g_slist_append(pb_files,
							(void *)file_info);
				i += ptr[i + 1] + 2;
			}

			ptr += typelen;
			break;
		default:
			DBG("All handled %02x", *ptr);
			finished = TRUE;
			break;
		}
	}

	if (pbd->pb_reference_file_info.record <
			(pbd->pb_reference_file_info.file_length /
			pbd->pb_reference_file_info.record_length)) {
		pbd->pb_reference_file_info.record++;
		DBG("Next EFpbr record %d", pbd->pb_reference_file_info.record);
		if (RIL_APPTYPE_SIM == ril_get_app_type()) {
			pbd->sim_driver->read_file_linear(get_sim(),
					pbd->pb_reference_file_info.
					file_id,
					pbd->pb_reference_file_info.
					record,
					pbd->pb_reference_file_info.
					record_length,
					sim_path, sizeof(sim_path),
					pb_reference_data_cb, cbd);
		} else {
			pbd->sim_driver->read_file_linear(get_sim(),
					pbd->pb_reference_file_info.
					file_id,
					pbd->pb_reference_file_info.
					record,
					pbd->pb_reference_file_info.
					record_length,
					usim_path, sizeof(usim_path),
					pb_reference_data_cb, cbd);
		}
	} else {
		struct pb_file_info *file_info;
		DBG("All EFpbr records read");
		pb_next = g_slist_nth(pb_files, 0);

		if (!pb_next)
			goto error;

		file_info = pb_next->data;

		if (!file_info || !pbd->sim_driver)
			goto error;

		pbd->sim_driver->read_file_info(get_sim(), file_info->file_id,
						usim_path, sizeof(usim_path),
						pb_content_info_cb, cbd);
	}

	return;
error:

	if (cbd){
		void *pb = cbd->data;
		g_free(cbd);
		if(cb)
			CALLBACK_WITH_FAILURE(cb, pb);
	}
}

static void pb_reference_info_cb(const struct ofono_error *error,
					int filelength,
					enum ofono_sim_file_structure structure,
					int recordlength,
					const unsigned char access[3],
					unsigned char file_status,
					void *data)
{

	struct cb_data *cbd = data;
	struct ofono_phonebook *pb = cbd->user;
	ofono_phonebook_cb_t cb = cbd->cb;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);
	int records = 0;

	if (!cbd)
		goto error;

	if (!pbd)
		goto error;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		goto error;

	if (structure != OFONO_SIM_FILE_STRUCTURE_FIXED)
		goto error;

	if (!pbd->sim_driver->read_file_linear)
		goto error;

	records = filelength / recordlength;

	if (!records)
		goto error;

	DBG("EFpbr size %d, record length %d, records %d",
		filelength, recordlength, records);
	pbd->pb_reference_file_info.file_id = SIM_EFPBR_FILEID;
	pbd->pb_reference_file_info.file_length = filelength;
	pbd->pb_reference_file_info.record_length = recordlength;
	pbd->pb_reference_file_info.record = 1;	/* Current record, not amount */
	pbd->pb_reference_file_info.structure = OFONO_SIM_FILE_STRUCTURE_FIXED;
	pbd->sim_driver->read_file_linear(get_sim(), SIM_EFPBR_FILEID,
					1, recordlength,
					usim_path, sizeof(usim_path),
					pb_reference_data_cb, cbd);
	return;
error:
	if (cbd){
		void *pb = cbd->data;
		g_free(cbd);
		if(cb)
			CALLBACK_WITH_FAILURE(cb, pb);
	}
}

static void ril_export_entries(struct ofono_phonebook *pb,
				const char *storage,
				ofono_phonebook_cb_t cb, void *data)
{
	struct pb_data *pd = ofono_phonebook_get_data(pb);
	struct cb_data *cbd = cb_data_new2(pb, cb, data);
	int fileid;

	DBG("Storage %s", storage);
	if (strcmp(storage, "SM"))	/* Only for SIM memory */
		goto error;

	switch (ril_get_app_type()) {
	case RIL_APPTYPE_SIM:
		DBG("SIM application");
		fileid = SIM_EFADN_FILEID;
		pd->sim_driver->read_file_info(get_sim(), fileid,
			sim_path, sizeof(sim_path), pb_adn_sim_info_cb, cbd);
		break;
	case RIL_APPTYPE_USIM:
		DBG("USIM application");
		fileid = SIM_EFPBR_FILEID;
		pd->sim_driver->read_file_info(get_sim(), fileid,
			usim_path, sizeof(usim_path),
			pb_reference_info_cb, cbd);
		break;
	default:
		DBG("UICC application type not unknown or supported");
		goto error;
		break;
	}

	return;

error:

	if (cbd){
		void *pb = cbd->data;
		g_free(cbd);
		if(cb)
			CALLBACK_WITH_FAILURE(cb, pb);
	}
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_phonebook *pb = user_data;
	struct pb_data *pbd = ofono_phonebook_get_data(pb);

	pbd->timer_id = 0;

	ofono_phonebook_register(pb);
	return FALSE;
}

static int ril_phonebook_probe(struct ofono_phonebook *pb,
			unsigned int vendor, void *user)
{
	GRil *ril = user;
	struct pb_data *pd = g_try_new0(struct pb_data, 1);
	pd->ril = g_ril_clone(ril);
	pd->sim_driver = get_sim_driver();
	ofono_phonebook_set_data(pb, pd);
	pd->timer_id = g_timeout_add_seconds(2, ril_delayed_register, pb);

	return 0;
}

static void ril_phonebook_remove(struct ofono_phonebook *pb)
{
	struct pb_data *pd = ofono_phonebook_get_data(pb);
	ofono_phonebook_set_data(pb, NULL);
	g_ril_unref(pd->ril);

	pb_files = NULL;
	pb_next = NULL;
	phonebook_entry_start = NULL;
	phonebook_entry_current = NULL;

	if (pd->timer_id > 0)
		g_source_remove(pd->timer_id);

	g_free(pd);
}

static struct ofono_phonebook_driver driver = {
	.name				= "rilmodem",
	.probe				= ril_phonebook_probe,
	.remove				= ril_phonebook_remove,
	.export_entries			= ril_export_entries
};

void ril_phonebook_init(void)
{
	ofono_phonebook_driver_register(&driver);
}

void ril_phonebook_exit(void)
{
	ofono_phonebook_driver_unregister(&driver);
}
