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

#include <string.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>

#include "qmi.h"
#include "dms.h"

#include "qmimodem.h"

struct devinfo_data {
	struct qmi_service *dms;
	bool device_is_3gpp;
};

static void string_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_devinfo_query_cb_t cb = cbd->cb;
	char *str;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	str = qmi_result_get_string(result, 0x01);
	if (!str) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, str, cbd->data);

	qmi_free(str);
}

static void qmi_query_manufacturer(struct ofono_devinfo *devinfo,
				ofono_devinfo_query_cb_t cb, void *user_data)
{
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->dms, QMI_DMS_GET_MANUFACTURER, NULL,
						string_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	g_free(cbd);
}

static void qmi_query_model(struct ofono_devinfo *devinfo,
				ofono_devinfo_query_cb_t cb, void *user_data)
{
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->dms, QMI_DMS_GET_MODEL_ID, NULL,
					string_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	g_free(cbd);
}

static void qmi_query_revision(struct ofono_devinfo *devinfo,
				ofono_devinfo_query_cb_t cb, void *user_data)
{
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->dms, QMI_DMS_GET_REV_ID, NULL,
					string_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	g_free(cbd);
}

static void get_ids_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_devinfo *devinfo = cbd->user;
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);
	ofono_devinfo_query_cb_t cb = cbd->cb;
	char *esn;
	char *imei;
	char *meid;
	char *str;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	esn = qmi_result_get_string(result, QMI_DMS_RESULT_ESN);
	imei = qmi_result_get_string(result, QMI_DMS_RESULT_IMEI);
	meid = qmi_result_get_string(result, QMI_DMS_RESULT_MEID);

	str = NULL;

	if (data->device_is_3gpp && imei && strcmp(imei, "0"))
		str = imei;
	else if (esn && strcmp(esn, "0"))
		str = esn;

	if (str == NULL && meid && strcmp(meid, "0"))
		str = meid;

	if (str)
		CALLBACK_WITH_SUCCESS(cb, str, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	qmi_free(esn);
	qmi_free(imei);
	qmi_free(meid);
}

static void qmi_query_serial(struct ofono_devinfo *devinfo,
				ofono_devinfo_query_cb_t cb, void *user_data)
{
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	cbd->user = devinfo;

	if (qmi_service_send(data->dms, QMI_DMS_GET_IDS, NULL,
					get_ids_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	g_free(cbd);
}

static void get_caps_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_devinfo *devinfo = user_data;
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);
	const struct qmi_dms_device_caps *caps;
	uint8_t i;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	caps = qmi_result_get(result, QMI_DMS_RESULT_DEVICE_CAPS, NULL);
	if (caps == NULL)
		goto error;

	data->device_is_3gpp = false;

	for (i = 0; i < caps->radio_if_count; i++) {
		switch (caps->radio_if[i]) {
		case QMI_DMS_RADIO_IF_GSM:
		case QMI_DMS_RADIO_IF_UMTS:
		case QMI_DMS_RADIO_IF_LTE:
			data->device_is_3gpp = true;
			break;
		}
	}

error:
	ofono_devinfo_register(devinfo);
}

static void qmi_query_caps(struct ofono_devinfo *devinfo)
{
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);

	DBG("");

	if (qmi_service_send(data->dms, QMI_DMS_GET_CAPS, NULL,
					get_caps_cb, devinfo, NULL) > 0)
		return;

	ofono_devinfo_register(devinfo);
}

static void create_dms_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_devinfo *devinfo = user_data;
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);

	DBG("");

	if (!service) {
		ofono_error("Failed to request DMS service");
		ofono_devinfo_remove(devinfo);
		return;
	}

	data->dms = qmi_service_ref(service);
	data->device_is_3gpp = false;

	qmi_query_caps(devinfo);
}

static int qmi_devinfo_probe(struct ofono_devinfo *devinfo,
				unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct devinfo_data *data;

	DBG("");

	data = g_new0(struct devinfo_data, 1);

	ofono_devinfo_set_data(devinfo, data);

	qmi_service_create_shared(device, QMI_SERVICE_DMS,
					create_dms_cb, devinfo, NULL);

	return 0;
}

static void qmi_devinfo_remove(struct ofono_devinfo *devinfo)
{
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);

	DBG("");

	ofono_devinfo_set_data(devinfo, NULL);

	qmi_service_unref(data->dms);

	g_free(data);
}

static const struct ofono_devinfo_driver driver = {
	.name			= "qmimodem",
	.probe			= qmi_devinfo_probe,
	.remove			= qmi_devinfo_remove,
	.query_manufacturer	= qmi_query_manufacturer,
	.query_model		= qmi_query_model,
	.query_revision		= qmi_query_revision,
	.query_serial		= qmi_query_serial,
};

void qmi_devinfo_init(void)
{
	ofono_devinfo_driver_register(&driver);
}

void qmi_devinfo_exit(void)
{
	ofono_devinfo_driver_unregister(&driver);
}
