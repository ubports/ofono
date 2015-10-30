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

#include "ril_plugin.h"
#include "ril_util.h"
#include "ril_log.h"
#include "ril_constants.h"

#include "util.h"
#include "simutil.h"

/* File info parameters */
#define FCP_TEMPLATE                    0x62
#define FCP_FILE_SIZE                   0x80
#define FCP_FILE_DESC                   0x82
#define FCP_FILE_ID                     0x83
#define FCP_FILE_LIFECYCLE              0x8A
#define FCP_FILE_SECURITY_ARR           0x8B
#define FCP_FILE_SECURITY_COMPACT       0x8C
#define FCP_FILE_SECURITY_EXPANDED      0xAB

#define SIM_EFPBR_FILEID                0x4F30

#define UNUSED                          0xff

#define EXT1_CP_SUBADDRESS              1
#define EXT1_ADDITIONAL_DATA            2

#define NAME_SIZE                       64
#define NUMBER_SIZE                     256
#define EMAIL_SIZE                      128
#define EXT_NUMBER_SIZE                 24
#define SNE_SIZE                        64

/* TON (Type Of Number) See TS 24.008 */
#define TON_MASK                        0x70
#define TON_INTERNATIONAL               0x10

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

struct ril_phonebook_file {
	int file_id;
	guchar file_type;
	guchar structure;
	int file_length;
	int record_length;
	int record;
	gboolean handled;
};

struct ril_phonebook_entry {
	int entry;
	char *name;
	char *number;
	char *email;
	char *anr;
	char *sne;
};

struct ril_phonebook {
	int refcount;
	GRilIoQueue *q;
	struct ril_modem *modem;
	struct ofono_phonebook *pb;
	guint timer_id;
};

struct ril_phonebook_export {
	struct ril_phonebook *pbd;
	int app_type;
	ofono_phonebook_cb_t cb;
	gpointer data;
	gint pb_entry;
	struct ril_phonebook_file ref_file_info;
	struct ril_phonebook_file *ext_file_info;
	struct ril_phonebook_file *extension_file_info; /* NEEDED? */
	struct ril_phonebook_file *current_file_info;
	GSList *pb_files;
	GSList *pb_next;
	GSList *pb_entries;
	guchar ext1_to_type;
	guchar ext1_to_entry;
};

static const guchar sim_path[4] = {0x3F, 0x00, 0x7F, 0x10};
static const guchar usim_path[6] = {0x3F, 0x00, 0x7F, 0x10, 0x5F, 0x3A};

static void ril_phonebook_content_data_read(struct ril_phonebook_export *exp,
	struct ril_phonebook_file *file_info);

static inline struct ril_phonebook *ril_phonebook_get_data(
						struct ofono_phonebook *pb)
{
	return ofono_phonebook_get_data(pb);
}

static void ril_phonebook_cancel_io(struct ril_phonebook *pbd)
{
	if (pbd->timer_id) {
		g_source_remove(pbd->timer_id);
		pbd->timer_id = 0;
	}
	grilio_queue_cancel_all(pbd->q, FALSE);
}

static void ril_phonebook_free(struct ril_phonebook *pbd)
{
	ril_phonebook_cancel_io(pbd);
	grilio_queue_unref(pbd->q);
	g_free(pbd);
}

static inline struct ril_phonebook *ril_phonebook_ref(struct ril_phonebook *pbd)
{
	GASSERT(pbd->refcount > 0);
	pbd->refcount++;
	return pbd;
}

static inline void ril_phonebook_unref(struct ril_phonebook *pbd)
{
	GASSERT(pbd);
	GASSERT(pbd->refcount > 0);
	if (!(--pbd->refcount)) {
		ril_phonebook_free(pbd);
	}
}

static struct ril_phonebook_export *ril_phonebook_export_new(
				struct ril_phonebook *pbd, int app_type,
				ofono_phonebook_cb_t cb, void *data)
{
	struct ril_phonebook_export *exp =
		g_new0(struct ril_phonebook_export, 1);

	exp->pbd = ril_phonebook_ref(pbd);
	exp->app_type = app_type;
	exp->cb = cb;
	exp->data = data;
	return exp;
}

static void ril_phonebook_entry_free(gpointer data)
{
	struct ril_phonebook_entry *entry = data;

	g_free(entry->number);
	g_free(entry->name);
	g_free(entry->anr);
	g_free(entry->sne);
	g_free(entry->email);
	g_free(entry);
}

static void ril_phonebook_export_done(struct ril_phonebook_export *exp,
								int type)
{
	/* Don't invoke completion callback if phonebook is already gone */
	if (exp->cb && exp->pbd->pb) {
		struct ofono_error error;
		error.error = 0;
		error.type = type;
		exp->cb(&error, exp->data);
	}

	g_free(exp->extension_file_info);
	g_free(exp->current_file_info);
	g_slist_free_full(exp->pb_files, g_free);
	g_slist_free_full(exp->pb_entries, ril_phonebook_entry_free);
	ril_phonebook_unref(exp->pbd);
	g_free(exp);
}

static inline void ril_phonebook_export_ok(struct ril_phonebook_export *exp)
{
	DBG("");
	ril_phonebook_export_done(exp, OFONO_ERROR_TYPE_NO_ERROR);
}

static inline void ril_phonebook_export_error(struct ril_phonebook_export *exp)
{
	DBG("");
	ril_phonebook_export_done(exp, OFONO_ERROR_TYPE_FAILURE);
}

/*
 * BCD to utf8 conversion. See table 4.4 in TS 31.102.
 * BCD 0x0C indicates pause before sending following digits as DTMF tones.
 * BCD 0x0D is a wildcard that means "any digit"
 * BCD 0x0E is reserved, we convert it to 'e' (why not?).
 */
static void ril_phonebook_bcd_to_utf8(char *utf8, const guchar *bcd, guint len)
{
	static const char digit_to_utf8[] = "0123456789*#pwe\0";
	guint i;

	for (i = 0; i < len; i++) {
		utf8[2*i] = digit_to_utf8[bcd[i] & 0x0f];
		utf8[2*i + 1] = digit_to_utf8[(bcd[i] >> 4) & 0x0f];
	}

	utf8[2*i] = 0;
}

static void ril_phonebook_create_entry(gpointer data, gpointer user_data)
{
	struct ril_phonebook_entry *pbe = data;
	struct ril_phonebook *pbd = user_data;

	if (pbd->pb) {
		if ((pbe->name && pbe->name[0]) ||
					(pbe->number && pbe->number[0]) ||
					(pbe->email && pbe->email[0]) ||
					(pbe->anr && pbe->anr[0]) ||
					(pbe->sne && pbe->sne[0])) {
			DBG("vCard: name=%s number=%s email=%s anr=%s sne=%s",
					pbe->name, pbe->number, pbe->email,
					pbe->anr, pbe->sne);
			ofono_phonebook_entry(pbd->pb, -1, pbe->number, -1,
					pbe->name, -1, NULL, pbe->anr, -1,
					pbe->sne, pbe->email, NULL, NULL);
		}
	}
}

static void ril_phonebook_create_entries(struct ril_phonebook_export *exp)
{
	DBG("All data requested, start vCard creation");
	g_slist_foreach(exp->pb_entries, ril_phonebook_create_entry, exp->pbd);
	DBG("Finally all PB data read");
}

static void ril_phonebook_handle_adn(struct ril_phonebook_export *exp,
						const guchar *msg, size_t len)
{
	guchar name_length;
	guchar number_start;
	guchar number_length = 0;
	guchar extension_record = UNUSED;
	guchar prefix;
	char *number = NULL;
	char *name;

	if (len < 14) {
		return;
	}

	name_length = len - 14;
	number_start = name_length;

	name = sim_string_to_utf8(msg, name_length);
	/* Length contains also TON&NPI */
	number_length = msg[number_start];

	if (number_length != UNUSED && number_length != 0) {
		number = g_malloc(NUMBER_SIZE);
		number_length--;
		prefix = 0;

		if ((msg[number_start + 1] & TON_MASK) == TON_INTERNATIONAL) {
			number[0] = '+';
			prefix = 1;
		}

		ril_phonebook_bcd_to_utf8(number + prefix,
						msg + number_start + 2,
							number_length);
		extension_record = msg[len - 1];
	}

	DBG("ADN name %s, number %s ", name, number);
	DBG("length %d extension_record %d", number_length, extension_record);

	/* THE PURPOSE OF THIS CODE WAS UNCLEAR
	if (extension_record != UNUSED) {
		next_file = g_try_new0(struct ril_phonebook_file, 1);
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
	*/

	if (name || number) {
		struct ril_phonebook_entry *new_entry =
			g_new0(struct ril_phonebook_entry, 1);

		new_entry->name = name;
		new_entry->number = number;

		DBG("Creating PB entry %d with name %s number %s",
			exp->pb_entry, new_entry->name, new_entry->number);

		exp->pb_entries = g_slist_append(exp->pb_entries, new_entry);
		exp->pb_entry++;
	}
}

static void ril_phonebook_handle_sne(struct ril_phonebook_export *exp,
						const guchar *msg, size_t len)
{
	guchar sne_length;
	guchar entry_nbr;
	char *sne;

	DBG("SNE");

	if (len < 2) {
		return;
	}

	sne_length = len - 2;
	entry_nbr = msg[len - 1];

	sne = sim_string_to_utf8(msg, sne_length);

	if (sne) {
		/* GSlist nth counts from 0, PB entries from 1 */
		GSList *list_entry = g_slist_nth(exp->pb_entries, entry_nbr-1);
		DBG("SNE \'%s\' to PB entry %d", sne, entry_nbr);

		if (list_entry) {
			struct ril_phonebook_entry *entry =
				list_entry->data;

			DBG("Adding SNE to entry %d", entry_nbr);
			DBG("name %s", entry->name);

			g_free(entry->sne);
			entry->sne = sne;
		} else {
			g_free(sne);
		}
	}
}

static void ril_phonebook_handle_anr(struct ril_phonebook_export *exp,
						const guchar *msg, size_t len)
{
	guchar number_length = 0;
	guchar extension_record = UNUSED;
	guchar aas_record = UNUSED;
	guchar prefix;
	guchar entry_nbr;
	char* anr = NULL;

	DBG("ANR");

	if (len < 1 || msg[0] == UNUSED) {
		return;
	}

	entry_nbr = msg[len - 1];
	aas_record = msg[0];
	/* Length contains also TON&NPI */
	number_length = msg[1];

	if (number_length) {
		number_length--;
		anr = g_malloc0(NUMBER_SIZE);
		prefix = 0;

		if ((msg[2] & TON_MASK) == TON_INTERNATIONAL) {
			anr[0] = '+';
			prefix = 1;
		}

		ril_phonebook_bcd_to_utf8(anr + prefix, msg + 3, number_length);
		extension_record = msg[len - 3];
	}

	DBG("ANR to entry %d number %s number length %d", entry_nbr, anr,
							number_length);
	DBG("extension_record %d aas %d", extension_record, aas_record);

	/* THE PURPOSE OF THIS CODE WAS UNCLEAR
	if (extension_record != UNUSED) {
		next_file = g_new0(struct ril_phonebook_file, 1);

		if (pbd->extension_file_info) {
			memmove(next_file, pbd-> extension_file_info,
					sizeof(struct ril_phonebook_file));
		} else {
			next_file->file_type = TYPE_EXT1;
			next_file->file_id = SIM_EFEXT1_FILEID;
		}

		next_file->record = extension_record;
		pbd->ext1_to_type = TYPE_ANR;
		pbd->ext1_to_entry = phonebook_entry_nbr;
	}
	*/

	if (anr) {
		/* GSlist nth counts from 0, PB entries from 1 */
		GSList *list_entry = g_slist_nth(exp->pb_entries, entry_nbr-1);

		if (list_entry) {
			struct ril_phonebook_entry *entry = list_entry->data;
			if (entry) {
				/* if one already exists, delete it */
				g_free(entry->anr);
				DBG("Adding ANR to entry %d, name %s",
						entry_nbr, entry->name);
				entry->anr = anr;
			}
		} else {
			g_free(anr);
		}
	}
}

static void ril_phonebook_handle_email(struct ril_phonebook_export *exp,
						const guchar *msg, size_t len)
{
	char *email;
	guchar entry_nbr;

	if (len < 1)
		return;

	entry_nbr = msg[len - 1];
	email = sim_string_to_utf8(msg, len - 2);

	if (email) {
		/* GSlist nth counts from 0, PB entries from 1 */
		GSList *list_entry = g_slist_nth(exp->pb_entries, entry_nbr-1);

		DBG("Email \'%s\' to PB entry %d", email, entry_nbr);
		if (list_entry) {
			struct ril_phonebook_entry *entry = list_entry->data;

			/* if one already exists, delete it */
			if (entry) {
				g_free(entry->email);
				DBG("Adding email to entry %d", entry_nbr);
				DBG("name %s", entry->name);
				entry->email = email;
			}
		} else {
			g_free(email);
		}
	}
}

static void ril_phonebook_handle_ext1(struct ril_phonebook_export *exp,
		const unsigned char *msg)
{
	char *ext_number = g_malloc0(EXT_NUMBER_SIZE);
	guchar next_extension_record, number_length = msg[1];

	ril_phonebook_bcd_to_utf8(ext_number, msg, number_length);
	next_extension_record = msg[number_length + 2];

	DBG("Number extension %s", ext_number);
	DBG("number length %d", number_length);
	DBG("extension_record %d", next_extension_record);

	/* pb_entry is already incremented & g_slist_nth counts from 0 */
	if (exp->ext1_to_type == TYPE_ADN) {
		GSList *entry = g_slist_nth(exp->pb_entries,
							exp->ext1_to_entry-1);
		DBG("Looking for ADN entry %d", exp->ext1_to_entry);
		if (entry) {
			struct ril_phonebook_entry *pb_entry = entry->data;
			if (pb_entry) {
				strcat(pb_entry->number, ext_number);
			}
		}
	} else if (exp->ext1_to_type == TYPE_ANR) {
		GSList *entry = g_slist_nth(exp->pb_entries,
						exp->ext1_to_entry-1);
		DBG("Looking for ANR entry %d", exp->ext1_to_entry);
		if (entry) {
			struct ril_phonebook_entry *pb_entry = entry->data;
			if (pb_entry) {
				strcat(pb_entry->anr, ext_number);
			}
		}
	}

	g_free(ext_number);

	/* THE PURPOSE OF THIS CODE WAS UNCLEAR
	if (next_extension_record != UNUSED) {
		next_file = g_new0(struct ril_phonebook_file, 1);
		if (exp->ext_file_info) {
			*next_file = *exp->ext_file_info;
		} else {
			next_file->file_type = TYPE_EXT1;
			next_file->file_id = SIM_EFEXT1_FILEID;
		}
		next_file->record = next_extension_record;
	}
	*/
}

static void ril_phonebook_decode_response(struct ril_phonebook_export *exp,
			guchar file_type, const guchar *msg, size_t len)
{
	switch (file_type) {
	case TYPE_ADN:
		ril_phonebook_handle_adn(exp, msg, len);
		break;
	case TYPE_SNE:
		ril_phonebook_handle_sne(exp, msg, len);
		break;
	case TYPE_ANR:
		ril_phonebook_handle_anr(exp, msg, len);
		break;
	case TYPE_AAS:
		DBG("AAS");
		break;
	case TYPE_EMAIL:
		ril_phonebook_handle_email(exp, msg, len);
		break;
	case TYPE_EXT1:
		DBG("EXT1 to type=%02X, entry=%d", exp->ext1_to_type,
							exp->ext1_to_entry);
		if (msg[0] == EXT1_ADDITIONAL_DATA) {
			ril_phonebook_handle_ext1(exp, msg);
		}
		break;
	default:
		DBG("Skipping type %02X", file_type);
		break;
	}
}

static void pb_adn_sim_data_cb(const struct ofono_error *error,
		const unsigned char *sdata, int length, void *data)
{
	struct ril_phonebook_export *exp = data;
	struct ofono_sim *sim = ril_modem_ofono_sim(exp->pbd->modem);
	struct ril_phonebook_file *file_info = exp->current_file_info;

	DBG("");
	GASSERT(file_info);
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || !exp->pbd->pb ||
							!sim || !file_info) {
		ril_phonebook_export_error(exp);
		return;
	}

	ril_phonebook_decode_response(exp, exp->extension_file_info ?
		exp->extension_file_info->file_type : file_info->file_type,
							sdata, length);

	/* APPARENTLY THIS CODE NEVER WORKED
	if (file_info) {
		DBG("Reading extension file %04X, record %d",
			file_info->file_id, file_info->record);
		ril_sim_read_file_linear(sim, file_info->file_id,
						file_info->record,
						file_info->record_length,
						sim_path, sizeof(sim_path),
						pb_adn_sim_data_cb, cbd_outer);

		g_free(extension_file_info);
		extension_file_info = file_info;
		return;
	}
	*/

	g_free(exp->extension_file_info);
	exp->extension_file_info = NULL;

	if (file_info->record <
			(file_info->file_length / file_info->record_length)) {

		file_info->record++;
		DBG("Same file, next record %d", file_info->record);
		ril_sim_read_file_linear(sim, file_info->file_id,
				file_info->record, file_info->record_length,
				sim_path, sizeof(sim_path),
				pb_adn_sim_data_cb, exp);
	} else {
		ril_phonebook_create_entries(exp);
		ril_phonebook_export_ok(exp);
	}
}

static void ril_phonebook_adn_sim_info_cb(const struct ofono_error *error,
		int filelength, enum ofono_sim_file_structure structure,
		int recordlength, const unsigned char access[3],
		unsigned char file_status, void *data)
{
	struct ril_phonebook_export *exp = data;
	struct ofono_sim *sim = ril_modem_ofono_sim(exp->pbd->modem);
	int records;

	DBG("");
	if (error->type == OFONO_ERROR_TYPE_NO_ERROR &&
				structure == OFONO_SIM_FILE_STRUCTURE_FIXED &&
				exp->pbd->pb && sim && recordlength &&
				(records = filelength / recordlength) > 0) {
		struct ril_phonebook_file *info;

		if (!exp->current_file_info) {
			exp->current_file_info =
				g_new0(struct ril_phonebook_file, 1);
		}

		info = exp->current_file_info;
		info->file_id = SIM_EFADN_FILEID;
		info->file_type = TYPE_ADN;
		info->structure = structure;
		info->file_length = filelength;
		info->record_length = recordlength;
		info->record = 1;

		ril_sim_read_file_linear(sim, info->file_id,
					info->record, info->record_length,
					sim_path, sizeof(sim_path),
					pb_adn_sim_data_cb, exp);
	} else {
		ril_phonebook_export_error(exp);
	}
}

static gboolean ril_phonebook_file_supported(
				const struct ril_phonebook_file *file)
{
	if (file) {
		switch (file->file_type) {
		case TYPE_ADN:
		case TYPE_EMAIL:
		case TYPE_SNE:
		case TYPE_ANR:
			return TRUE;
		default:
			return FALSE;
		}
	}
	return FALSE;
}

static void ril_phonebook_content_data_cb(const struct ofono_error *error,
				const unsigned char *sdata,
				int length, void *data)
{
	struct ril_phonebook_export *exp = data;
	struct ril_phonebook_file *file_info = exp->pb_next->data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || !exp->pbd->pb) {
		ril_phonebook_export_error(exp);
		return;
	}

	ril_phonebook_decode_response(exp, exp->extension_file_info ?
		exp->extension_file_info->file_type : file_info->file_type,
							sdata, length);

	/* APPARENTLY THIS CODE NEVER WORKED
	if (file_info) {
		DBG("Reading extension file %04X, record %d, structure %d",
			file_info->file_id, file_info->record,
			file_info->structure);
		ril_phonebook_content_data_read(exp, file_info);
		g_free(extension_file_info);
		extension_file_info = file_info;
		return;
	}
	*/

	g_free(exp->extension_file_info);
	exp->extension_file_info = NULL;

	if (((file_info->structure == OFONO_SIM_FILE_STRUCTURE_FIXED) ||
		(file_info->structure == OFONO_SIM_FILE_STRUCTURE_CYCLIC)) &&
		(file_info->record <
			(file_info->file_length / file_info->record_length))) {
		file_info->record++;
		DBG("Same file, next record %d", file_info->record);
	} else {
		DBG("Next file in list");
		if ((exp->pb_next = g_slist_next(exp->pb_next)) != NULL &&
	 	 	 !ril_phonebook_file_supported(exp->pb_next->data)) {
			file_info = exp->pb_next->data;
			DBG("Skipping file type %02X", file_info->file_type);
			exp->pb_next = g_slist_next(exp->pb_next);
		}

		if (!exp->pb_next) {
			ril_phonebook_create_entries(exp);
			ril_phonebook_export_ok(exp);
			return;
		}

		file_info = exp->pb_next->data;
	}

	ril_phonebook_content_data_read(exp, file_info);
}

static void ril_phonebook_content_data_read(struct ril_phonebook_export *exp,
	struct ril_phonebook_file *file)
{
	struct ofono_sim* sim = ril_modem_ofono_sim(exp->pbd->modem);

	if (exp->pbd->pb && sim) {
		DBG("Reading content type=%02X, file ID=%04X, structure=%d",
				file->file_type, file->file_id,
				file->structure);

		switch (file->structure) {
		case OFONO_SIM_FILE_STRUCTURE_FIXED:
			ril_sim_read_file_linear(sim, file->file_id,
				file->record, file->record_length,
				usim_path, sizeof(usim_path),
				ril_phonebook_content_data_cb, exp);
			return;
		case OFONO_SIM_FILE_STRUCTURE_CYCLIC:
			ril_sim_read_file_cyclic(sim, file->file_id,
				file->record, file->record_length, NULL, 0,
				ril_phonebook_content_data_cb, exp);
			return;
		case OFONO_SIM_FILE_STRUCTURE_TRANSPARENT:
			ril_sim_read_file_transparent(sim, file->file_id, 0,
				file->file_length, usim_path, sizeof(usim_path),
				ril_phonebook_content_data_cb, exp);
			return;
		}
	}

	ril_phonebook_export_error(exp);
}

static void ril_phonebook_content_info_cb(const struct ofono_error *error,
		int filelength, enum ofono_sim_file_structure structure,
		int recordlength, const unsigned char access[3],
		unsigned char file_status, void *data)
{
	struct ril_phonebook_export *exp = data;
	struct ofono_sim* sim = ril_modem_ofono_sim(exp->pbd->modem);
	struct ril_phonebook_file *file;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || !exp->pbd->pb || !sim) {
		ril_phonebook_export_error(exp);
		return;
	}

	file = exp->pb_next->data;
	file->structure = structure;
	file->file_length = filelength;
	file->record_length = recordlength;
	file->record = 1;

	DBG("File type=%02X, File ID=%04X, Struct=%d, File len=%d, Rec len=%d",
		file->file_type, file->file_id, file->structure,
		file->file_length, file->record_length);

	if (file->file_type == TYPE_EXT1) {
		exp->ext_file_info = file;
	}

	exp->pb_next = g_slist_next(exp->pb_next);
	if (exp->pb_next) {
		file = exp->pb_next->data;
		DBG("Reading next content info %04X", file->file_id);
		ril_sim_read_file_info(sim, file->file_id,
					usim_path, sizeof(usim_path),
					ril_phonebook_content_info_cb, exp);
	} else {
		DBG("All info requested, start content reading");

		/* Re-start from beginning */
		exp->pb_next = exp->pb_files;
		file = exp->pb_next->data;

		DBG("content_data_read type=%02X", file->file_type);
		ril_phonebook_content_data_read(exp, file);
	}
}

static void ril_phonebook_reference_data_cb(const struct ofono_error *error,
		const unsigned char *sdata, int length, void *data)
{
	struct ril_phonebook_export *exp = data;
	struct ril_phonebook_file* ref = &exp->ref_file_info;
	struct ofono_sim* sim = ril_modem_ofono_sim(exp->pbd->modem);
	const guchar *ptr = sdata;
	gboolean finished = FALSE;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR || !exp->pbd->pb || !sim) {
		ril_phonebook_export_error(exp);
		return;
	}

	while ((ptr < sdata + length) && !finished) {
		int i, typelen;
		switch (ptr[0]) {
		case TYPE_1_TAG:
		case TYPE_2_TAG:
		case TYPE_3_TAG:
			typelen = ptr[1];
			DBG("File type=%02X, len=%d", ptr[0], typelen);
			ptr += 2;
			for (i = 0; i < typelen; i += ptr[i+1] + 2) {
				struct ril_phonebook_file *file =
					g_new0(struct ril_phonebook_file, 1);

				file->file_type = ptr[i];
				file->file_id = (ptr[i+2] << 8) + ptr[i+3];
				DBG("Creating file info type=%02X id=%04X",
					file->file_type, file->file_id);
				exp->pb_files = g_slist_append(exp->pb_files,
									file);
			}
			ptr += typelen;
			break;

		default:
			DBG("All handled %02x", *ptr);
			finished = TRUE;
			break;
		}
	}

	if (ref->record < (ref->file_length/ref->record_length)) {
		ref->record++;
		DBG("Next EFpbr record %d", ref->record);
		switch (exp->app_type) {
		case RIL_APPTYPE_SIM:
			ril_sim_read_file_linear(sim, ref->file_id,
				ref->record, ref->record_length,
				sim_path, sizeof(sim_path),
				ril_phonebook_reference_data_cb, exp);
			return;
		case RIL_APPTYPE_USIM:
			ril_sim_read_file_linear(sim, ref->file_id,
				ref->record, ref->record_length,
				usim_path, sizeof(usim_path),
				ril_phonebook_reference_data_cb, exp);
			return;
		default:
			break;
		}
	} else {
		DBG("All EFpbr records read");
		exp->pb_next = exp->pb_files;
		if (exp->pb_next) {
			struct ril_phonebook_file *file = exp->pb_next->data;
			ril_sim_read_file_info(sim, file->file_id,
					usim_path, sizeof(usim_path),
					ril_phonebook_content_info_cb, exp);
			return;
		} else {
			ril_phonebook_export_ok(exp);
		}
	}

	ril_phonebook_export_error(exp);
}

static void ril_phonebook_reference_info_cb(const struct ofono_error *error,
		int filelength, enum ofono_sim_file_structure structure,
		int recordlength, const unsigned char access[3],
		unsigned char file_status, void *data)
{

	struct ril_phonebook_export *exp = data;
	struct ofono_sim* sim = ril_modem_ofono_sim(exp->pbd->modem);
	int records;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR ||
				structure != OFONO_SIM_FILE_STRUCTURE_FIXED ||
				!exp->pbd->pb || !sim || !recordlength) {
		ril_phonebook_export_error(exp);
		return;
	}

	records = filelength / recordlength;
	if (records) {
		struct ril_phonebook_file* ref = &exp->ref_file_info;

		DBG("EFpbr size %d, record length %d, records %d",
					filelength, recordlength, records);
		ref->file_id = SIM_EFPBR_FILEID;
		ref->file_length = filelength;
		ref->record_length = recordlength;
		ref->record = 1;	/* Current record, not amount */
		ref->structure = OFONO_SIM_FILE_STRUCTURE_FIXED;
		ril_sim_read_file_linear(sim, SIM_EFPBR_FILEID,
				1, recordlength, usim_path, sizeof(usim_path),
				ril_phonebook_reference_data_cb, exp);
	} else {
		ril_phonebook_export_error(exp);
	}

}

static void ril_phonebook_export_entries(struct ofono_phonebook *pb,
		const char *storage, ofono_phonebook_cb_t cb, void *data)
{
	struct ril_phonebook *pbd = ril_phonebook_get_data(pb);
	struct ofono_sim *sim = ril_modem_ofono_sim(pbd->modem);
	struct ofono_error error;

	DBG("Storage %s", storage);

	/* Only for SIM memory */
	if (!strcmp(storage, "SM")) {
		const int type = ril_sim_app_type(sim);
		switch (type) {
		case RIL_APPTYPE_SIM:
			DBG("SIM application");
			ril_sim_read_file_info(sim, SIM_EFADN_FILEID,
				sim_path, sizeof(sim_path),
				ril_phonebook_adn_sim_info_cb,
				ril_phonebook_export_new(pbd, type, cb, data));
			return;
		case RIL_APPTYPE_USIM:
			DBG("USIM application");
			ril_sim_read_file_info(sim, SIM_EFPBR_FILEID,
				usim_path, sizeof(usim_path),
				ril_phonebook_reference_info_cb,
				ril_phonebook_export_new(pbd, type, cb, data));
			return;
		default:
			DBG("Unsupported UICC application type %d", type);
			break;
		}
	}

	cb(ril_error_failure(&error), data);
}

static gboolean ril_phonebook_register(gpointer user_data)
{
	struct ril_phonebook *pbd = user_data;

	pbd->timer_id = 0;
	ofono_phonebook_register(pbd->pb);

	/* Single shot */
	return FALSE;
}

static int ril_phonebook_probe(struct ofono_phonebook *pb,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_phonebook *pbd = g_new0(struct ril_phonebook, 1);

	DBG("");
	pbd->refcount = 1;
	pbd->modem = modem;
	pbd->pb = pb;
	pbd->q = grilio_queue_new(ril_modem_io(modem));

	pbd->timer_id = g_idle_add(ril_phonebook_register, pbd);
	ofono_phonebook_set_data(pb, pbd);
	return 0;
}

static void ril_phonebook_remove(struct ofono_phonebook *pb)
{
	struct ril_phonebook *pbd = ril_phonebook_get_data(pb);
	DBG("");
	ril_phonebook_cancel_io(pbd);
	pbd->modem = NULL;
	pbd->pb = NULL;
	ofono_phonebook_set_data(pb, NULL);
	ril_phonebook_unref(pbd);
}

const struct ofono_phonebook_driver ril_phonebook_driver = {
	.name           = RILMODEM_DRIVER,
	.probe          = ril_phonebook_probe,
	.remove         = ril_phonebook_remove,
	.export_entries = ril_phonebook_export_entries
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
