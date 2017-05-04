/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>

#include "qmi.h"
#include "nas.h"

#include "src/common.h"
#include "qmimodem.h"

struct gprs_data {
	struct qmi_service *nas;
};

static bool extract_ss_info(struct qmi_result *result, int *status, int *tech)
{
	const struct qmi_nas_serving_system *ss;
	uint16_t len;
	int i;

	DBG("");

	ss = qmi_result_get(result, QMI_NAS_RESULT_SERVING_SYSTEM, &len);
	if (!ss)
		return false;

	if (ss->ps_state == QMI_NAS_ATTACH_STATE_ATTACHED)
		*status = NETWORK_REGISTRATION_STATUS_REGISTERED;
	else
		*status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;

	*tech = -1;
	for (i = 0; i < ss->radio_if_count; i++) {
		DBG("radio in use %d", ss->radio_if[i]);

		*tech = qmi_nas_rat_to_tech(ss->radio_if[i]);
	}

	return true;
}

static int handle_ss_info(struct qmi_result *result, struct ofono_gprs *gprs)
{
	int status;
	int tech;

	DBG("");

	if (!extract_ss_info(result, &status, &tech))
		return -1;

	if (status == NETWORK_REGISTRATION_STATUS_REGISTERED)
		if (tech == ACCESS_TECHNOLOGY_EUTRAN) {
			/* On LTE we are effectively always attached; and
			 * the default bearer is established as soon as the
			 * network is joined.
			 */
			/* FIXME: query default profile number and APN
			 * instead of assuming profile 1 and ""
			 */
			ofono_gprs_cid_activated(gprs, 1 , "automatic");
		}

	return status;
}

static void ss_info_notify(struct qmi_result *result, void *user_data)
{
	struct ofono_gprs *gprs = user_data;
	int status;

	DBG("");

	status = handle_ss_info(result, gprs);

	if (status >= 0)
		ofono_gprs_status_notify(gprs, status);
}

static void attach_detach_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_cb_t cb = cbd->cb;
	uint16_t error;

	DBG("");

	if (qmi_result_set_error(result, &error)) {
		if (error == 26) {
			/* no effect */
			goto done;
		}

		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

done:
	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void qmi_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *user_data)
{
	struct gprs_data *data = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_param *param;
	uint8_t action;

	DBG("attached %d", attached);

	if (attached)
		action = QMI_NAS_ATTACH_ACTION_ATTACH;
	else
		action = QMI_NAS_ATTACH_ACTION_DETACH;

	param = qmi_param_new_uint8(QMI_NAS_PARAM_ATTACH_ACTION, action);
	if (!param)
		goto error;

	if (qmi_service_send(data->nas, QMI_NAS_ATTACH_DETACH, param,
					attach_detach_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void get_ss_info_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_gprs *gprs = cbd->user;
	ofono_gprs_status_cb_t cb = cbd->cb;
	int status;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	status = handle_ss_info(result, gprs);

	if (status < 0)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, status, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void qmi_attached_status(struct ofono_gprs *gprs,
				ofono_gprs_status_cb_t cb, void *user_data)
{
	struct gprs_data *data = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	cbd->user = gprs;
	if (qmi_service_send(data->nas, QMI_NAS_GET_SS_INFO, NULL,
					get_ss_info_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

	g_free(cbd);
}

static void create_nas_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *data = ofono_gprs_get_data(gprs);

	DBG("");

	if (!service) {
		ofono_error("Failed to request NAS service");
		ofono_gprs_remove(gprs);
		return;
	}

	data->nas = qmi_service_ref(service);

	/*
	 * First get the SS info - the modem may already be connected,
	 * and the state-change notification may never arrive
	 */
	qmi_service_send(data->nas, QMI_NAS_GET_SS_INFO, NULL,
					ss_info_notify, gprs, NULL);

	qmi_service_register(data->nas, QMI_NAS_SS_INFO_IND,
					ss_info_notify, gprs, NULL);

	ofono_gprs_set_cid_range(gprs, 1, 1);

	ofono_gprs_register(gprs);
}

static int qmi_gprs_probe(struct ofono_gprs *gprs,
				unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct gprs_data *data;

	DBG("");

	data = g_new0(struct gprs_data, 1);

	ofono_gprs_set_data(gprs, data);

	qmi_service_create_shared(device, QMI_SERVICE_NAS,
						create_nas_cb, gprs, NULL);

	return 0;
}

static void qmi_gprs_remove(struct ofono_gprs *gprs)
{
	struct gprs_data *data = ofono_gprs_get_data(gprs);

	DBG("");

	ofono_gprs_set_data(gprs, NULL);

	qmi_service_unregister_all(data->nas);

	qmi_service_unref(data->nas);

	g_free(data);
}

static struct ofono_gprs_driver driver = {
	.name			= "qmimodem",
	.probe			= qmi_gprs_probe,
	.remove			= qmi_gprs_remove,
	.set_attached		= qmi_set_attached,
	.attached_status	= qmi_attached_status,
};

void qmi_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void qmi_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
