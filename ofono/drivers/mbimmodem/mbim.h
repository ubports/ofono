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

struct mbim_device;

typedef void (*mbim_device_debug_func_t) (const char *str, void *user_data);
typedef void (*mbim_device_disconnect_func_t) (void *user_data);
typedef void (*mbim_device_destroy_func_t) (void *user_data);
typedef void (*mbim_device_ready_func_t) (void *user_data);

extern const uint8_t mbim_uuid_basic_connect[];
extern const uint8_t mbim_uuid_sms[];
extern const uint8_t mbim_uuid_ussd[];
extern const uint8_t mbim_uuid_phonebook[];
extern const uint8_t mbim_uuid_stk[];
extern const uint8_t mbim_uuid_auth[];
extern const uint8_t mbim_uuid_dss[];

struct mbim_device *mbim_device_new(int fd, uint32_t max_segment_size);
bool mbim_device_set_close_on_unref(struct mbim_device *device, bool do_close);
struct mbim_device *mbim_device_ref(struct mbim_device *device);
void mbim_device_unref(struct mbim_device *device);
bool mbim_device_shutdown(struct mbim_device *device);

bool mbim_device_set_max_outstanding(struct mbim_device *device, uint32_t max);

bool mbim_device_set_debug(struct mbim_device *device,
				mbim_device_debug_func_t func, void *user_data,
				mbim_device_destroy_func_t destroy);
bool mbim_device_set_disconnect_handler(struct mbim_device *device,
					mbim_device_disconnect_func_t function,
					void *user_data,
					mbim_device_destroy_func_t destroy);
bool mbim_device_set_ready_handler(struct mbim_device *device,
					mbim_device_ready_func_t function,
					void *user_data,
					mbim_device_destroy_func_t destroy);
