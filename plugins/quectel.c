/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014  Philip Paeps. All rights reserved.
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

#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/tty.h>
#include <linux/gsmmux.h>
#include <ell/ell.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *cfun_prefix[] = { "+CFUN:", NULL };
static const char *cpin_prefix[] = { "+CPIN:", NULL };
static const char *none_prefix[] = { NULL };

static const uint8_t gsm0710_terminate[] = {
	0xf9, /* open flag */
	0x03, /* channel 0 */
	0xef, /* UIH frame */
	0x05, /* 2 data bytes */
	0xc3, /* terminate 1 */
	0x01, /* terminate 2 */
	0xf2, /* crc */
	0xf9, /* close flag */
};

struct quectel_data {
	GAtChat *modem;
	GAtChat *aux;
	guint cpin_ready;
	bool have_sim;

	/* used by quectel uart driver */
	GAtChat *uart;
	int mux_ready_count;
	int initial_ldisc;
};

static void quectel_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int quectel_probe(struct ofono_modem *modem)
{
	struct quectel_data *data;

	DBG("%p", modem);

	data = l_new(struct quectel_data, 1);

	ofono_modem_set_data(modem, data);

	return 0;
}

static void quectel_remove(struct ofono_modem *modem)
{
	struct quectel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->cpin_ready != 0)
		g_at_chat_unregister(data->aux, data->cpin_ready);

	ofono_modem_set_data(modem, NULL);
	g_at_chat_unref(data->aux);
	g_at_chat_unref(data->modem);
	g_at_chat_unref(data->uart);
	l_free(data);
}

static void close_mux_cb(struct l_timeout *timeout, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct quectel_data *data = ofono_modem_get_data(modem);
	GIOChannel *device;
	ssize_t write_count;
	int fd;

	DBG("%p", modem);

	device = g_at_chat_get_channel(data->uart);
	fd = g_io_channel_unix_get_fd(device);

	/* restore initial tty line discipline */
	if (ioctl(fd, TIOCSETD, &data->initial_ldisc) < 0)
		ofono_warn("Failed to restore line discipline");

	/* terminate gsm 0710 multiplexing on the modem side */
	write_count = write(fd, gsm0710_terminate, sizeof(gsm0710_terminate));
	if (write_count != sizeof(gsm0710_terminate))
		ofono_warn("Failed to terminate gsm multiplexing");

	g_at_chat_unref(data->uart);
	data->uart = NULL;

	l_timeout_remove(timeout);
	ofono_modem_set_powered(modem, FALSE);
}

static void close_serial(struct ofono_modem *modem)
{
	struct quectel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_unref(data->aux);
	data->aux = NULL;

	g_at_chat_unref(data->modem);
	data->modem = NULL;

	/*
	 * if gsm0710 multiplexing is used, the aux and modem file descriptors
	 * must be closed before closing the underlying serial device to avoid
	 * an old kernel dead-lock:
	 * https://lists.ofono.org/pipermail/ofono/2011-March/009405.html
	 *
	 * setup a timer to iterate the mainloop once to let gatchat close the
	 * virtual file descriptors unreferenced above
	 */
	if (data->uart)
		l_timeout_create_ms(1, close_mux_cb, modem, NULL);
	else
		ofono_modem_set_powered(modem, false);
}

static void cpin_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct quectel_data *data = ofono_modem_get_data(modem);
	const char *sim_inserted;
	GAtResultIter iter;

	DBG("%p", modem);

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CPIN:"))
		return;

	g_at_result_iter_next_unquoted_string(&iter, &sim_inserted);

	if (g_strcmp0(sim_inserted, "NOT INSERTED") != 0)
		data->have_sim = true;

	ofono_modem_set_powered(modem, TRUE);

	/* Turn off the radio. */
	g_at_chat_send(data->aux, "AT+CFUN=4", none_prefix, NULL, NULL, NULL);

	g_at_chat_unregister(data->aux, data->cpin_ready);
	data->cpin_ready = 0;
}

static void cpin_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	DBG("%p ok %d", user_data, ok);

	if (ok)
		cpin_notify(result, user_data);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct quectel_data *data = ofono_modem_get_data(modem);

	DBG("%p ok %d", modem, ok);

	if (!ok) {
		close_serial(modem);
		return;
	}

	data->cpin_ready = g_at_chat_register(data->aux, "+CPIN", cpin_notify,
						FALSE, modem, NULL);
	g_at_chat_send(data->aux, "AT+CPIN?", cpin_prefix, cpin_query, modem,
			NULL);
}

static void cfun_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct quectel_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int cfun;

	DBG("%p ok %d", modem, ok);

	if (!ok) {
		close_serial(modem);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+CFUN:") == FALSE) {
		close_serial(modem);
		return;
	}

	g_at_result_iter_next_number(&iter, &cfun);

	/*
	 * The modem firmware powers up in CFUN=1 but will respond to AT+CFUN=4
	 * with ERROR until some amount of time (which varies with temperature)
	 * passes.  Empirical evidence suggests that the firmware will report an
	 * unsolicited +CPIN: notification when it is ready to be useful.
	 *
	 * Work around this feature by only transitioning to CFUN=4 if the
	 * modem is not in CFUN=1 or until after we've received an unsolicited
	 * +CPIN: notification.
	 */
	if (cfun != 1)
		g_at_chat_send(data->aux, "AT+CFUN=4", none_prefix, cfun_enable,
				modem, NULL);
	else
		cfun_enable(TRUE, NULL, modem);
}

static int open_ttys(struct ofono_modem *modem)
{
	struct quectel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->modem = at_util_open_device(modem, "Modem", quectel_debug,
						"Modem: ", NULL);
	if (data->modem == NULL)
		return -EINVAL;

	data->aux = at_util_open_device(modem, "Aux", quectel_debug, "Aux: ",
					NULL);
	if (data->aux == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	g_at_chat_set_slave(data->modem, data->aux);

	g_at_chat_send(data->modem, "ATE0; &C0; +CMEE=1", none_prefix, NULL,
			NULL, NULL);
	g_at_chat_send(data->aux, "ATE0; &C0; +CMEE=1", none_prefix, NULL, NULL,
			NULL);
	g_at_chat_send(data->aux, "AT+CFUN?", cfun_prefix, cfun_query, modem,
			NULL);

	return -EINPROGRESS;
}

static void mux_ready_cb(struct l_timeout *timeout, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct quectel_data *data = ofono_modem_get_data(modem);
	struct stat st;
	int ret;

	DBG("%p", modem);

	/* check if the last (and thus all) virtual gsm tty's are created */
	ret = stat(ofono_modem_get_string(modem, "Modem"), &st);
	if (ret < 0) {
		if (data->mux_ready_count++ < 5) {
			/* not ready yet; try again in 100 ms*/
			l_timeout_modify_ms(timeout, 100);
			return;
		}

		/* not ready after 500 ms; bail out */
		close_serial(modem);
		return;
	}

	/* virtual gsm tty's are ready */
	l_timeout_remove(timeout);

	if (open_ttys(modem) != -EINPROGRESS)
		close_serial(modem);

	g_at_chat_set_slave(data->uart, data->modem);
}

static void cmux_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct quectel_data *data = ofono_modem_get_data(modem);
	struct gsm_config gsm_config;
	GIOChannel *device;
	int ldisc = N_GSM0710;
	int fd;

	DBG("%p", modem);

	device = g_at_chat_get_channel(data->uart);
	fd = g_io_channel_unix_get_fd(device);

	/* get initial line discipline to restore after use */
	if (ioctl(fd, TIOCGETD, &data->initial_ldisc) < 0) {
		ofono_error("Failed to get current line discipline: %s",
				strerror(errno));
		close_serial(modem);
		return;
	}

	/* enable gsm 0710 multiplexing line discipline */
	if (ioctl(fd, TIOCSETD, &ldisc) < 0) {
		ofono_error("Failed to set multiplexer line discipline: %s",
				strerror(errno));
		close_serial(modem);
		return;
	}

	/* get n_gsm configuration */
	if (ioctl(fd, GSMIOC_GETCONF, &gsm_config) < 0) {
		ofono_error("Failed to get gsm config: %s", strerror(errno));
		close_serial(modem);
		return;
	}

	gsm_config.initiator = 1;     /* cpu side is initiating multiplexing */
	gsm_config.encapsulation = 0; /* basic transparency encoding */
	gsm_config.mru = 127;         /* 127 bytes rx mtu */
	gsm_config.mtu = 127;         /* 127 bytes tx mtu */
	gsm_config.t1 = 10;           /* 100 ms ack timer */
	gsm_config.n2 = 3;            /* 3 retries */
	gsm_config.t2 = 30;           /* 300 ms response timer */
	gsm_config.t3 = 10;           /* 100 ms wake up response timer */
	gsm_config.i = 1;             /* subset */

	/* set the new configuration */
	if (ioctl(fd, GSMIOC_SETCONF, &gsm_config) < 0) {
		ofono_error("Failed to set gsm config: %s", strerror(errno));
		close_serial(modem);
		return;
	}

	/*
	 * the kernel does not yet support mapping the underlying serial device
	 * to its virtual gsm ttys, so hard-code gsmtty1 gsmtty2 for now
	 */
	ofono_modem_set_string(modem, "Aux", "/dev/gsmtty1");
	ofono_modem_set_string(modem, "Modem", "/dev/gsmtty2");

	/* wait for gsmtty devices to appear */
	if (!l_timeout_create_ms(100, mux_ready_cb, modem, NULL)) {
		close_serial(modem);
		return;
	}
}

static int open_serial(struct ofono_modem *modem)
{
	struct quectel_data *data = ofono_modem_get_data(modem);
	const char *rts_cts;

	DBG("%p", modem);

	rts_cts = ofono_modem_get_string(modem, "RtsCts");

	data->uart = at_util_open_device(modem, "Device", quectel_debug,
						"UART: ",
						"Baud", "115200",
						"Parity", "none",
						"StopBits", "1",
						"DataBits", "8",
						"XonXoff", "off",
						"Local", "on",
						"Read", "on",
						"RtsCts", rts_cts,
						NULL);
	if (data->uart == NULL)
		return -EINVAL;

	g_at_chat_send(data->uart, "ATE0", none_prefix, NULL, NULL,
			NULL);

	/* setup multiplexing */
	g_at_chat_send(data->uart, "AT+CMUX=0,0,5,127,10,3,30,10,2", NULL,
			cmux_cb, modem, NULL);

	return -EINPROGRESS;
}

static int quectel_enable(struct ofono_modem *modem)
{
	DBG("%p", modem);

	if (ofono_modem_get_string(modem, "Device"))
		return open_serial(modem);
	else
		return open_ttys(modem);
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("%p", modem);

	close_serial(modem);
}

static int quectel_disable(struct ofono_modem *modem)
{
	struct quectel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);

	g_at_chat_cancel_all(data->aux);
	g_at_chat_unregister_all(data->aux);

	g_at_chat_send(data->aux, "AT+CFUN=0", cfun_prefix, cfun_disable, modem,
			NULL);

	return -EINPROGRESS;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("%p", user_data);

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void quectel_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct quectel_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(data->aux, command, cfun_prefix, set_online_cb, cbd,
				g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void quectel_pre_sim(struct ofono_modem *modem)
{
	struct quectel_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->aux);
	sim = ofono_sim_create(modem, OFONO_VENDOR_QUECTEL, "atmodem",
				data->aux);

	if (sim && data->have_sim == true)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void quectel_post_sim(struct ofono_modem *modem)
{
	struct quectel_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->aux);
	gc = ofono_gprs_context_create(modem, 0, "atmodem", data->modem);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static void quectel_post_online(struct ofono_modem *modem)
{
	struct quectel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, 0, "atmodem", data->aux);
}

static struct ofono_modem_driver quectel_driver = {
	.name			= "quectel",
	.probe			= quectel_probe,
	.remove			= quectel_remove,
	.enable			= quectel_enable,
	.disable		= quectel_disable,
	.set_online		= quectel_set_online,
	.pre_sim		= quectel_pre_sim,
	.post_sim		= quectel_post_sim,
	.post_online		= quectel_post_online,
};

static int quectel_init(void)
{
	return ofono_modem_driver_register(&quectel_driver);
}

static void quectel_exit(void)
{
	ofono_modem_driver_unregister(&quectel_driver);
}

OFONO_PLUGIN_DEFINE(quectel, "Quectel driver", VERSION,
    OFONO_PLUGIN_PRIORITY_DEFAULT, quectel_init, quectel_exit)
