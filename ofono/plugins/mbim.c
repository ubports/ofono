/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
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
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <unistd.h>
#include <stdlib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>
#include <ofono/devinfo.h>
#include <ofono/sim.h>
#include <ofono/netreg.h>
#include <ofono/sms.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>

#include <ell/ell.h>

#include <drivers/mbimmodem/mbim.h>
#include <drivers/mbimmodem/mbim-message.h>
#include <drivers/mbimmodem/mbim-desc.h>
#include <drivers/mbimmodem/util.h>

struct mbim_data {
	struct mbim_device *device;
	uint16_t max_segment;
	uint8_t max_outstanding;
	uint8_t max_sessions;
};

static void mbim_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int mbim_parse_descriptors(struct mbim_data *md, const char *file)
{
	void *data;
	size_t len;
	const struct mbim_desc *desc = NULL;
	const struct mbim_extended_desc *ext_desc = NULL;

	data = l_file_get_contents(file, &len);
	if (!data)
		return -EIO;

	if (!mbim_find_descriptors(data, len, &desc, &ext_desc)) {
		l_free(data);
		return -ENOENT;
	}

	if (desc)
		md->max_segment = L_LE16_TO_CPU(desc->wMaxControlMessage);

	if (ext_desc)
		md->max_outstanding = ext_desc->bMaxOutstandingCommandMessages;

	l_free(data);
	return 0;
}

static int mbim_probe(struct ofono_modem *modem)
{
	const char *descriptors;
	struct mbim_data *data;
	int err;

	DBG("%p", modem);

	descriptors = ofono_modem_get_string(modem, "DescriptorFile");

	if (!descriptors)
		return -EINVAL;

	data = l_new(struct mbim_data, 1);
	data->max_outstanding = 1;

	err = mbim_parse_descriptors(data, descriptors);
	if (err < 0) {
		DBG("Warning, unable to load descriptors, setting defaults");
		data->max_segment = 512;
	}

	DBG("MaxSegment: %d, MaxOutstanding: %d",
			data->max_segment, data->max_outstanding);

	ofono_modem_set_data(modem, data);

	return 0;
}

static void mbim_remove(struct ofono_modem *modem)
{
	struct mbim_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	mbim_device_unref(data->device);

	ofono_modem_set_data(modem, NULL);
	l_free(data);
}

static void mbim_radio_state_init_cb(struct mbim_message *message, void *user)
{
	struct ofono_modem *modem = user;
	struct mbim_data *md = ofono_modem_get_data(modem);
	uint32_t hw_state;
	uint32_t sw_state;
	bool r;

	if (mbim_message_get_error(message) != 0)
		goto error;

	r = mbim_message_get_arguments(message, "uu",
					&hw_state, &sw_state);
	if (!r)
		goto error;

	/* TODO: How to handle HwRadioState != 1 */
	DBG("HwRadioState: %u, SwRadioState: %u", hw_state, sw_state);
	ofono_modem_set_powered(modem, TRUE);
	return;

error:
	mbim_device_shutdown(md->device);
}

static void mbim_device_subscribe_list_set_cb(struct mbim_message *message,
						void *user)
{
	struct ofono_modem *modem = user;
	struct mbim_data *md = ofono_modem_get_data(modem);

	if (mbim_message_get_error(message) != 0)
		goto error;

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_RADIO_STATE,
					MBIM_COMMAND_TYPE_SET);

	mbim_message_set_arguments(message, "u", 0);
	if (mbim_device_send(md->device, 0, message,
				mbim_radio_state_init_cb, modem, NULL))
		return;

error:
	mbim_device_shutdown(md->device);
}

static void mbim_device_caps_info_cb(struct mbim_message *message, void *user)
{
	struct ofono_modem *modem = user;
	struct mbim_data *md = ofono_modem_get_data(modem);
	uint32_t device_type;
	uint32_t cellular_class;
	uint32_t voice_class;
	uint32_t sim_class;
	uint32_t data_class;
	uint32_t sms_caps;
	uint32_t control_caps;
	uint32_t max_sessions;
	char *custom_data_class;
	char *device_id;
	char *firmware_info;
	char *hardware_info;
	bool r;

	if (mbim_message_get_error(message) != 0)
		goto error;

	r = mbim_message_get_arguments(message, "uuuuuuuussss",
					&device_type, &cellular_class,
					&voice_class, &sim_class, &data_class,
					&sms_caps, &control_caps, &max_sessions,
					&custom_data_class, &device_id,
					&firmware_info, &hardware_info);
	if (!r)
		goto error;

	md->max_sessions = max_sessions;

	DBG("DeviceId: %s", device_id);
	DBG("FirmwareInfo: %s", firmware_info);
	DBG("HardwareInfo: %s", hardware_info);

	ofono_modem_set_string(modem, "DeviceId", device_id);
	ofono_modem_set_string(modem, "FirmwareInfo", firmware_info);

	l_free(custom_data_class);
	l_free(device_id);
	l_free(firmware_info);
	l_free(hardware_info);

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_DEVICE_SERVICE_SUBSCRIBE_LIST,
					MBIM_COMMAND_TYPE_SET);

	mbim_message_set_arguments(message, "av", 2,
					"16yuuuuuuu",
					mbim_uuid_basic_connect, 6,
					MBIM_CID_SUBSCRIBER_READY_STATUS,
					MBIM_CID_RADIO_STATE,
					MBIM_CID_REGISTER_STATE,
					MBIM_CID_PACKET_SERVICE,
					MBIM_CID_SIGNAL_STATE,
					MBIM_CID_CONNECT,
					"16yuuuu", mbim_uuid_sms, 3,
					MBIM_CID_SMS_CONFIGURATION,
					MBIM_CID_SMS_READ,
					MBIM_CID_SMS_MESSAGE_STORE_STATUS);

	if (mbim_device_send(md->device, 0, message,
				mbim_device_subscribe_list_set_cb,
				modem, NULL))
		return;

error:
	mbim_device_shutdown(md->device);
}

static void mbim_device_closed(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbim_data *md = ofono_modem_get_data(modem);

	mbim_device_unref(md->device);
	md->device = NULL;

	ofono_modem_set_powered(modem, FALSE);
}

static void mbim_device_ready(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct mbim_data *md = ofono_modem_get_data(modem);
	struct mbim_message *message =
		mbim_message_new(mbim_uuid_basic_connect,
					1, MBIM_COMMAND_TYPE_QUERY);

	mbim_message_set_arguments(message, "");
	mbim_device_send(md->device, 0, message,
				mbim_device_caps_info_cb, modem, NULL);
}

static int mbim_enable(struct ofono_modem *modem)
{
	const char *device;
	int fd;
	struct mbim_data *md = ofono_modem_get_data(modem);

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (!device)
		return -EINVAL;

	DBG("%p", device);
	fd = open(device, O_EXCL | O_NONBLOCK | O_RDWR);
	if (fd < 0)
		return -EIO;

	md->device = mbim_device_new(fd, md->max_segment);

	mbim_device_set_close_on_unref(md->device, true);
	mbim_device_set_max_outstanding(md->device, md->max_outstanding);
	mbim_device_set_ready_handler(md->device,
					mbim_device_ready, modem, NULL);
	mbim_device_set_disconnect_handler(md->device,
					mbim_device_closed, modem, NULL);
	mbim_device_set_debug(md->device, mbim_debug, "MBIM:", NULL);

	return -EINPROGRESS;
}

static void mbim_radio_off_for_disable(struct mbim_message *message, void *user)
{
	struct ofono_modem *modem = user;
	struct mbim_data *md = ofono_modem_get_data(modem);

	DBG("%p", modem);

	mbim_device_shutdown(md->device);
}

static int mbim_disable(struct ofono_modem *modem)
{
	struct mbim_data *md = ofono_modem_get_data(modem);
	struct mbim_message *message;

	DBG("%p", modem);

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_RADIO_STATE,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(message, "u", 0);

	if (mbim_device_send(md->device, 0, message,
				mbim_radio_off_for_disable, modem, NULL) > 0)
		return -EINPROGRESS;

	mbim_device_closed(modem);
	return 0;
}

static void mbim_set_online_cb(struct mbim_message *message, void *user)
{
	struct cb_data *cbd = user;
	ofono_modem_online_cb_t cb = cbd->cb;

	if (mbim_message_get_error(message) != 0)
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	else
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void mbim_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct mbim_data *md = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct mbim_message *message;

	DBG("%p %s", modem, online ? "online" : "offline");

	message = mbim_message_new(mbim_uuid_basic_connect,
					MBIM_CID_RADIO_STATE,
					MBIM_COMMAND_TYPE_SET);
	mbim_message_set_arguments(message, "u", online ? 1 : 0);

	if (mbim_device_send(md->device, 0, message,
				mbim_set_online_cb, cbd, l_free) > 0)
		return;

	l_free(cbd);
	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void mbim_pre_sim(struct ofono_modem *modem)
{
	struct mbim_data *md = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "mbim", NULL);
	ofono_sim_create(modem, 0, "mbim", md->device);
}

static void mbim_post_sim(struct ofono_modem *modem)
{
	struct mbim_data *md = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_sms_create(modem, 0, "mbim", md->device);
	gprs = ofono_gprs_create(modem, 0, "mbim", md->device);

	ofono_gprs_set_cid_range(gprs, 0, md->max_sessions);

	gc = ofono_gprs_context_create(modem, 0, "mbim", md->device);
	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		ofono_gprs_add_context(gprs, gc);
	}
}

static void mbim_post_online(struct ofono_modem *modem)
{
	struct mbim_data *md = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, 0, "mbim", md->device);
}

static struct ofono_modem_driver mbim_driver = {
	.name		= "mbim",
	.probe		= mbim_probe,
	.remove		= mbim_remove,
	.enable		= mbim_enable,
	.disable	= mbim_disable,
	.set_online	= mbim_set_online,
	.pre_sim	= mbim_pre_sim,
	.post_sim	= mbim_post_sim,
	.post_online	= mbim_post_online,
};

static int mbim_init(void)
{
	return ofono_modem_driver_register(&mbim_driver);
}

static void mbim_exit(void)
{
	ofono_modem_driver_unregister(&mbim_driver);
}

OFONO_PLUGIN_DEFINE(mbim, "MBIM modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, mbim_init, mbim_exit)
