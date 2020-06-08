/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
 *  Copyright (C) 2017 Alexander Couzens <lynxis@fe80.eu>
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

#include <string.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include <drivers/common/call_list.h>
#include "src/common.h"

#include "qmi.h"
#include "qmimodem.h"
#include "voice.h"
#include "voice_generated.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif


/* qmi protocol */


/* end of qmi */

struct voicecall_data {
	struct qmi_service *voice;
	uint16_t major;
	uint16_t minor;
	GSList *call_list;
	struct voicecall_static *vs;
	struct ofono_phone_number dialed;
};

static void all_call_status_ind(struct qmi_result *result, void *user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GSList *calls = NULL;
	int i;
	int size = 0;
	struct qmi_voice_all_call_status_ind status_ind;

	if (qmi_voice_call_status(result, &status_ind) != NONE) {
		DBG("Parsing of all call status indication failed");
		return;
	}

	if (!status_ind.remote_party_number_set || !status_ind.call_information_set) {
		DBG("Some required fields are not set");
		return;
	}

	size = status_ind.call_information->size;
	if (!size) {
		DBG("No call informations received!");
		return;
	}

	/* expect we have valid fields for every call */
	if (size != status_ind.remote_party_number_size)  {
		DBG("Not all fields have the same size");
		return;
	}

	for (i = 0; i < size; i++) {
		struct qmi_voice_call_information_instance call_info;
		struct ofono_call *call;
		const struct qmi_voice_remote_party_number_instance *remote_party = status_ind.remote_party_number[i];
		int number_size;

		call_info = status_ind.call_information->instance[i];
		call = g_new0(struct ofono_call, 1);
		call->id = call_info.id;
		call->direction = qmi_to_ofono_direction(call_info.direction);

		if (qmi_to_ofono_status(call_info.state, &call->status)) {
			DBG("Ignore call id %d, because can not convert QMI state 0x%x to ofono.",
			    call_info.id, call_info.state);
			continue;
		}
		DBG("Call %d in state %s(%d)",
		    call_info.id,
		    qmi_voice_call_state_name(call_info.state),
		    call_info.state);

		call->type = 0; /* always voice */
		number_size = remote_party->number_size;
		if (number_size > OFONO_MAX_PHONE_NUMBER_LENGTH)
			number_size = OFONO_MAX_PHONE_NUMBER_LENGTH;
		strncpy(call->phone_number.number, remote_party->number,
				number_size);
		/* FIXME: set phone_number_type */

		if (strlen(call->phone_number.number) > 0)
			call->clip_validity = 0;
		else
			call->clip_validity = 2;

		calls = g_slist_insert_sorted(calls, call, ofono_call_compare);
	}

	ofono_call_list_notify(vc, &vd->call_list, calls);
}

static void event_update(struct qmi_result *result, void *user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *data = ofono_voicecall_get_data(vc);

	DBG("");

	qmi_service_send(data->voice, QMI_VOICE_GET_ALL_STATUS, NULL,
				all_call_status_ind, vc, NULL);
}

static void create_voice_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *data = ofono_voicecall_get_data(vc);

	DBG("");

	if (!service) {
		ofono_error("Failed to request Voice service");
		ofono_voicecall_remove(vc);
		return;
	}

	if (!qmi_service_get_version(service, &data->major, &data->minor)) {
		ofono_error("Failed to get Voice service version");
		ofono_voicecall_remove(vc);
		return;
	}

	data->voice = qmi_service_ref(service);

	/* FIXME: we should call indication_register to ensure we get notified on call events.
	 * We rely at the moment on the default value of notifications
	 */
	qmi_service_register(data->voice, QMI_VOICE_IND_ALL_STATUS,
			     all_call_status_ind, vc, NULL);

	qmi_service_register(data->voice, QMI_SERVICE_UPDATE,
					event_update, vc, NULL);

	ofono_voicecall_register(vc);
}

static int qmi_voicecall_probe(struct ofono_voicecall *vc,
					unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct voicecall_data *data;

	DBG("");

	data = g_new0(struct voicecall_data, 1);

	ofono_voicecall_set_data(vc, data);

	qmi_service_create(device, QMI_SERVICE_VOICE,
					create_voice_cb, vc, NULL);

	return 0;
}

static void qmi_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *data = ofono_voicecall_get_data(vc);

	DBG("");

	ofono_voicecall_set_data(vc, NULL);

	qmi_service_unregister_all(data->voice);

	qmi_service_unref(data->voice);

	g_slist_free_full(data->call_list, g_free);

	g_free(data);
}

static void dial_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_voicecall *vc = cbd->user;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	ofono_voicecall_cb_t cb = cbd->cb;
	uint16_t error;
	struct qmi_voice_dial_call_result dial_result;

	if (qmi_result_set_error(result, &error)) {
		DBG("QMI Error %d", error);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	if (NONE != qmi_voice_dial_call_parse(result, &dial_result)) {
		DBG("Received invalid Result");
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	if (!dial_result.call_id_set) {
		DBG("Didn't receive a call id");
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	DBG("New call QMI id %d", dial_result.call_id);
	ofono_call_list_dial_callback(vc,
				      &vd->call_list,
				      &vd->dialed,
				      dial_result.call_id);


	/* FIXME: create a timeout on this call_id */
	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void dial(struct ofono_voicecall *vc, const struct ofono_phone_number *ph,
		enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
		void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct qmi_voice_dial_call_arg arg;

	cbd->user = vc;
	arg.calling_number_set = true;
	arg.calling_number = ph->number;
	memcpy(&vd->dialed, ph, sizeof(*ph));

	arg.call_type_set = true;
	arg.call_type = QMI_CALL_TYPE_VOICE_FORCE;

	if (!qmi_voice_dial_call(
				&arg,
				vd->voice,
				dial_cb,
				cbd,
				g_free))
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void answer_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_voicecall_cb_t cb = cbd->cb;
	uint16_t error;
	struct qmi_voice_answer_call_result answer_result;

	if (qmi_result_set_error(result, &error)) {
		DBG("QMI Error %d", error);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	/* TODO: what happens when calling it with no active call or wrong caller id? */
	if (NONE != qmi_voice_answer_call_parse(result, &answer_result)) {
		DBG("Received invalid Result");
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void answer(struct ofono_voicecall *vc, ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct qmi_voice_answer_call_arg arg;
	struct ofono_call *call;
	GSList *list;

	DBG("");
	cbd->user = vc;

	list = g_slist_find_custom(vd->call_list,
				   GINT_TO_POINTER(CALL_STATUS_INCOMING),
				   ofono_call_compare_by_status);

	if (list == NULL) {
		DBG("Can not find a call to answer");
		goto err;
	}

	call = list->data;

	arg.call_id_set = true;
	arg.call_id = call->id;

	if (!qmi_voice_answer_call(
				&arg,
				vd->voice,
				answer_cb,
				cbd,
				g_free))
		return;
err:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void end_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_voicecall_cb_t cb = cbd->cb;
	uint16_t error;
	struct qmi_voice_end_call_result end_result;

	if (qmi_result_set_error(result, &error)) {
		DBG("QMI Error %d", error);
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	if (NONE != qmi_voice_end_call_parse(result, &end_result)) {
		DBG("Received invalid Result");
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void release_specific(struct ofono_voicecall *vc, int id,
		ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	struct qmi_voice_end_call_arg arg;

	DBG("");
	cbd->user = vc;

	arg.call_id_set = true;
	arg.call_id = id;

	if (!qmi_voice_end_call(&arg,
				vd->voice,
				end_cb,
				cbd,
				g_free))
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void hangup_active(struct ofono_voicecall *vc,
		ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ofono_call *call;
	GSList *list = NULL;
	enum call_status active[] = {
		CALL_STATUS_ACTIVE,
		CALL_STATUS_DIALING,
		CALL_STATUS_ALERTING,
		CALL_STATUS_INCOMING,
	};
	int i;

	DBG("");
	for (i = 0; i < ARRAY_SIZE(active); i++) {
		list = g_slist_find_custom(vd->call_list,
					   GINT_TO_POINTER(active[i]),
					   ofono_call_compare_by_status);

		if (list)
			break;
	}

	if (list == NULL) {
		DBG("Can not find a call to hang up");
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	call = list->data;
	release_specific(vc, call->id, cb, data);
}

static const struct ofono_voicecall_driver driver = {
	.name		= "qmimodem",
	.probe		= qmi_voicecall_probe,
	.remove		= qmi_voicecall_remove,
	.dial		= dial,
	.answer		= answer,
	.hangup_active  = hangup_active,
	.release_specific  = release_specific,
};

void qmi_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void qmi_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
