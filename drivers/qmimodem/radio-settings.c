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
#include <ofono/radio-settings.h>

#include "qmi.h"
#include "nas.h"
#include "dms.h"

#include "qmimodem.h"

struct settings_data {
	struct qmi_service *nas;
	struct qmi_service *dms;
	uint16_t major;
	uint16_t minor;
};

static void get_system_selection_pref_cb(struct qmi_result *result,
							void* user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	enum ofono_radio_access_mode mode = OFONO_RADIO_ACCESS_MODE_ANY;
	uint16_t pref;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	qmi_result_get_uint16(result,
			QMI_NAS_RESULT_SYSTEM_SELECTION_PREF_MODE, &pref);

	switch (pref) {
	case QMI_NAS_RAT_MODE_PREF_GSM:
		mode = OFONO_RADIO_ACCESS_MODE_GSM;
		break;
	case QMI_NAS_RAT_MODE_PREF_UMTS:
		mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		break;
	case QMI_NAS_RAT_MODE_PREF_LTE:
		mode = OFONO_RADIO_ACCESS_MODE_LTE;
		break;
	}

	CALLBACK_WITH_SUCCESS(cb, mode, cbd->data);
}

static void qmi_query_rat_mode(struct ofono_radio_settings *rs,
			ofono_radio_settings_rat_mode_query_cb_t cb,
			void *user_data)
{
	struct settings_data *data = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->nas,
				QMI_NAS_GET_SYSTEM_SELECTION_PREF, NULL,
				get_system_selection_pref_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void set_system_selection_pref_cb(struct qmi_result *result,
							void* user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void qmi_set_rat_mode(struct ofono_radio_settings *rs,
			enum ofono_radio_access_mode mode,
			ofono_radio_settings_rat_mode_set_cb_t cb,
			void *user_data)
{
	struct settings_data *data = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	uint16_t pref = QMI_NAS_RAT_MODE_PREF_ANY;
	struct qmi_param *param;

	DBG("");

	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		pref = QMI_NAS_RAT_MODE_PREF_ANY;
		break;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		pref = QMI_NAS_RAT_MODE_PREF_GSM;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		pref = QMI_NAS_RAT_MODE_PREF_UMTS;
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		pref = QMI_NAS_RAT_MODE_PREF_LTE;
		break;
	}

	param = qmi_param_new();
	if (!param) {
		CALLBACK_WITH_FAILURE(cb, user_data);
		return;
	}

	qmi_param_append_uint16(param, QMI_NAS_PARAM_SYSTEM_SELECTION_PREF_MODE,
			pref);

	if (qmi_service_send(data->nas,
				QMI_NAS_SET_SYSTEM_SELECTION_PREF, param,
				set_system_selection_pref_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);
	CALLBACK_WITH_FAILURE(cb, user_data);
	g_free(cbd);
}

static void get_caps_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_available_rats_query_cb_t cb = cbd->cb;
	const struct qmi_dms_device_caps *caps;
	unsigned int available_rats;
	uint16_t len;
	uint8_t i;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	caps = qmi_result_get(result, QMI_DMS_RESULT_DEVICE_CAPS, &len);
	if (!caps)
		goto error;

	available_rats = 0;
	for (i = 0; i < caps->radio_if_count; i++) {
		switch (caps->radio_if[i]) {
		case QMI_DMS_RADIO_IF_GSM:
			available_rats |= OFONO_RADIO_ACCESS_MODE_GSM;
			break;
		case QMI_DMS_RADIO_IF_UMTS:
			available_rats |= OFONO_RADIO_ACCESS_MODE_UMTS;
			break;
		case QMI_DMS_RADIO_IF_LTE:
			available_rats |= OFONO_RADIO_ACCESS_MODE_LTE;
			break;
		}
	}

	CALLBACK_WITH_SUCCESS(cb, available_rats, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void qmi_query_available_rats(struct ofono_radio_settings *rs,
			ofono_radio_settings_available_rats_query_cb_t cb,
			void *data)
{
	struct settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (!rsd->dms)
		goto error;

	if (qmi_service_send(rsd->dms, QMI_DMS_GET_CAPS, NULL,
					get_caps_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void create_dms_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_radio_settings *rs = user_data;
	struct settings_data *data = ofono_radio_settings_get_data(rs);

	DBG("");

	if (!service)
		return;

	data->dms = qmi_service_ref(service);
}

static void create_nas_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_radio_settings *rs = user_data;
	struct settings_data *data = ofono_radio_settings_get_data(rs);

	DBG("");

	if (!service) {
		ofono_error("Failed to request NAS service");
		ofono_radio_settings_remove(rs);
		return;
	}

	if (!qmi_service_get_version(service, &data->major, &data->minor)) {
		ofono_error("Failed to get NAS service version");
		ofono_radio_settings_remove(rs);
		return;
	}

	data->nas = qmi_service_ref(service);

	ofono_radio_settings_register(rs);
}

static int qmi_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct settings_data *data;

	DBG("");

	data = g_new0(struct settings_data, 1);

	ofono_radio_settings_set_data(rs, data);

	qmi_service_create_shared(device, QMI_SERVICE_DMS,
						create_dms_cb, rs, NULL);
	qmi_service_create_shared(device, QMI_SERVICE_NAS,
						create_nas_cb, rs, NULL);

	return 0;
}

static void qmi_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct settings_data *data = ofono_radio_settings_get_data(rs);

	DBG("");

	ofono_radio_settings_set_data(rs, NULL);

	qmi_service_unregister_all(data->nas);

	qmi_service_unref(data->nas);

	g_free(data);
}

static struct ofono_radio_settings_driver driver = {
	.name		= "qmimodem",
	.probe		= qmi_radio_settings_probe,
	.remove		= qmi_radio_settings_remove,
	.set_rat_mode	= qmi_set_rat_mode,
	.query_rat_mode = qmi_query_rat_mode,
	.query_available_rats = qmi_query_available_rats,
};

void qmi_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void qmi_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
