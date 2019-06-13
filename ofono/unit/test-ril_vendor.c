/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2019 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "drivers/ril/ril_vendor.h"
#include "drivers/ril/ril_vendor_impl.h"
#include "drivers/ril/ril_network.h"
#include "drivers/ril/ril_data.h"
#include "drivers/ril/ril_log.h"

#include "ofono.h"

#include <grilio_request.h>
#include <grilio_parser.h>

GLOG_MODULE_DEFINE("ril");

/* Stubs */
typedef struct ril_network TestNetwork;
typedef GObjectClass TestNetworkClass;
static void test_network_init(TestNetwork *self) {}
static void test_network_class_init(TestNetworkClass *klass) {}
G_DEFINE_TYPE(TestNetwork, test_network, G_TYPE_OBJECT)

void ril_network_query_registration_state(struct ril_network *network)
{
}

const struct ofono_gprs_primary_context *ofono_gprs_context_settings_by_type
		(struct ofono_gprs *gprs, enum ofono_gprs_context_type type)
{
	return NULL;
}

/* Test vendor objects and drivers */

typedef RilVendor TestVendor;
typedef RilVendorClass TestVendorClass;
static void test_vendor_init(TestVendor *self) {}
static void test_vendor_class_init(TestVendorClass* klass) {}
static const struct ril_vendor_defaults test_defaults = { .enable_cbs = TRUE };
G_DEFINE_TYPE(TestVendor, test_vendor, RIL_VENDOR_TYPE)

static void test_driver_get_defaults(struct ril_vendor_defaults *defaults)
{
	memcpy(defaults, &test_defaults, sizeof(*defaults));
}

static RilVendor *test_driver_create_vendor(const void *driver_data,
					GRilIoChannel *io, const char *path,
					const struct ril_slot_config *config)
{
	TestVendor *self = g_object_new(test_vendor_get_type(), NULL);

	ril_vendor_init_base(self, io);
	return self;
}

RIL_VENDOR_DRIVER_DEFINE(test_driver) {
	.name           = "test",
	.get_defaults   = test_driver_get_defaults,
	.create_vendor  = test_driver_create_vendor
};

RIL_VENDOR_DRIVER_DEFINE(dummy_driver) { .name = "dummy" };

/* Tests */

static void test_null(void)
{
	ril_vendor_unref(NULL);
	ril_vendor_set_network(NULL, NULL);
	ril_vendor_data_call_parse(NULL, NULL, 0, NULL);
	ril_vendor_get_defaults(NULL, NULL);
	g_assert(!ril_vendor_find_driver(NULL));
	g_assert(!ril_vendor_create(NULL, NULL, NULL, NULL));
	g_assert(!ril_vendor_ref(NULL));
	g_assert(!ril_vendor_request_to_string(NULL, 0));
	g_assert(!ril_vendor_event_to_string(NULL, 0));
	g_assert(!ril_vendor_set_attach_apn_req(NULL, NULL, NULL, NULL,
						RIL_AUTH_NONE, NULL));
	g_assert(!ril_vendor_data_call_req(NULL, 0, RIL_DATA_PROFILE_DEFAULT,
				NULL, NULL, NULL, RIL_AUTH_NONE, NULL));
}

static void test_drivers(void)
{
	const struct ril_vendor_driver *driver;
	struct ril_vendor_defaults defaults;

	/* This one exists and has all the callbacks */
	driver = ril_vendor_find_driver(test_driver.name);
	g_assert(driver);
	memset(&defaults, 0, sizeof(defaults));
	ril_vendor_get_defaults(driver, &defaults);
	g_assert(!memcmp(&defaults, &test_defaults, sizeof(defaults)));

	/* This one has no callbacks at all */
	driver = ril_vendor_find_driver(dummy_driver.name);
	g_assert(driver);
	memset(&defaults, 0, sizeof(defaults));
	g_assert(!ril_vendor_create(driver, NULL, NULL, NULL));
	ril_vendor_get_defaults(driver, &defaults);

	/* And this one doesn't exist */
	g_assert(!ril_vendor_find_driver("no such driver"));
}

static void test_base(void)
{
	TestNetwork *network = g_object_new(test_network_get_type(), NULL);
	const struct ril_vendor_driver *driver;
	struct ril_vendor *base;

	/* Create test vendor which does nothing but extends the base */
	driver = ril_vendor_find_driver(test_driver.name);
	g_assert(driver);
	base = ril_vendor_create(driver, NULL, NULL, NULL);
	ril_vendor_set_network(base, NULL);
	ril_vendor_set_network(base, network);
	ril_vendor_set_network(base, NULL);
	ril_vendor_set_network(base, network);

	g_assert(!ril_vendor_request_to_string(base, 0));
	g_assert(!ril_vendor_event_to_string(base, 0));
	g_assert(!ril_vendor_set_attach_apn_req(base, NULL, NULL, NULL,
						RIL_AUTH_NONE, NULL));
	g_assert(!ril_vendor_data_call_req(base, 0, RIL_DATA_PROFILE_DEFAULT,
				NULL, NULL, NULL, RIL_AUTH_NONE, NULL));
	g_assert(!ril_vendor_data_call_parse(base, NULL, 0, NULL));

	g_assert(ril_vendor_ref(base) == base);
	ril_vendor_unref(base);
	ril_vendor_unref(base);
	g_object_unref(network);
}

static void test_mtk(void)
{
	TestNetwork *network = g_object_new(test_network_get_type(), NULL);
	const struct ril_vendor_driver *driver = ril_vendor_find_driver("mtk");
	struct ril_vendor_defaults defaults;
	struct ril_slot_config config;
	struct ril_vendor *mtk;

	g_assert(driver);
	memset(&defaults, 0, sizeof(defaults));
	memset(&config, 0, sizeof(config));
	ril_vendor_get_defaults(driver, &defaults);
	mtk = ril_vendor_create(driver, NULL, NULL, &config);
	g_assert(mtk);

	/* Freeing the network clears vendor's weak pointer */
	ril_vendor_set_network(mtk, network);
	g_object_unref(network);
	g_assert(!ril_vendor_request_to_string(mtk, 0));
	g_assert(!ril_vendor_event_to_string(mtk, 0));
	ril_vendor_unref(mtk);
}

static const char *MTK_RESUME_REGISTRATION="MTK_RESUME_REGISTRATION";
static const char *MTK_SET_CALL_INDICATION="MTK_SET_CALL_INDICATION";
static const char *MTK_PS_NETWORK_STATE_CHANGED="MTK_PS_NETWORK_STATE_CHANGED";
static const char *MTK_REGISTRATION_SUSPENDED="MTK_REGISTRATION_SUSPENDED";
static const char *MTK_SET_ATTACH_APN="MTK_SET_ATTACH_APN";
static const char *MTK_INCOMING_CALL_INDICATION="MTK_INCOMING_CALL_INDICATION";

static void test_mtk1(void)
{
	const struct ril_vendor_driver *driver = ril_vendor_find_driver("mtk1");
	struct ril_slot_config config;
	struct ril_vendor *mtk1;
	GRilIoRequest* req;

	g_assert(driver);
	memset(&config, 0, sizeof(config));
	mtk1 = ril_vendor_create(driver, NULL, NULL, &config);
	g_assert(mtk1);

	g_assert(!g_strcmp0(ril_vendor_request_to_string(mtk1, 2050),
					MTK_RESUME_REGISTRATION));
	g_assert(!g_strcmp0(ril_vendor_request_to_string(mtk1, 2065),
					MTK_SET_CALL_INDICATION));
	g_assert(!g_strcmp0(ril_vendor_event_to_string(mtk1, 3012),
					MTK_PS_NETWORK_STATE_CHANGED));
	g_assert(!g_strcmp0(ril_vendor_event_to_string(mtk1, 3021),
					MTK_REGISTRATION_SUSPENDED));
	g_assert(!g_strcmp0(ril_vendor_event_to_string(mtk1, 3065),
					MTK_SET_ATTACH_APN));
	g_assert(!g_strcmp0(ril_vendor_event_to_string(mtk1, 3037),
					MTK_INCOMING_CALL_INDICATION));

	/* mtk1 doesn't parse data calls */
	g_assert(!ril_vendor_data_call_parse(mtk1, NULL, 0, NULL));

	/* Build RIL_REQUEST_SET_INITIAL_ATTACH_APN */
	req = ril_vendor_set_attach_apn_req(mtk1, "apn", "username",
					"password", RIL_AUTH_NONE, "IP");
	grilio_request_unref(req);

	/* Build RIL_REQUEST_SETUP_DATA_CALL */
	req = ril_vendor_data_call_req(mtk1, 1, RIL_DATA_PROFILE_DEFAULT,
			"apn", "username", "password", RIL_AUTH_NONE, "IP");
	grilio_request_unref(req);

	ril_vendor_unref(mtk1);
}

static void test_mtk2(void)
{
	static const guint8 noprot[] = {
		0x00, 0x00, 0x00, 0x00, /* status */
		0x00, 0x00, 0x00, 0x00, /* retry_time */
		0x00, 0x00, 0x00, 0x00, /* cid */
		0x02, 0x00, 0x00, 0x00, /* active */
		0x00, 0x05, 0x00, 0x00  /* mtu */
	};
	static const guint8 noifname[] = {
		0x00, 0x00, 0x00, 0x00, /* status */
		0x00, 0x00, 0x00, 0x00, /* retry_time */
		0x00, 0x00, 0x00, 0x00, /* cid */
		0x02, 0x00, 0x00, 0x00, /* active */
		0x00, 0x05, 0x00, 0x00, /* mtu */
		/* "IP" */
		0x02, 0x00, 0x00, 0x00, 0x49, 0x00, 0x50, 0x00,
		0x00, 0x00, 0x00, 0x00
	};
	static const guint8 noaddr[] = {
		0x00, 0x00, 0x00, 0x00, /* status */
		0x00, 0x00, 0x00, 0x00, /* retry_time */
		0x00, 0x00, 0x00, 0x00, /* cid */
		0x02, 0x00, 0x00, 0x00, /* active */
		0x00, 0x05, 0x00, 0x00, /* mtu */
		/* "IP" */
		0x02, 0x00, 0x00, 0x00, 0x49, 0x00, 0x50, 0x00,
		0x00, 0x00, 0x00, 0x00,
		/* "ccmni0" */
		0x06, 0x00, 0x00, 0x00, 0x63, 0x00, 0x63, 0x00,
		0x6d, 0x00, 0x6e, 0x00, 0x69, 0x00, 0x30, 0x00,
		0x00, 0x00, 0x00, 0x00
	};
	static const guint8 datacall[] = {
		0x00, 0x00, 0x00, 0x00, /* status */
		0x00, 0x00, 0x00, 0x00, /* retry_time */
		0x00, 0x00, 0x00, 0x00, /* cid */
		0x02, 0x00, 0x00, 0x00, /* active */
		0x00, 0x05, 0x00, 0x00, /* mtu */
		/* "IP" */
		0x02, 0x00, 0x00, 0x00, 0x49, 0x00, 0x50, 0x00,
		0x00, 0x00, 0x00, 0x00,
		/* "ccmni0" */
		0x06, 0x00, 0x00, 0x00, 0x63, 0x00, 0x63, 0x00,
		0x6d, 0x00, 0x6e, 0x00, 0x69, 0x00, 0x30, 0x00,
		0x00, 0x00, 0x00, 0x00,
		/* "10.236.123.155" */
		0x0e, 0x00, 0x00, 0x00, 0x31, 0x00, 0x30, 0x00,
		0x2e, 0x00, 0x32, 0x00, 0x33, 0x00, 0x36, 0x00,
		0x2e, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00,
		0x2e, 0x00, 0x31, 0x00, 0x35, 0x00, 0x35, 0x00,
		0x00, 0x00, 0x00, 0x00,
		/* "217.118.66.243 217.118.66.244" */
		0x1d, 0x00, 0x00, 0x00, 0x32, 0x00, 0x31, 0x00,
		0x37, 0x00, 0x2e, 0x00, 0x31, 0x00, 0x31, 0x00,
		0x38, 0x00, 0x2e, 0x00, 0x36, 0x00, 0x36, 0x00,
		0x2e, 0x00, 0x32, 0x00, 0x34, 0x00, 0x33, 0x00,
		0x20, 0x00, 0x32, 0x00, 0x31, 0x00, 0x37, 0x00,
		0x2e, 0x00, 0x31, 0x00, 0x31, 0x00, 0x38, 0x00,
		0x2e, 0x00, 0x36, 0x00, 0x36, 0x00, 0x2e, 0x00,
		0x32, 0x00, 0x34, 0x00, 0x34, 0x00, 0x00, 0x00, 
		/* "10.236.123.155" */
		0x0e, 0x00, 0x00, 0x00, 0x31, 0x00, 0x30, 0x00,
		0x2e, 0x00, 0x32, 0x00, 0x33, 0x00, 0x36, 0x00,
		0x2e, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00,
		0x2e, 0x00, 0x31, 0x00, 0x35, 0x00, 0x35, 0x00,
		0x00, 0x00, 0x00, 0x00,
		/* whatever... */
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00
	};

	const struct ril_vendor_driver *driver = ril_vendor_find_driver("mtk2");
	struct ril_slot_config config;
	struct ril_data_call call;
	struct ril_vendor *mtk2;
	GRilIoParser rilp;
	GRilIoRequest* req;

	g_assert(driver);
	memset(&config, 0, sizeof(config));
	mtk2 = ril_vendor_create(driver, NULL, NULL, &config);
	g_assert(mtk2);

	g_assert(!g_strcmp0(ril_vendor_request_to_string(mtk2, 2065),
					MTK_RESUME_REGISTRATION));
	g_assert(!g_strcmp0(ril_vendor_request_to_string(mtk2, 2086),
					MTK_SET_CALL_INDICATION));
	g_assert(!g_strcmp0(ril_vendor_event_to_string(mtk2, 3015),
					MTK_PS_NETWORK_STATE_CHANGED));
	g_assert(!g_strcmp0(ril_vendor_event_to_string(mtk2, 3024),
					MTK_REGISTRATION_SUSPENDED));
	g_assert(!g_strcmp0(ril_vendor_event_to_string(mtk2, 3073),
					MTK_SET_ATTACH_APN));
	g_assert(!g_strcmp0(ril_vendor_event_to_string(mtk2, 3042),
					MTK_INCOMING_CALL_INDICATION));

	/* Build RIL_REQUEST_SET_INITIAL_ATTACH_APN */
	req = ril_vendor_set_attach_apn_req(mtk2, "apn", "username",
					"password", RIL_AUTH_NONE, "IP");
	grilio_request_unref(req);

	/* Build RIL_REQUEST_SETUP_DATA_CALL */
	req = ril_vendor_data_call_req(mtk2, 1, RIL_DATA_PROFILE_DEFAULT,
			"apn", "username", "password", RIL_AUTH_NONE, "IP");
	grilio_request_unref(req);

	/* Parse data call (version < 11) */
	memset(&call, 0, sizeof(call));
	memset(&rilp, 0, sizeof(rilp));
	g_assert(!ril_vendor_data_call_parse(mtk2, &call, 11, &rilp));

	memset(&call, 0, sizeof(call));
	grilio_parser_init(&rilp, noprot, sizeof(noprot));
	g_assert(!ril_vendor_data_call_parse(mtk2, &call, 10, &rilp));

	memset(&call, 0, sizeof(call));
	grilio_parser_init(&rilp, noifname, sizeof(noifname));
	g_assert(!ril_vendor_data_call_parse(mtk2, &call, 10, &rilp));

	memset(&call, 0, sizeof(call));
	grilio_parser_init(&rilp, noaddr, sizeof(noaddr));
	g_assert(!ril_vendor_data_call_parse(mtk2, &call, 10, &rilp));
	g_free(call.ifname);

	grilio_parser_init(&rilp, datacall, sizeof(datacall));
	g_assert(ril_vendor_data_call_parse(mtk2, &call, 10, &rilp));
	g_assert(call.active == RIL_DATA_CALL_ACTIVE);
	g_assert(call.mtu == 1280);
	g_assert(call.prot == OFONO_GPRS_PROTO_IP);
	g_assert(!g_strcmp0(call.ifname, "ccmni0"));
	g_assert(!g_strcmp0(call.dnses[0], "217.118.66.243"));
	g_assert(!g_strcmp0(call.dnses[1], "217.118.66.244"));
	g_assert(!call.dnses[2]);
	g_assert(!g_strcmp0(call.gateways[0], "10.236.123.155"));
	g_assert(!call.gateways[1]);
	g_assert(!g_strcmp0(call.addresses[0], "10.236.123.155"));
	g_assert(!call.addresses[1]);
	g_free(call.ifname);
	g_strfreev(call.dnses);
	g_strfreev(call.gateways);
	g_strfreev(call.addresses);

	ril_vendor_unref(mtk2);
}

#define TEST_(name) "/ril_vendor/" name

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	__ofono_log_init("test-ril_vendor",
		g_test_verbose() ? "*" : NULL,
		FALSE, FALSE);

	g_test_add_func(TEST_("null"), test_null);
	g_test_add_func(TEST_("drivers"), test_drivers);
	g_test_add_func(TEST_("base"), test_base);
	g_test_add_func(TEST_("mtk"), test_mtk);
	g_test_add_func(TEST_("mtk1"), test_mtk1);
	g_test_add_func(TEST_("mtk2"), test_mtk2);

	return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
