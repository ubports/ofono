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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>

#include <glib.h>

#include "ofono.h"
#include "common.h"
#include "hfp.h"
#include "gatserver.h"
#include "gatppp.h"
#include "handsfree-audio.h"
#include "system-settings.h"

#define RING_TIMEOUT 3

#define CVSD_OFFSET 0
#define MSBC_OFFSET 1
#define CODECS_COUNT (MSBC_OFFSET + 1)

struct hfp_codec_info {
	unsigned char type;
	ofono_bool_t supported;
};

struct atom_callback {
	struct ofono_atom *atom;
	ofono_emulator_request_cb_t cb;
	void *data;
	ofono_destroy_func destroy;
};

struct ofono_emulator {
	GSList *atoms;
	struct ofono_atom *active_atom;
	enum ofono_emulator_type type;
	GAtServer *server;
	GAtPPP *ppp;
	int l_features;
	int r_features;
	GSList *indicators;
	guint callsetup_source;
	int pns_id;
	struct ofono_handsfree_card *card;
	struct hfp_codec_info r_codecs[CODECS_COUNT];
	unsigned char selected_codec;
	unsigned char negotiated_codec;
	unsigned char proposed_codec;
	ofono_emulator_codec_negotiation_cb codec_negotiation_cb;
	void *codec_negotiation_data;
	ofono_bool_t bac_received;
	/* Table of atom_callback, using prefixes as key */
	GHashTable *prefixes;
	bool slc : 1;
	unsigned int events_mode : 2;
	bool events_ind : 1;
	unsigned int cmee_mode : 2;
	bool clip : 1;
	bool ccwa : 1;
	bool ddr_active : 1;
};

struct indicator {
	char *name;
	int value;
	int min;
	int max;
	gboolean deferred;
	gboolean active;
	gboolean mandatory;
};

static void emulator_debug(const char *str, void *data)
{
	ofono_info("%s: %s\n", (char *)data, str);
}

static void emulator_disconnect(gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	DBG("%p", em);

	ofono_emulator_remove(em);
}

static void ppp_connect(const char *iface, const char *local,
			const char *remote,
			const char *dns1, const char *dns2,
			gpointer user_data)
{
	DBG("Network Device: %s\n", iface);
	DBG("IP Address: %s\n", local);
	DBG("Remote IP Address: %s\n", remote);
	DBG("Primary DNS Server: %s\n", dns1);
	DBG("Secondary DNS Server: %s\n", dns2);
}

static void cleanup_ppp(struct ofono_emulator *em)
{
	DBG("");

	g_at_ppp_unref(em->ppp);
	em->ppp = NULL;

	__ofono_private_network_release(em->pns_id);
	em->pns_id = 0;

	g_at_server_resume(em->server);
	g_at_server_send_final(em->server, G_AT_SERVER_RESULT_NO_CARRIER);
}

static void ppp_disconnect(GAtPPPDisconnectReason reason, gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	cleanup_ppp(em);
}

static void ppp_suspend(gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	DBG("");

	g_at_server_resume(em->server);
}

static void suspend_server(gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtIO *io = g_at_server_get_io(em->server);

	g_at_server_suspend(em->server);

	if (g_at_ppp_listen(em->ppp, io) == FALSE)
		cleanup_ppp(em);
}

static void request_private_network_cb(
			const struct ofono_private_network_settings *pns,
			void *data)
{
	struct ofono_emulator *em = data;
	GAtIO *io = g_at_server_get_io(em->server);

	if (pns == NULL)
		goto error;

	em->ppp = g_at_ppp_server_new_full(pns->server_ip, pns->fd);
	if (em->ppp == NULL) {
		close(pns->fd);
		goto badalloc;
	}

	g_at_ppp_set_server_info(em->ppp, pns->peer_ip,
					pns->primary_dns, pns->secondary_dns);

	g_at_ppp_set_acfc_enabled(em->ppp, TRUE);
	g_at_ppp_set_pfc_enabled(em->ppp, TRUE);

	g_at_ppp_set_credentials(em->ppp, "", "");
	g_at_ppp_set_debug(em->ppp, emulator_debug, "PPP");

	g_at_ppp_set_connect_function(em->ppp, ppp_connect, em);
	g_at_ppp_set_disconnect_function(em->ppp, ppp_disconnect, em);
	g_at_ppp_set_suspend_function(em->ppp, ppp_suspend, em);

	g_at_server_send_intermediate(em->server, "CONNECT");
	g_at_io_set_write_done(io, suspend_server, em);

	return;

badalloc:
	__ofono_private_network_release(em->pns_id);

error:
	em->pns_id = 0;
	g_at_server_send_final(em->server, G_AT_SERVER_RESULT_ERROR);
}

static gboolean dial_call(struct ofono_emulator *em, const char *dial_str)
{
	char c = *dial_str;

	DBG("dial call %s", dial_str);

	if (c == '*' || c == '#' || c == 'T' || c == 't') {
		if (__ofono_private_network_request(request_private_network_cb,
						&em->pns_id, em) == FALSE)
			return FALSE;
	}

	return TRUE;
}

static void dial_cb(GAtServer *server, GAtServerRequestType type,
				GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	const char *dial_str;

	DBG("");

	if (type != G_AT_SERVER_REQUEST_TYPE_SET)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, ""))
		goto error;

	dial_str = g_at_result_iter_raw_line(&iter);
	if (!dial_str)
		goto error;

	if (em->ppp)
		goto error;

	if (!dial_call(em, dial_str))
		goto error;

	return;

error:
	g_at_server_send_final(em->server, G_AT_SERVER_RESULT_ERROR);
}

static void dun_ath_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;

	DBG("");

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &val) == FALSE)
			goto error;

		if (val != 0)
			goto error;

		/* Fall through */

	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		if (em->ppp == NULL)
			goto error;

		g_at_ppp_unref(em->ppp);
		em->ppp = NULL;

		__ofono_private_network_release(em->pns_id);
		em->pns_id = 0;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
error:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void resume_ppp(gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	g_at_server_suspend(em->server);
	g_at_ppp_resume(em->ppp);
}

static void dun_ato_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtIO *io = g_at_server_get_io(em->server);
	GAtResultIter iter;
	int val;

	DBG("");

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &val) == FALSE)
			goto error;

		if (val != 0)
			goto error;

		/* Fall through */
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		if (em->ppp == NULL)
			goto error;

		g_at_server_send_intermediate(em->server, "CONNECT");
		g_at_io_set_write_done(io, resume_ppp, em);
		break;

	default:
error:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static struct indicator *find_indicator(struct ofono_emulator *em,
						const char *name, int *index)
{
	GSList *l;
	int i;

	for (i = 1, l = em->indicators; l; l = l->next, i++) {
		struct indicator *ind = l->data;

		if (g_str_equal(ind->name, name) == FALSE)
			continue;

		if (index)
			*index = i;

		return ind;
	}

	return NULL;
}

static struct ofono_call *find_call_with_status(struct ofono_emulator *em,
								int status)
{
	struct ofono_modem *modem;
	struct ofono_voicecall *vc;

	if (em->active_atom == NULL)
		return NULL;

	modem = __ofono_atom_get_modem(em->active_atom);

	vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL, modem);
	if (vc == NULL)
		return NULL;

	return __ofono_voicecall_find_call_with_status(vc, status);
}

static void notify_deferred_indicators(GAtServer *server, void *user_data)
{
	struct ofono_emulator *em = user_data;
	int i;
	char buf[20];
	GSList *l;
	struct indicator *ind;

	for (i = 1, l = em->indicators; l; l = l->next, i++) {
		ind = l->data;

		if (!ind->deferred)
			continue;

		if (em->events_mode == 3 && em->events_ind && em->slc &&
				ind->active) {
			sprintf(buf, "+CIEV: %d,%d", i, ind->value);
			g_at_server_send_unsolicited(em->server, buf);
		}

		ind->deferred = FALSE;
	}
}

static gboolean notify_ccwa(void *user_data)
{
	struct ofono_emulator *em = user_data;
	struct ofono_call *c;
	const char *phone;
	/*
	 * '+CCWA: "+",' + phone number + phone type on 3 digits max
	 * + terminating null
	 */
	char str[OFONO_MAX_PHONE_NUMBER_LENGTH + 14 + 1];

	if ((em->type == OFONO_EMULATOR_TYPE_HFP && em->slc == FALSE) ||
			!em->ccwa)
		goto end;

	c = find_call_with_status(em, CALL_STATUS_WAITING);

	if (c && c->clip_validity == CLIP_VALIDITY_VALID) {
		phone = phone_number_to_string(&c->phone_number);
		sprintf(str, "+CCWA: \"%s\",%d", phone, c->phone_number.type);

		g_at_server_send_unsolicited(em->server, str);
	} else
		g_at_server_send_unsolicited(em->server, "+CCWA: \"\",128");

end:
	em->callsetup_source = 0;

	return FALSE;
}

static gboolean notify_ring(void *user_data)
{
	struct ofono_emulator *em = user_data;
	struct ofono_call *c;
	const char *phone;
	/*
	 * '+CLIP: "+",' + phone number + phone type on 3 digits max
	 * + terminating null
	 */
	char str[OFONO_MAX_PHONE_NUMBER_LENGTH + 14 + 1];

	if (em->type == OFONO_EMULATOR_TYPE_HFP && em->slc == FALSE)
		return TRUE;

	g_at_server_send_unsolicited(em->server, "RING");

	if (!em->clip)
		return TRUE;

	c = find_call_with_status(em, CALL_STATUS_INCOMING);

	if (c == NULL)
		return TRUE;

	switch (c->clip_validity) {
	case CLIP_VALIDITY_VALID:
		phone = phone_number_to_string(&c->phone_number);
		sprintf(str, "+CLIP: \"%s\",%d", phone, c->phone_number.type);
		g_at_server_send_unsolicited(em->server, str);
		break;

	case CLIP_VALIDITY_WITHHELD:
		g_at_server_send_unsolicited(em->server, "+CLIP: \"\",128");
		break;
	}

	return TRUE;
}

static void brsf_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;
	char buf[16];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &val) == FALSE)
			goto fail;

		if (val < 0 || val > 0xffff)
			goto fail;

		em->r_features = val;

		sprintf(buf, "+BRSF: %d", em->l_features);
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cind_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GSList *l;
	struct indicator *ind;
	gsize size;
	int len;
	char *buf;
	char *tmp;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		/*
		 * "+CIND: " + terminating null + number of indicators *
		 * (max of 3 digits in the value + separator)
		 */
		size = 7 + 1 + (g_slist_length(em->indicators) * 4);
		buf = g_try_malloc0(size);
		if (buf == NULL)
			goto fail;

		len = sprintf(buf, "+CIND: ");
		tmp = buf + len;

		for (l = em->indicators; l; l = l->next) {
			ind = l->data;
			len = sprintf(tmp, "%s%d",
					l == em->indicators ? "" : ",",
					ind->value);
			tmp = tmp + len;
		}

		g_at_server_send_info(em->server, buf, TRUE);
		g_free(buf);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		/*
		 * '+CIND: ' + terminating null + number of indicators *
		 * ( indicator name + '("",(000,000))' + separator)
		 */
		size = 8;

		for (l = em->indicators; l; l = l->next) {
			ind = l->data;
			size += strlen(ind->name) + 15;
		}

		buf = g_try_malloc0(size);
		if (buf == NULL)
			goto fail;

		len = sprintf(buf, "+CIND: ");
		tmp = buf + len;

		for (l = em->indicators; l; l = l->next) {
			ind = l->data;
			len = sprintf(tmp, "%s(\"%s\",(%d%c%d))",
					l == em->indicators ? "" : ",",
					ind->name, ind->min,
					(ind->max - ind->min) == 1 ? ',' : '-',
					ind->max);
			tmp = tmp + len;
		}

		g_at_server_send_info(server, buf, TRUE);
		g_free(buf);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cmer_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	char buf[32];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		sprintf(buf, "+CMER: %d,0,0,%d,0", em->events_mode,
						em->events_ind);
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		sprintf(buf, "+CMER: (0,3),(0),(0),(0,1),(0)");
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		int mode = em->events_mode;
		int ind = em->events_ind;
		int val;

		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		/* mode */
		if (!g_at_result_iter_next_number_default(&iter, mode, &mode))
			goto fail;

		if (mode != 0 && mode != 3)
			goto fail;

		/* keyp */
		if (!g_at_result_iter_next_number_default(&iter, 0, &val)) {
			if (!g_at_result_iter_skip_next(&iter))
				goto done;
			goto fail;
		}

		if (val != 0)
			goto fail;

		/* disp */
		if (!g_at_result_iter_next_number_default(&iter, 0, &val)) {
			if (!g_at_result_iter_skip_next(&iter))
				goto done;
			goto fail;
		}

		if (val != 0)
			goto fail;

		/* ind */
		if (!g_at_result_iter_next_number_default(&iter, ind, &ind)) {
			if (!g_at_result_iter_skip_next(&iter))
				goto done;
			goto fail;
		}

		if (ind != 0 && ind != 1)
			goto fail;

		/* bfr */
		if (!g_at_result_iter_next_number_default(&iter, 0, &val)) {
			if (!g_at_result_iter_skip_next(&iter))
				goto done;
			goto fail;
		}

		if (val != 0)
			goto fail;

		/* check that bfr is last parameter */
		if (g_at_result_iter_skip_next(&iter))
			goto fail;

done:
		em->events_mode = mode;
		em->events_ind = ind;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

		__ofono_emulator_slc_condition(em,
					OFONO_EMULATOR_SLC_CONDITION_CMER);
		break;
	}

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void clip_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;

	if (em->slc == FALSE)
		goto fail;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (!g_at_result_iter_next_number(&iter, &val))
			goto fail;

		if (val != 0 && val != 1)
			goto fail;

		/* check this is last parameter */
		if (g_at_result_iter_skip_next(&iter))
			goto fail;

		em->clip = val;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void ccwa_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;
	struct indicator *call_ind;
	struct indicator *cs_ind;

	if (em->slc == FALSE)
		goto fail;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (!g_at_result_iter_next_number(&iter, &val))
			goto fail;

		if (val != 0 && val != 1)
			goto fail;

		/* check this is last parameter */
		if (g_at_result_iter_skip_next(&iter))
			goto fail;

		call_ind = find_indicator(em, OFONO_EMULATOR_IND_CALL, NULL);
		cs_ind = find_indicator(em, OFONO_EMULATOR_IND_CALLSETUP, NULL);

		if (cs_ind->value == OFONO_EMULATOR_CALLSETUP_INCOMING &&
				call_ind->value == OFONO_EMULATOR_CALL_ACTIVE &&
				em->ccwa == FALSE && val == 1)
			em->callsetup_source = g_timeout_add_seconds(0,
							notify_ccwa, em);

		em->ccwa = val;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void cmee_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;
	char buf[16];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &val) == FALSE)
			goto fail;

		if (val != 0 && val != 1)
			goto fail;

		em->cmee_mode = val;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		sprintf(buf, "+CMEE: %d", em->cmee_mode);
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		/* HFP only support 0 and 1 */
		sprintf(buf, "+CMEE: (0,1)");
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void bia_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		GSList *l;
		int val;

		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		/* check validity of the request */
		while (g_at_result_iter_next_number_default(&iter, 0, &val))
			if (val != 0 &&  val != 1)
				goto fail;

		/* Check that we have no non-numbers in the stream */
		if (g_at_result_iter_skip_next(&iter) == TRUE)
			goto fail;

		/* request is valid, update the indicator activation status */
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		for (l = em->indicators; l; l = l->next) {
			struct indicator *ind = l->data;

			if (g_at_result_iter_next_number_default(&iter,
						ind->active, &val) == FALSE)
				break;

			if (ind->mandatory == TRUE)
				continue;

			ind->active = val;
		}

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	}

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void bind_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	char buf[128];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_info(em->server, "+BIND: 1,1", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

		__ofono_emulator_slc_condition(em,
					OFONO_EMULATOR_SLC_CONDITION_BIND);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		sprintf(buf, "+BIND: (1)");
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		int hf_indicator;
		int num_hf_indicators = 0;

		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		/* check validity of the request */
		while (num_hf_indicators < 20 &&
				g_at_result_iter_next_number(&iter,
							&hf_indicator)) {
			if (hf_indicator > 0xffff)
				goto fail;

			num_hf_indicators += 1;
		}

		/* Check that we have nothing extra in the stream */
		if (g_at_result_iter_skip_next(&iter) == TRUE)
			goto fail;

		/* request is valid, update the indicator activation status */
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		while (g_at_result_iter_next_number(&iter, &hf_indicator))
			ofono_info("HF supports indicator: 0x%04x",
					hf_indicator);

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

		break;
	}

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void biev_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		int hf_indicator;
		int val;

		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &hf_indicator) == FALSE)
			goto fail;

		if (hf_indicator != HFP_HF_INDICATOR_ENHANCED_SAFETY)
			goto fail;

		if (em->ddr_active == FALSE)
			goto fail;

		if (g_at_result_iter_next_number(&iter, &val) == FALSE)
			goto fail;

		if (val < 0 || val > 1)
			goto fail;

		ofono_info("Enhanced Safety indicator: %d", val);

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	}

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void finish_codec_negotiation(struct ofono_emulator *em,
			int err)
{
	if (em->codec_negotiation_cb == NULL)
		return;

	em->codec_negotiation_cb(err, em->codec_negotiation_data);

	em->codec_negotiation_cb = NULL;
	em->codec_negotiation_data = NULL;
}

static void bac_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;

	DBG("");

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		/*
		 * CVSD codec is mandatory and must come first.
		 * See HFP v1.6 4.34.1
		 */
		if (g_at_result_iter_next_number(&iter, &val) == FALSE ||
				val != HFP_CODEC_CVSD)
			goto fail;

		em->bac_received = TRUE;

		em->negotiated_codec = 0;
		em->r_codecs[CVSD_OFFSET].supported = TRUE;

		while (g_at_result_iter_next_number(&iter, &val)) {
			switch (val) {
			case HFP_CODEC_MSBC:
				em->r_codecs[MSBC_OFFSET].supported = TRUE;
				break;
			default:
				DBG("Unsupported HFP codec %d", val);
				break;
			}
		}

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

		/*
		 * If we're currently in the process of selecting a codec
		 * we have to restart that now
		 */
		if (em->proposed_codec) {
			em->proposed_codec = 0;
			ofono_emulator_start_codec_negotiation(em, NULL, NULL);
		}

		break;

	default:
fail:
		DBG("Process AT+BAC failed");
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);

		finish_codec_negotiation(em, -EIO);

		break;
	}
}

static void connect_sco(struct ofono_emulator *em)
{
	int err;

	DBG("");

	if (em->card == NULL) {
		finish_codec_negotiation(em, -EINVAL);
		return;
	}

	err = ofono_handsfree_card_connect_sco(em->card);
	if (err == 0) {
		finish_codec_negotiation(em, 0);
		return;
	}

	/* If we have another codec we can try then lets do that */
	if (em->negotiated_codec != HFP_CODEC_CVSD) {
		em->selected_codec = HFP_CODEC_CVSD;
		ofono_emulator_start_codec_negotiation(em,
					em->codec_negotiation_cb,
					em->codec_negotiation_data);
		return;
	}

	finish_codec_negotiation(em, -EIO);
}

static void bcs_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (!g_at_result_iter_next_number(&iter, &val))
			break;

		if (em->proposed_codec != val) {
			em->proposed_codec = 0;
			break;
		}

		em->proposed_codec = 0;
		em->negotiated_codec = val;

		DBG("negotiated codec %d", val);

		if (em->card != NULL)
			ofono_handsfree_card_set_codec(em->card,
							em->negotiated_codec);

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

		connect_sco(em);

		return;
	default:
		break;
	}

	finish_codec_negotiation(em, -EIO);

	g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
}

static void bcc_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

		if (!em->negotiated_codec) {
			ofono_emulator_start_codec_negotiation(em, NULL, NULL);
			return;
		}

		connect_sco(em);

		return;
	default:
		break;
	}

	g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
}

static void emulator_add_indicator(struct ofono_emulator *em, const char* name,
					int min, int max, int dflt,
					gboolean mandatory)
{
	struct indicator *ind;

	ind = g_try_new0(struct indicator, 1);
	if (ind == NULL) {
		ofono_error("Unable to allocate indicator structure");
		return;
	}

	ind->name = g_strdup(name);
	ind->min = min;
	ind->max = max;
	ind->value = dflt;
	ind->active = TRUE;
	ind->mandatory = mandatory;

	em->indicators = g_slist_append(em->indicators, ind);
}

static void free_atom_callback(gpointer data)
{
	struct atom_callback *atom_cb = data;

	if (atom_cb->destroy)
		atom_cb->destroy(atom_cb->data);

	g_free(atom_cb);
}

static void atom_cbs_destroy(gpointer key, gpointer value, gpointer user_data)
{
	GSList *atom_cbs = value;

	g_slist_free_full(atom_cbs, free_atom_callback);
}

static void emulator_unregister(struct ofono_atom *atom)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	GSList *l;

	DBG("%p %p", em, atom);

	/* Do the clean up when this is the last remaining atom */
	if (em->atoms != NULL && em->atoms->next != NULL)
		return;

	DBG("Last atom unregistering");

	if (em->callsetup_source) {
		g_source_remove(em->callsetup_source);
		em->callsetup_source = 0;
	}

	for (l = em->indicators; l; l = l->next) {
		struct indicator *ind = l->data;

		g_free(ind->name);
		g_free(ind);
	}

	g_slist_free(em->indicators);
	em->indicators = NULL;

	g_at_ppp_unref(em->ppp);
	em->ppp = NULL;

	if (em->pns_id > 0) {
		__ofono_private_network_release(em->pns_id);
		em->pns_id = 0;
	}

	g_at_server_unref(em->server);
	em->server = NULL;

	g_hash_table_foreach(em->prefixes, atom_cbs_destroy, NULL);
	g_hash_table_destroy(em->prefixes);
	em->prefixes = NULL;

	ofono_handsfree_card_remove(em->card);
	em->card = NULL;
}

void ofono_emulator_register(struct ofono_emulator *em, int fd)
{
	GIOChannel *io;
	GSList *l;

	DBG("%p, %d", em, fd);

	if (fd < 0)
		return;

	io = g_io_channel_unix_new(fd);

	em->server = g_at_server_new(io);
	if (em->server == NULL)
		return;

	g_io_channel_unref(io);

	g_at_server_set_debug(em->server, emulator_debug, "Server");
	g_at_server_set_disconnect_function(em->server,
						emulator_disconnect, em);
	g_at_server_set_finish_callback(em->server, notify_deferred_indicators,
						em);

	if (em->type == OFONO_EMULATOR_TYPE_HFP) {
		em->ddr_active = true;

		emulator_add_indicator(em, OFONO_EMULATOR_IND_SERVICE, 0, 1, 0,
									FALSE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_CALL, 0, 1, 0,
									TRUE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_CALLSETUP, 0, 3,
								0, TRUE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_CALLHELD, 0, 2,
								0, TRUE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_SIGNAL, 0, 5, 0,
									FALSE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_ROAMING, 0, 1, 0,
									FALSE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_BATTERY, 0, 5, 5,
									FALSE);

		g_at_server_register(em->server, "+BRSF", brsf_cb, em, NULL);
		g_at_server_register(em->server, "+CIND", cind_cb, em, NULL);
		g_at_server_register(em->server, "+CMER", cmer_cb, em, NULL);
		g_at_server_register(em->server, "+CLIP", clip_cb, em, NULL);
		g_at_server_register(em->server, "+CCWA", ccwa_cb, em, NULL);
		g_at_server_register(em->server, "+CMEE", cmee_cb, em, NULL);
		g_at_server_register(em->server, "+BIA", bia_cb, em, NULL);
		g_at_server_register(em->server, "+BIND", bind_cb, em, NULL);
		g_at_server_register(em->server, "+BIEV", biev_cb, em, NULL);
		g_at_server_register(em->server, "+BAC", bac_cb, em, NULL);
		g_at_server_register(em->server, "+BCC", bcc_cb, em, NULL);
		g_at_server_register(em->server, "+BCS", bcs_cb, em, NULL);
	}

	for (l = em->atoms; l; l = l->next) {
		struct ofono_atom *atom = l->data;

		__ofono_atom_register(atom, emulator_unregister);
	}

	switch (em->type) {
	case OFONO_EMULATOR_TYPE_DUN:
		g_at_server_register(em->server, "D", dial_cb, em, NULL);
		g_at_server_register(em->server, "H", dun_ath_cb, em, NULL);
		g_at_server_register(em->server, "O", dun_ato_cb, em, NULL);
		break;
	case OFONO_EMULATOR_TYPE_HFP:
		g_at_server_set_echo(em->server, FALSE);
		break;
	default:
		break;
	}
}

static void emulator_remove(struct ofono_atom *atom)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	DBG("em: %p, atom: %p", em, atom);

	em->atoms = g_slist_remove(em->atoms, atom);

	if (em->atoms != NULL)
		return;

	DBG("Removing emulator");
	g_free(em);
}

struct ofono_emulator *ofono_emulator_create(GList *modems,
						enum ofono_emulator_type type)
{
	struct ofono_emulator *em;
	enum ofono_atom_type atom_t;
	GList *l;

	if (type == OFONO_EMULATOR_TYPE_DUN)
		atom_t = OFONO_ATOM_TYPE_EMULATOR_DUN;
	else if (type == OFONO_EMULATOR_TYPE_HFP)
		atom_t = OFONO_ATOM_TYPE_EMULATOR_HFP;
	else
		return NULL;

	em = g_try_new0(struct ofono_emulator, 1);

	if (em == NULL)
		return NULL;

	em->type = type;
	em->l_features |= HFP_AG_FEATURE_3WAY;
	em->l_features |= HFP_AG_FEATURE_REJECT_CALL;
	em->l_features |= HFP_AG_FEATURE_ENHANCED_CALL_STATUS;
	em->l_features |= HFP_AG_FEATURE_ENHANCED_CALL_CONTROL;
	em->l_features |= HFP_AG_FEATURE_EXTENDED_RES_CODE;
	em->l_features |= HFP_AG_FEATURE_HF_INDICATORS;
	em->l_features |= HFP_AG_FEATURE_CODEC_NEGOTIATION;
	em->events_mode = 3;	/* default mode is forwarding events */
	em->cmee_mode = 0;	/* CME ERROR disabled by default */

	em->prefixes = g_hash_table_new_full(g_str_hash, g_str_equal,
								g_free, NULL);

	for (l = modems; l; l = l->next) {
		struct ofono_atom *atom;
		struct ofono_modem *modem = l->data;

		DBG("modem: %p, type: %d", modem, type);

		atom = __ofono_modem_add_atom_offline(modem, atom_t,
							emulator_remove, em);
		em->atoms = g_slist_prepend(em->atoms, atom);
	}

	return em;
}

static void free_atom(gpointer data)
{
	struct ofono_atom *atom = data;

	__ofono_atom_free(atom);
}

void ofono_emulator_remove(struct ofono_emulator *em)
{
	/*
	 * emulator_remove removes the atom from the list and eventually removes
	 * the emulator too, so we need to make this copy for a safe removal.
	 */
	GSList *atoms = g_slist_copy(em->atoms);

	g_slist_free_full(atoms, free_atom);
}

void ofono_emulator_send_final(struct ofono_emulator *em,
				const struct ofono_error *final)
{
	char buf[256];

	/*
	 * TODO: Handle various CMEE modes and report error strings from
	 * common.c
	 */
	switch (final->type) {
	case OFONO_ERROR_TYPE_CMS:
		sprintf(buf, "+CMS ERROR: %d", final->error);
		g_at_server_send_ext_final(em->server, buf);
		break;

	case OFONO_ERROR_TYPE_CME:
		switch (em->cmee_mode) {
		case 1:
			sprintf(buf, "+CME ERROR: %d", final->error);
			break;

		case 2:
			sprintf(buf, "+CME ERROR: %s",
						telephony_error_to_str(final));
			break;

		default:
			goto failure;
		}

		g_at_server_send_ext_final(em->server, buf);
		break;

	case OFONO_ERROR_TYPE_NO_ERROR:
		g_at_server_send_final(em->server, G_AT_SERVER_RESULT_OK);
		break;

	case OFONO_ERROR_TYPE_CEER:
	case OFONO_ERROR_TYPE_SIM:
	case OFONO_ERROR_TYPE_FAILURE:
	case OFONO_ERROR_TYPE_ERRNO:
failure:
		g_at_server_send_final(em->server, G_AT_SERVER_RESULT_ERROR);
		break;
	};
}

void ofono_emulator_send_unsolicited(struct ofono_emulator *em,
					const char *result)
{
	g_at_server_send_unsolicited(em->server, result);
}

void ofono_emulator_send_intermediate(struct ofono_emulator *em,
					const char *result)
{
	g_at_server_send_intermediate(em->server, result);
}

void ofono_emulator_send_info(struct ofono_emulator *em, const char *line,
				ofono_bool_t last)
{
	g_at_server_send_info(em->server, line, last);
}

static struct ofono_atom *get_preferred_atom(struct ofono_emulator *em)
{
	char *path;
	GSList *l;
	struct ofono_atom *preferred = em->atoms->data;

	path = __ofono_system_settings_get_string_value(PREFERRED_VOICE_MODEM);
	if (path == NULL)
		goto end;

	for (l = em->atoms; l; l = l->next) {
		struct ofono_atom *atom = l->data;

		if (g_strcmp0(__ofono_atom_get_path(atom), path) == 0) {
			preferred = atom;
			break;
		}
	}

	g_free(path);

end:
	return preferred;
}

static struct ofono_atom *get_active_atom(struct ofono_emulator *em)
{
	if (em->active_atom)
		return em->active_atom;

	return get_preferred_atom(em);
}

static struct atom_callback *find_atom_callback(GSList *prefix_cbs,
						const struct ofono_atom *atom)
{
	GSList *l;

	for (l = prefix_cbs; l; l = prefix_cbs->next) {
		struct atom_callback *atom_cb = l->data;

		if (atom_cb->atom != atom)
			continue;

		return atom_cb;
	}

	return NULL;
}

struct handler {
	char *prefix;
	struct ofono_emulator *em;
};

struct ofono_emulator_request {
	GAtResultIter iter;
	enum ofono_emulator_request_type type;
};

static void handler_proxy(GAtServer *server, GAtServerRequestType type,
					GAtResult *result, gpointer userdata)
{
	GSList *prefix_cbs;
	struct atom_callback *atom_cb;
	struct handler *h = userdata;
	struct ofono_emulator_request req;
	struct ofono_atom *atom;

	atom = get_active_atom(h->em);
	prefix_cbs = g_hash_table_lookup(h->em->prefixes, h->prefix);
	atom_cb = find_atom_callback(prefix_cbs, atom);
	if (atom_cb == NULL) {
		ofono_error("%s: No atom for prefix %s", __func__, h->prefix);
		return;
	}

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY;
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_SET;
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_QUERY;
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_SUPPORT;
	}

	g_at_result_iter_init(&req.iter, result);
	g_at_result_iter_next(&req.iter, "");

	atom_cb->cb(h->em, &req, atom_cb->data);
}

static void handler_proxy_need_slc(GAtServer *server,
					GAtServerRequestType type,
					GAtResult *result, gpointer userdata)
{
	struct handler *h = userdata;

	if (h->em->slc == FALSE) {
		g_at_server_send_final(h->em->server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	handler_proxy(server, type, result, userdata);
}

static void handler_proxy_chld(GAtServer *server, GAtServerRequestType type,
				GAtResult *result, gpointer userdata)
{
	struct handler *h = userdata;

	if (h->em->slc == FALSE && type != G_AT_SERVER_REQUEST_TYPE_SUPPORT) {
		g_at_server_send_final(h->em->server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	handler_proxy(server, type, result, userdata);
}

static void handler_destroy(gpointer userdata)
{
	struct handler *h = userdata;

	g_free(h->prefix);
	g_free(h);
}

ofono_bool_t ofono_emulator_add_handler(struct ofono_atom *atom,
					const char *prefix,
					ofono_emulator_request_cb_t cb,
					void *data, ofono_destroy_func destroy)
{
	struct handler *h;
	GSList *prefix_cbs;
	struct atom_callback *atom_cb;
	GAtServerNotifyFunc func = handler_proxy;
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	gboolean already_registered;

	DBG("%p %s cb %p", atom, prefix, cb);

	prefix_cbs = g_hash_table_lookup(em->prefixes, prefix);
	already_registered = prefix_cbs ? TRUE : FALSE;

	if (find_atom_callback(prefix_cbs, atom)) {
		ofono_info("%s: Atom %p already registered for prefix %s",
							__func__, atom, prefix);
		return FALSE;
	}

	atom_cb = g_malloc0(sizeof(*atom_cb));
	atom_cb->atom = atom;
	atom_cb->cb = cb;
	atom_cb->data = data;
	atom_cb->destroy = destroy;

	/*
	 * Append to avoid modifying the head, which is the value in the hash
	 * table. We refresh it for the case of the first insertion though.
	 */
	prefix_cbs = g_slist_append(prefix_cbs, atom_cb);

	if (already_registered)
		return TRUE;

	g_hash_table_insert(em->prefixes, g_strdup(prefix), prefix_cbs);

	h = g_new0(struct handler, 1);
	h->prefix = g_strdup(prefix);
	h->em = em;

	if (em->type == OFONO_EMULATOR_TYPE_HFP) {
		func = handler_proxy_need_slc;

		if (!strcmp(prefix, "+CHLD"))
			func = handler_proxy_chld;
	}

	if (g_at_server_register(em->server, prefix, func, h,
					handler_destroy) == TRUE)
		return TRUE;

	g_free(h);

	return FALSE;
}

ofono_bool_t ofono_emulator_remove_handler(struct ofono_atom *atom,
						const char *prefix)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	GSList *prefix_cbs;
	struct atom_callback *atom_cb;

	prefix_cbs = g_hash_table_lookup(em->prefixes, prefix);
	atom_cb = find_atom_callback(prefix_cbs, atom);

	if (atom_cb == NULL)
		return FALSE;

	prefix_cbs = g_slist_remove(prefix_cbs, atom_cb);

	free_atom_callback(atom_cb);

	if (prefix_cbs != NULL) {
		g_hash_table_replace(em->prefixes,
						g_strdup(prefix), prefix_cbs);
		return TRUE;
	}

	g_hash_table_remove(em->prefixes, prefix);

	return g_at_server_unregister(em->server, prefix);
}

ofono_bool_t ofono_emulator_request_next_string(
					struct ofono_emulator_request *req,
					const char **str)
{
	return g_at_result_iter_next_string(&req->iter, str);
}

ofono_bool_t ofono_emulator_request_next_number(
					struct ofono_emulator_request *req,
					int *number)
{
	return g_at_result_iter_next_number(&req->iter, number);
}

const char *ofono_emulator_request_get_raw(struct ofono_emulator_request *req)
{
	return g_at_result_iter_raw_line(&req->iter);
}

enum ofono_emulator_request_type ofono_emulator_request_get_type(
					struct ofono_emulator_request *req)
{
	return req->type;
}

static gboolean valid_indication(struct ofono_emulator *em,
				struct ofono_atom *atom, const char *name)
{
	struct ofono_atom *preferred;

	if (g_strcmp0(name, OFONO_EMULATOR_IND_BATTERY) == 0)
		return TRUE;

	if (em->active_atom) {
		if (em->active_atom == atom)
			return TRUE;
		else
			return FALSE;
	} else if (g_strcmp0(name, OFONO_EMULATOR_IND_CALL) == 0
			|| g_strcmp0(name, OFONO_EMULATOR_IND_CALLSETUP) == 0) {
		return TRUE;
	}

	/* Return FALSE if the modem is not the preferred one */
	preferred = get_preferred_atom(em);
	if (preferred == atom)
		return TRUE;
	else
		return FALSE;
}

void ofono_emulator_set_indicator(struct ofono_atom *atom,
						const char *name, int value)
{
	int i;
	char buf[20];
	struct indicator *ind;
	struct indicator *call_ind;
	struct indicator *cs_ind;
	gboolean call;
	gboolean callsetup;
	gboolean waiting;
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	if (!valid_indication(em, atom, name))
		return;

	DBG("%s\t%d", name, value);

	ind = find_indicator(em, name, &i);

	if (ind == NULL || ind->value == value || value < ind->min
			|| value > ind->max)
		return;

	ind->value = value;

	call_ind = find_indicator(em, OFONO_EMULATOR_IND_CALL, NULL);
	cs_ind = find_indicator(em, OFONO_EMULATOR_IND_CALLSETUP, NULL);

	call = ind == call_ind;
	callsetup = ind == cs_ind;

	if (call || callsetup) {
		if (call_ind->value == OFONO_EMULATOR_CALL_INACTIVE
				&& cs_ind->value ==
					OFONO_EMULATOR_CALLSETUP_INACTIVE) {
			DBG("Call finished for HFP atom %p", atom);
			em->active_atom = NULL;
		} else if (em->active_atom == NULL) {
			DBG("New call from HFP atom %p", atom);
			em->active_atom = atom;
		}
	}

	/*
	 * When callsetup indicator goes to Incoming and there is an active
	 * call a +CCWA should be sent before +CIEV
	 */
	waiting = (callsetup && value == OFONO_EMULATOR_CALLSETUP_INCOMING &&
			call_ind->value == OFONO_EMULATOR_CALL_ACTIVE);

	if (waiting)
		notify_ccwa(em);

	if (em->events_mode == 3 && em->events_ind && em->slc && ind->active) {
		if (!g_at_server_command_pending(em->server)) {
			sprintf(buf, "+CIEV: %d,%d", i, ind->value);
			g_at_server_send_unsolicited(em->server, buf);
		} else
			ind->deferred = TRUE;
	}

	/*
	 * Ring timer should be started when:
	 * - callsetup indicator is set to Incoming and there is no active call
	 *   (not a waiting call)
	 * - or call indicator is set to inactive while callsetup is already
	 *   set to Incoming.
	 * In those cases, a first RING should be sent just after the +CIEV
	 * Ring timer should be stopped for all other values of callsetup
	 */
	if (waiting)
		return;

	/* Call state went from active/held + waiting -> incoming */
	if (call && value == OFONO_EMULATOR_CALL_INACTIVE &&
			cs_ind->value == OFONO_EMULATOR_CALLSETUP_INCOMING)
		goto start_ring;

	if (!callsetup)
		return;

	if (value != OFONO_EMULATOR_CALLSETUP_INCOMING) {
		if (em->callsetup_source > 0) {
			g_source_remove(em->callsetup_source);
			em->callsetup_source = 0;
		}

		return;
	}

start_ring:
	notify_ring(em);
	em->callsetup_source = g_timeout_add_seconds(RING_TIMEOUT,
							notify_ring, em);
}

void __ofono_emulator_set_indicator_forced(struct ofono_atom *atom,
						const char *name, int value)
{
	int i;
	struct indicator *ind;
	char buf[20];
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	if (!valid_indication(em, atom, name))
		return;

	ind = find_indicator(em, name, &i);

	if (ind == NULL || value < ind->min || value > ind->max)
		return;

	ind->value = value;

	if (em->events_mode == 3 && em->events_ind && em->slc && ind->active) {
		if (!g_at_server_command_pending(em->server)) {
			sprintf(buf, "+CIEV: %d,%d", i, ind->value);
			g_at_server_send_unsolicited(em->server, buf);
		} else
			ind->deferred = TRUE;
	}
}

void __ofono_emulator_slc_condition(struct ofono_emulator *em,
					enum ofono_emulator_slc_condition cond)
{
	if (em->slc == TRUE)
		return;

	switch (cond) {
	case OFONO_EMULATOR_SLC_CONDITION_CMER:
		if ((em->r_features & HFP_HF_FEATURE_3WAY) &&
				(em->l_features & HFP_AG_FEATURE_3WAY))
			return;
		/* Fall Through */

	case OFONO_EMULATOR_SLC_CONDITION_CHLD:
		if ((em->r_features & HFP_HF_FEATURE_HF_INDICATORS) &&
				(em->l_features & HFP_HF_FEATURE_HF_INDICATORS))
			return;
		/* Fall Through */

	case OFONO_EMULATOR_SLC_CONDITION_BIND:
		ofono_info("SLC reached");
		em->slc = TRUE;

		ofono_handsfree_card_register(em->card);

	default:
		break;
	}
}

void ofono_emulator_set_hf_indicator_active(struct ofono_emulator *em,
						int indicator,
						ofono_bool_t active)
{
	char buf[64];

	if (!(em->l_features & HFP_HF_FEATURE_HF_INDICATORS))
		return;

	if (!(em->r_features & HFP_HF_FEATURE_HF_INDICATORS))
		return;

	if (indicator != HFP_HF_INDICATOR_ENHANCED_SAFETY)
		return;

	em->ddr_active = active;

	sprintf(buf, "+BIND: %d,%d", HFP_HF_INDICATOR_ENHANCED_SAFETY, active);
	g_at_server_send_unsolicited(em->server, buf);
}

void ofono_emulator_set_handsfree_card(struct ofono_emulator *em,
					struct ofono_handsfree_card *card)
{
	if (em == NULL)
		return;

	em->card = card;
}

static unsigned char select_codec(struct ofono_emulator *em)
{
	if (ofono_handsfree_audio_has_wideband() &&
			em->r_codecs[MSBC_OFFSET].supported)
		return HFP_CODEC_MSBC;

	/* CVSD is mandatory for both sides */
	return HFP_CODEC_CVSD;
}

int ofono_emulator_start_codec_negotiation(struct ofono_emulator *em,
			ofono_emulator_codec_negotiation_cb cb, void *data)
{
	char buf[64];
	unsigned char codec;
	int err;

	if (em == NULL)
		return -EINVAL;

	if (cb != NULL && em->codec_negotiation_cb != NULL)
		return -EALREADY;

	if (em->proposed_codec > 0)
		return -EALREADY;

	if (!em->bac_received || em->negotiated_codec > 0) {
		/*
		 * If we didn't received any +BAC during the SLC setup the
		 * remote side doesn't support codec negotiation and we can
		 * directly connect our card. Otherwise if we got +BAC and
		 * already have a negotiated codec we can proceed here
		 * without doing any negotiation again.
		 *
		 * Report success/error via the callback even if we have not
		 * done any negotiation as the other side may have to clean up.
		 */
		err = ofono_handsfree_card_connect_sco(em->card);
		if (err < 0)
			ofono_error("SCO connection failed");

		cb(err, data);
		return 0;
	}

	if (em->selected_codec > 0) {
		codec = em->selected_codec;
		em->selected_codec = 0;
		goto done;
	}

	codec = select_codec(em);
	if (!codec) {
		DBG("Failed to select HFP codec");
		return -EINVAL;
	}

done:
	em->proposed_codec = codec;

	em->codec_negotiation_cb = cb;
	em->codec_negotiation_data = data;

	snprintf(buf, 64, "+BCS: %d", em->proposed_codec);
	g_at_server_send_unsolicited(em->server, buf);

	return 0;
}
