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

#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <libudev.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>

enum modem_type {
	MODEM_TYPE_USB,
	MODEM_TYPE_SERIAL,
	MODEM_TYPE_PCIE,
};

struct modem_info {
	char *syspath;
	char *devname;
	char *driver;
	char *vendor;
	char *model;
	enum modem_type type;
	union {
		GSList *devices;
		struct serial_device_info* serial;
	};
	struct ofono_modem *modem;
	const char *sysattr;
};

struct device_info {
	char *devpath;
	char *devnode;
	char *interface;
	char *number;
	char *label;
	char *sysattr;
	char *subsystem;
};

struct serial_device_info {
	char *devpath;
	char *devnode;
	char *subsystem;
	struct udev_device* dev;
};

static gboolean setup_isi(struct modem_info *modem)
{
	const char *node = NULL;
	int addr = 0;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->sysattr);

		if (g_strcmp0(info->sysattr, "820") == 0) {
			if (g_strcmp0(info->interface, "2/254/0") == 0)
				addr = 16;

			node = info->devnode;
		}
	}

	if (node == NULL)
		return FALSE;

	DBG("interface=%s address=%d", node, addr);

	ofono_modem_set_string(modem->modem, "Interface", node);
	ofono_modem_set_integer(modem->modem, "Address", addr);

	return TRUE;
}

static gboolean setup_mbm(struct modem_info *modem)
{
	const char *mdm = NULL, *app = NULL, *network = NULL, *gps = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->sysattr);

		if (g_str_has_suffix(info->sysattr, "Modem") == TRUE ||
				g_str_has_suffix(info->sysattr,
							"Modem 2") == TRUE) {
			if (mdm == NULL)
				mdm = info->devnode;
			else
				app = info->devnode;
		} else if (g_str_has_suffix(info->sysattr,
						"GPS Port") == TRUE ||
				g_str_has_suffix(info->sysattr,
						"Module NMEA") == TRUE) {
			gps = info->devnode;
		} else if (g_str_has_suffix(info->sysattr,
						"Network Adapter") == TRUE ||
				g_str_has_suffix(info->sysattr,
						"gw") == TRUE ||
				g_str_has_suffix(info->sysattr,
						"NetworkAdapter") == TRUE) {
			network = info->devnode;
		}
	}

	if (mdm == NULL || app == NULL)
		return FALSE;

	DBG("modem=%s data=%s network=%s gps=%s", mdm, app, network, gps);

	ofono_modem_set_string(modem->modem, "ModemDevice", mdm);
	ofono_modem_set_string(modem->modem, "DataDevice", app);
	ofono_modem_set_string(modem->modem, "GPSDevice", gps);
	ofono_modem_set_string(modem->modem, "NetworkInterface", network);

	return TRUE;
}

static gboolean setup_hso(struct modem_info *modem)
{
	const char *ctl = NULL, *app = NULL, *mdm = NULL, *net = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->sysattr);

		if (g_strcmp0(info->sysattr, "Control") == 0)
			ctl = info->devnode;
		else if (g_strcmp0(info->sysattr, "Application") == 0)
			app = info->devnode;
		else if (g_strcmp0(info->sysattr, "Modem") == 0)
			mdm = info->devnode;
		else if (info->sysattr == NULL &&
				g_str_has_prefix(info->devnode, "hso") == TRUE)
			net = info->devnode;
	}

	if (ctl == NULL || app == NULL)
		return FALSE;

	DBG("control=%s application=%s modem=%s network=%s",
						ctl, app, mdm, net);

	ofono_modem_set_string(modem->modem, "Control", ctl);
	ofono_modem_set_string(modem->modem, "Application", app);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_gobi(struct modem_info *modem)
{
	const char *qmi = NULL, *mdm = NULL, *net = NULL;
	const char *gps = NULL, *diag = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s %s", info->devnode, info->interface,
						info->number, info->label,
						info->sysattr, info->subsystem);

		if (g_strcmp0(info->subsystem, "usbmisc") == 0) /* cdc-wdm */
			qmi = info->devnode;
		else if (g_strcmp0(info->subsystem, "net") == 0) /* wwan */
			net = info->devnode;
		else if (g_strcmp0(info->subsystem, "tty") == 0) {
			if (g_strcmp0(info->interface, "255/255/255") == 0) {
				if (g_strcmp0(info->number, "00") == 0)
					diag = info->devnode; /* ec20 */
				else if (g_strcmp0(info->number, "01") == 0)
					diag = info->devnode; /* gobi */
				else if (g_strcmp0(info->number, "02") == 0)
					mdm = info->devnode; /* gobi */
				else if (g_strcmp0(info->number, "03") == 0)
					gps = info->devnode; /* gobi */
			} else if (g_strcmp0(info->interface, "255/0/0") == 0) {
				if (g_strcmp0(info->number, "01") == 0)
					gps = info->devnode; /* ec20 */
				if (g_strcmp0(info->number, "02") == 0)
					mdm = info->devnode; /* ec20 */
				/* ignore the 3rd device second AT/mdm iface */
			}
		}
	}

	DBG("qmi=%s net=%s mdm=%s gps=%s diag=%s", qmi, net, mdm, gps, diag);

	if (qmi == NULL || mdm == NULL || net == NULL)
		return FALSE;


	ofono_modem_set_string(modem->modem, "Device", qmi);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Diag", diag);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_sierra(struct modem_info *modem)
{
	const char *mdm = NULL, *app = NULL, *net = NULL, *diag = NULL, *qmi = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->subsystem);

		if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "01") == 0)
				diag = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "04") == 0)
				app = info->devnode;
			else if (g_strcmp0(info->number, "07") == 0)
				net = info->devnode;
			else if (g_strcmp0(info->subsystem, "net") == 0) {
				/*
				 * When using the voice firmware on a mc7304
				 * the second cdc-wdm interface doesn't handle
				 * qmi messages properly.
				 * Some modems still have a working second
				 * cdc-wdm interface, some are not. But always
				 * the first interface works.
				 */
				if (g_strcmp0(info->number, "08") == 0) {
					net = info->devnode;
				} else if (g_strcmp0(info->number, "0a") == 0) {
					if (net == NULL)
						net = info->devnode;
				}
			} else if (g_strcmp0(info->subsystem, "usbmisc") == 0) {
				if (g_strcmp0(info->number, "08") == 0) {
					qmi = info->devnode;
				} else if (g_strcmp0(info->number, "0a") == 0) {
					if (qmi == NULL)
						qmi = info->devnode;
				}
			}
		}
	}

	if (qmi != NULL && net != NULL) {
		ofono_modem_set_driver(modem->modem, "gobi");
		goto done;
	}

	if (mdm == NULL || net == NULL)
		return FALSE;

done:
	DBG("modem=%s app=%s net=%s diag=%s qmi=%s", mdm, app, net, diag, qmi);

	ofono_modem_set_string(modem->modem, "Device", qmi);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "App", app);
	ofono_modem_set_string(modem->modem, "Diag", diag);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_huawei(struct modem_info *modem)
{
	const char *qmi = NULL, *mdm = NULL, *net = NULL;
	const char *pcui = NULL, *diag = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "modem") == 0 ||
				g_strcmp0(info->interface, "255/1/1") == 0 ||
				g_strcmp0(info->interface, "255/2/1") == 0 ||
				g_strcmp0(info->interface, "255/3/1") == 0 ||
				g_strcmp0(info->interface, "255/1/49") == 0) {
			mdm = info->devnode;
		} else if (g_strcmp0(info->label, "pcui") == 0 ||
				g_strcmp0(info->interface, "255/1/2") == 0 ||
				g_strcmp0(info->interface, "255/2/2") == 0 ||
				g_strcmp0(info->interface, "255/2/18") == 0 ||
				g_strcmp0(info->interface, "255/3/18") == 0 ||
				g_strcmp0(info->interface, "255/1/50") == 0) {
			pcui = info->devnode;
		} else if (g_strcmp0(info->label, "diag") == 0 ||
				g_strcmp0(info->interface, "255/1/3") == 0 ||
				g_strcmp0(info->interface, "255/2/3") == 0 ||
				g_strcmp0(info->interface, "255/1/51") == 0) {
			diag = info->devnode;
		} else if (g_strcmp0(info->interface, "255/1/8") == 0 ||
				g_strcmp0(info->interface, "255/1/56") == 0) {
			net = info->devnode;
		} else if (g_strcmp0(info->interface, "255/1/9") == 0 ||
				g_strcmp0(info->interface, "255/1/57") == 0) {
			qmi = info->devnode;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				pcui = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				pcui = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				pcui = info->devnode;
			else if (g_strcmp0(info->number, "04") == 0)
				pcui = info->devnode;
		}
	}

	if (qmi != NULL && net != NULL) {
		ofono_modem_set_driver(modem->modem, "gobi");
		goto done;
	}

	if (mdm == NULL || pcui == NULL)
		return FALSE;

done:
	DBG("mdm=%s pcui=%s diag=%s qmi=%s net=%s", mdm, pcui, diag, qmi, net);

	ofono_modem_set_string(modem->modem, "Device", qmi);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Pcui", pcui);
	ofono_modem_set_string(modem->modem, "Diag", diag);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_speedup(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_linktop(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->interface, "2/2/1") == 0) {
			if (g_strcmp0(info->number, "01") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_icera(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL, *net = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->interface, "2/2/1") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
		} else if (g_strcmp0(info->interface, "2/6/0") == 0) {
			if (g_strcmp0(info->number, "05") == 0)
				net = info->devnode;
			else if (g_strcmp0(info->number, "06") == 0)
				net = info->devnode;
			else if (g_strcmp0(info->number, "07") == 0)
				net = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s net=%s", aux, mdm, net);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_alcatel(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "03") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "05") == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_novatel(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_nokia(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "10/0/0") == 0) {
			if (g_strcmp0(info->number, "02") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "04") == 0)
				aux = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_telit(struct modem_info *modem)
{
	const char *mdm = NULL, *aux = NULL, *gps = NULL, *net = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				gps = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				aux = info->devnode;
		} else if (g_strcmp0(info->interface, "2/2/1") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "06") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "0a") == 0)
				gps = info->devnode;
		} else if (info->sysattr && (g_str_has_suffix(info->sysattr,
						"CDC NCM") == TRUE)) {
			net = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("modem=%s aux=%s gps=%s net=%s", mdm, aux, gps, net);

	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "GPS", gps);

	if (net != NULL)
		ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_telitqmi(struct modem_info *modem)
{
	const char *qmi = NULL, *net = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->subsystem);

		if (g_strcmp0(info->interface, "255/255/255") == 0 &&
				g_strcmp0(info->number, "02") == 0) {
			if (g_strcmp0(info->subsystem, "net") == 0)
				net = info->devnode;
			else if (g_strcmp0(info->subsystem, "usbmisc") == 0)
				qmi = info->devnode;
		}
	}

	if (qmi == NULL || net == NULL)
		return FALSE;

	DBG("qmi=%s net=%s", qmi, net);

	ofono_modem_set_string(modem->modem, "Device", qmi);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	ofono_modem_set_boolean(modem->modem, "ForceSimLegacy", TRUE);
	ofono_modem_set_boolean(modem->modem, "AlwaysOnline", TRUE);
	ofono_modem_set_driver(modem->modem, "gobi");

	return TRUE;
}

static gboolean setup_droid(struct modem_info *modem)
{
	const char *at = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->subsystem);

		if (g_strcmp0(info->interface, "255/255/255") == 0 &&
				g_strcmp0(info->number, "04") == 0) {
			at = info->devnode;
		}
	}

	if (at == NULL)
		return FALSE;

	ofono_modem_set_string(modem->modem, "Device", at);
	ofono_modem_set_driver(modem->modem, "droid");

	return TRUE;
}

/* TODO: Not used as we have no simcom driver */
static gboolean setup_simcom(struct modem_info *modem)
{
	const char *mdm = NULL, *aux = NULL, *gps = NULL, *diag = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				diag = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				gps = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("modem=%s aux=%s gps=%s diag=%s", mdm, aux, gps, diag);

	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Data", aux);
	ofono_modem_set_string(modem->modem, "GPS", gps);

	return TRUE;
}

static gboolean setup_zte(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL, *qcdm = NULL;
	const char *modem_intf;
	GSList *list;

	DBG("%s", modem->syspath);

	if (g_strcmp0(modem->model, "0016") == 0 ||
				g_strcmp0(modem->model, "0017") == 0 ||
				g_strcmp0(modem->model, "0117") == 0)
		modem_intf = "02";
	else
		modem_intf = "03";

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				qcdm = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, modem_intf) == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s qcdm=%s", aux, mdm, qcdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_samsung(struct modem_info *modem)
{
	const char *control = NULL, *network = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->interface, "10/0/0") == 0)
			control = info->devnode;
		else if (g_strcmp0(info->interface, "255/0/0") == 0)
			network = info->devnode;
	}

	if (control == NULL && network == NULL)
		return FALSE;

	DBG("control=%s network=%s", control, network);

	ofono_modem_set_string(modem->modem, "ControlPort", control);
	ofono_modem_set_string(modem->modem, "NetworkInterface", network);

	return TRUE;
}

static gboolean setup_quectel_usb(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "02") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_quectel_serial(struct modem_info *modem)
{
	struct serial_device_info *info = modem->serial;
	const char *value;

	value = udev_device_get_property_value(info->dev,
						"OFONO_QUECTEL_GPIO_CHIP");
	if (value)
		ofono_modem_set_string(modem->modem, "GpioChip", value);

	value = udev_device_get_property_value(info->dev,
						"OFONO_QUECTEL_GPIO_OFFSET");
	if (value)
		ofono_modem_set_string(modem->modem, "GpioOffset", value);

	value = udev_device_get_property_value(info->dev,
						"OFONO_QUECTEL_GPIO_LEVEL");
	if (value)
		ofono_modem_set_boolean(modem->modem, "GpioLevel", TRUE);

	value = udev_device_get_property_value(info->dev,
						"OFONO_QUECTEL_MUX");
	if (value)
		ofono_modem_set_string(modem->modem, "Mux", value);

	value = udev_device_get_property_value(info->dev,
						"OFONO_QUECTEL_RTSCTS");
	ofono_modem_set_string(modem->modem, "RtsCts", value ? value : "off");
	ofono_modem_set_string(modem->modem, "Device", info->devnode);

	return TRUE;
}

static gboolean setup_quectel(struct modem_info *modem)
{
	if (modem->type == MODEM_TYPE_SERIAL)
		return setup_quectel_serial(modem);
	else if (modem->type == MODEM_TYPE_USB)
		return setup_quectel_usb(modem);
	else
		return FALSE;
}

static gboolean setup_quectelqmi(struct modem_info *modem)
{
	const char *qmi = NULL, *net = NULL, *gps = NULL, *aux = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = g_slist_next(list)) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->subsystem);

		if (g_strcmp0(info->interface, "255/255/255") == 0 &&
				g_strcmp0(info->number, "04") == 0) {
			if (g_strcmp0(info->subsystem, "net") == 0)
				net = info->devnode;
			else if (g_strcmp0(info->subsystem, "usbmisc") == 0)
				qmi = info->devnode;
		} else if (g_strcmp0(info->interface, "255/0/0") == 0 &&
				g_strcmp0(info->number, "01") == 0) {
			gps = info->devnode;
		} else if (g_strcmp0(info->interface, "255/0/0") == 0 &&
				g_strcmp0(info->number, "02") == 0) {
			aux = info->devnode;
		}
	}

	DBG("qmi=%s net=%s", qmi, net);

	if (qmi == NULL || net == NULL)
		return FALSE;

	DBG("qmi=%s net=%s", qmi, net);

	ofono_modem_set_string(modem->modem, "Device", qmi);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	DBG("gps=%s aux=%s", gps, aux);

	if (gps)
		ofono_modem_set_string(modem->modem, "GPS", gps);
	if (aux)
		ofono_modem_set_string(modem->modem, "Aux", aux);

	ofono_modem_set_driver(modem->modem, "gobi");

	return TRUE;
}

static gboolean setup_mbim(struct modem_info *modem)
{
	const char *ctl = NULL, *net = NULL, *atcmd = NULL;
	GSList *list;
	char descriptors[PATH_MAX];

	DBG("%s [%s:%s]", modem->syspath, modem->vendor, modem->model);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s %s", info->devnode, info->interface,
						info->number, info->label,
						info->sysattr, info->subsystem);

		if (g_strcmp0(info->subsystem, "usbmisc") == 0) /* cdc-wdm */
			ctl = info->devnode;
		else if (g_strcmp0(info->subsystem, "net") == 0) /* wwan */
			net = info->devnode;
		else if (g_strcmp0(info->subsystem, "tty") == 0) {
			if (g_strcmp0(info->number, "02") == 0)
				atcmd = info->devnode;
		}
	}

	if (ctl == NULL || net == NULL)
		return FALSE;

	DBG("ctl=%s net=%s atcmd=%s", ctl, net, atcmd);

	sprintf(descriptors, "%s/descriptors", modem->syspath);

	ofono_modem_set_string(modem->modem, "Device", ctl);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);
	ofono_modem_set_string(modem->modem, "DescriptorFile", descriptors);

	return TRUE;
}

static gboolean setup_serial_modem(struct modem_info* modem)
{
	struct serial_device_info* info;

	info = modem->serial;

	ofono_modem_set_string(modem->modem, "Device", info->devnode);

	return TRUE;
}

static gboolean setup_tc65(struct modem_info* modem)
{
	ofono_modem_set_driver(modem->modem, "cinterion");

	return setup_serial_modem(modem);
}

static gboolean setup_ehs6(struct modem_info* modem)
{
	ofono_modem_set_driver(modem->modem, "cinterion");

	return setup_serial_modem(modem);
}

static gboolean setup_ifx(struct modem_info* modem)
{
	struct serial_device_info* info;
	const char *value;

	info = modem->serial;

	value = udev_device_get_property_value(info->dev, "OFONO_IFX_LDISC");
	if (value)
		ofono_modem_set_string(modem->modem, "LineDiscipline", value);

	value = udev_device_get_property_value(info->dev, "OFONO_IFX_AUDIO");
	if (value)
		ofono_modem_set_string(modem->modem, "AudioSetting", value);

	value = udev_device_get_property_value(info->dev, "OFONO_IFX_LOOPBACK");
	if (value)
		ofono_modem_set_string(modem->modem, "AudioLoopback", value);

	ofono_modem_set_string(modem->modem, "Device", info->devnode);

	return TRUE;
}

static gboolean setup_wavecom(struct modem_info* modem)
{
	struct serial_device_info* info;
	const char *value;

	info = modem->serial;

	value = udev_device_get_property_value(info->dev,
						"OFONO_WAVECOM_MODEL");
	if (value)
		ofono_modem_set_string(modem->modem, "Model", value);

	ofono_modem_set_string(modem->modem, "Device", info->devnode);

	return TRUE;
}

static gboolean setup_isi_serial(struct modem_info* modem)
{
	struct serial_device_info* info;
	const char *value;

	info = modem->serial;

	if (g_strcmp0(udev_device_get_subsystem(info->dev), "net") != 0)
		return FALSE;

	value = udev_device_get_sysattr_value(info->dev, "type");
	if (g_strcmp0(value, "820") != 0)
		return FALSE;

	/* OK, we want this device to be a modem */
	value = udev_device_get_sysname(info->dev);
	if (value)
		ofono_modem_set_string(modem->modem, "Interface", value);

	value = udev_device_get_property_value(info->dev, "OFONO_ISI_ADDRESS");
	if (value)
		ofono_modem_set_integer(modem->modem, "Address", atoi(value));

	return TRUE;
}

static gboolean setup_ublox(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL, *net = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
					info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		/*
		 * "2/2/1"
		 *  - a common modem interface both for older models like LISA,
		 *    and for newer models like TOBY.
		 * For TOBY-L2, NetworkInterface can be detected for each
		 * profile:
		 *  - low-medium throughput profile : 2/6/0
		 *  - fairly backward-compatible profile : 10/0/0
		 *  - high throughput profile : 224/1/3
		 */
		} else if (g_strcmp0(info->interface, "2/2/1") == 0) {
			if (!g_strcmp0(modem->model, "1010")) {
				if (g_strcmp0(info->number, "06") == 0)
					aux = info->devnode;
			} else {
				if (g_strcmp0(info->number, "02") == 0)
					aux = info->devnode;
			}
			if (g_strcmp0(info->number, "00") == 0)
				mdm = info->devnode;
		} else if (g_strcmp0(info->interface, "2/6/0") == 0 ||
				g_strcmp0(info->interface, "2/13/0") == 0 ||
				g_strcmp0(info->interface, "10/0/0") == 0 ||
				g_strcmp0(info->interface, "224/1/3") == 0) {
			net = info->devnode;
		}
	}

	/* Abort only if both interfaces are NULL, as it's highly possible that
	 * only one of 2 interfaces is available for U-blox modem.
	 */
	if (aux == NULL && mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s net=%s", aux, mdm, net);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_gemalto(struct modem_info* modem)
{
	const char *app = NULL, *gps = NULL, *mdm = NULL,
		*net = NULL, *qmi = NULL, *net2 = NULL;

	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->subsystem);

		/* PHS8-P */
		if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "01") == 0)
				gps = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				app = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->subsystem, "net") == 0)
				net = info->devnode;
			else if (g_strcmp0(info->subsystem, "usbmisc") == 0)
				qmi = info->devnode;
		}

		/* Cinterion ALS3, PLS8-E, PLS8-X */
		if (g_strcmp0(info->interface, "2/2/1") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				app = info->devnode;
			else if (g_strcmp0(info->number, "04") == 0)
				gps = info->devnode;
		}

		if (g_strcmp0(info->interface, "2/6/0") == 0) {
			if (g_strcmp0(info->subsystem, "net") == 0) {
				if (g_strcmp0(info->number, "0a") == 0)
					net = info->devnode;
				if (g_strcmp0(info->number, "0c") == 0)
					net2 = info->devnode;
			}
		}
	}

	DBG("application=%s gps=%s modem=%s network=%s qmi=%s",
			app, gps, mdm, net, qmi);

	if (app == NULL || mdm == NULL)
		return FALSE;

	ofono_modem_set_string(modem->modem, "Application", app);
	ofono_modem_set_string(modem->modem, "GPS", gps);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Device", qmi);
	ofono_modem_set_string(modem->modem, "Model", modem->model);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	if (net2)
		ofono_modem_set_string(modem->modem, "NetworkInterface2", net2);

	return TRUE;
}

static gboolean setup_xmm7xxx(struct modem_info *modem)
{
	const char *mdm = NULL, *net = NULL, *net2 = NULL, *net3 = NULL;
	GSList *list;

	DBG("%s %s\n", __DATE__, __TIME__);
	DBG("%s %s %s %s %s %s\n", modem->syspath, modem->devname,
		modem->driver, modem->vendor, modem->model, modem->sysattr);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s %s %s\n", info->devpath, info->devnode,
				info->interface, info->number, info->label,
				info->sysattr, info->subsystem);

		if (g_strcmp0(info->subsystem, "pci") == 0) {
			if ((g_strcmp0(modem->vendor, "0x8086") == 0) &&
				(g_strcmp0(modem->model, "0x7560") == 0)) {
				mdm = "/dev/iat";
				net = "inm0";
				net2 = "inm1";
				net3 = "inm2";
				ofono_modem_set_string(modem->modem,
					"CtrlPath", "/PCIE/IOSM/CTRL/1");
				ofono_modem_set_string(modem->modem, "DataPath",
					"/PCIE/IOSM/IPS/");
			}
		} else { /* For USB */
			if (g_strcmp0(modem->model, "095a") == 0) {
				if (g_strcmp0(info->subsystem, "tty") == 0) {
					if (g_strcmp0(info->number, "00") == 0)
						mdm = info->devnode;
				} else if (g_strcmp0(info->subsystem, "net")
									== 0) {
					if (g_strcmp0(info->number, "06") == 0)
						net = info->devnode;
					if (g_strcmp0(info->number, "08") == 0)
						net2 = info->devnode;
					if (g_strcmp0(info->number, "0a") == 0)
						net3 = info->devnode;
				}
			} else {
				if (g_strcmp0(info->subsystem, "tty") == 0) {
					if (g_strcmp0(info->number, "02") == 0)
						mdm = info->devnode;
				} else if (g_strcmp0(info->subsystem, "net")
									== 0) {
					if (g_strcmp0(info->number, "00") == 0)
						net = info->devnode;
				}
			}

			ofono_modem_set_string(modem->modem, "CtrlPath",
								"/USBCDC/0");
			ofono_modem_set_string(modem->modem, "DataPath",
								"/USBHS/NCM/");
		}
	}

	if (mdm == NULL || net == NULL)
		return FALSE;

	DBG("modem=%s net=%s\n", mdm, net);

	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	if (net2)
		ofono_modem_set_string(modem->modem, "NetworkInterface2", net2);

	if (net3)
		ofono_modem_set_string(modem->modem, "NetworkInterface3", net3);

	return TRUE;
}

static gboolean setup_sim7x00(struct modem_info *modem)
{
	const char *audio = NULL, *diag = NULL, *gps = NULL;
	const char *mdm = NULL, *net = NULL, *ppp = NULL;
	const char *qmi = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s %s", info->devnode, info->interface,
						info->number, info->label,
						info->sysattr, info->subsystem);

		/*
		 * SIM7100 serial port layout:
		 * 0: QCDM/DIAG
		 * 1: NMEA
		 * 2: AT
		 * 3: AT/PPP
		 * 4: audio
		 *
		 * -- https://www.spinics.net/lists/linux-usb/msg135728.html
		 */
		if (g_strcmp0(info->subsystem, "usbmisc") == 0) /* cdc-wdm */
			qmi = info->devnode; /* SIM7600 */
		else if (g_strcmp0(info->subsystem, "net") == 0) /* wwan */
			net = info->devnode; /* SIM7600 */
		else if (g_strcmp0(info->subsystem, "tty") == 0) {
			if (g_strcmp0(info->interface, "255/255/255") == 0) {
				if (g_strcmp0(info->number, "00") == 0)
					diag = info->devnode; /* SIM7x00 */
			} else if (g_strcmp0(info->interface, "255/0/0") == 0) {
				if (g_strcmp0(info->number, "01") == 0)
					gps = info->devnode; /* SIM7x00 */
				else if (g_strcmp0(info->number, "02") == 0)
					mdm = info->devnode; /* SIM7x00 */
				else if (g_strcmp0(info->number, "03") == 0)
					ppp = info->devnode; /* SIM7100 */
				else if (g_strcmp0(info->number, "04") == 0)
					audio = info->devnode; /* SIM7100 */
			}
		}
	}

	if (mdm == NULL)
		return FALSE;

	if (qmi != NULL && net != NULL) {
		DBG("qmi=%s net=%s mdm=%s gps=%s diag=%s",
						qmi, net, mdm, gps, diag);

		ofono_modem_set_driver(modem->modem, "gobi");

		ofono_modem_set_string(modem->modem, "Device", qmi);
		ofono_modem_set_string(modem->modem, "Modem", mdm);
		ofono_modem_set_string(modem->modem, "NetworkInterface", net);
	} else {
		DBG("at=%s ppp=%s gps=%s diag=%s, audio=%s",
						mdm, ppp, gps, diag, audio);

		ofono_modem_set_driver(modem->modem, "sim7100");

		ofono_modem_set_string(modem->modem, "AT", mdm);
		ofono_modem_set_string(modem->modem, "PPP", ppp);
		ofono_modem_set_string(modem->modem, "Audio", audio);
	}

	ofono_modem_set_string(modem->modem, "GPS", gps);
	ofono_modem_set_string(modem->modem, "Diag", diag);
	return TRUE;
}

static struct {
	const char *name;
	gboolean (*setup)(struct modem_info *modem);
	const char *sysattr;
} driver_list[] = {
	{ "isiusb",	setup_isi,	"type"			},
	{ "mbm",	setup_mbm,	"device/interface"	},
	{ "hso",	setup_hso,	"hsotype"		},
	{ "gobi",	setup_gobi	},
	{ "sierra",	setup_sierra	},
	{ "huawei",	setup_huawei	},
	{ "speedupcdma",setup_speedup	},
	{ "speedup",	setup_speedup	},
	{ "linktop",	setup_linktop	},
	{ "alcatel",	setup_alcatel	},
	{ "novatel",	setup_novatel	},
	{ "nokia",	setup_nokia	},
	{ "telit",	setup_telit,	"device/interface"	},
	{ "telitqmi",	setup_telitqmi	},
	{ "simcom",	setup_simcom	},
	{ "sim7x00",	setup_sim7x00	},
	{ "zte",	setup_zte	},
	{ "icera",	setup_icera	},
	{ "samsung",	setup_samsung	},
	{ "quectel",	setup_quectel	},
	{ "quectelqmi",	setup_quectelqmi},
	{ "ublox",	setup_ublox	},
	{ "gemalto",	setup_gemalto	},
	{ "xmm7xxx",	setup_xmm7xxx	},
	{ "mbim",	setup_mbim	},
	{ "droid",	setup_droid	},
	/* Following are non-USB modems */
	{ "ifx",	setup_ifx		},
	{ "u8500",	setup_isi_serial	},
	{ "n900",	setup_isi_serial	},
	{ "calypso",	setup_serial_modem	},
	{ "cinterion",	setup_serial_modem	},
	{ "nokiacdma",	setup_serial_modem	},
	{ "sim900",	setup_serial_modem	},
	{ "wavecom",	setup_wavecom		},
	{ "tc65",	setup_tc65		},
	{ "ehs6",	setup_ehs6		},
	{ }
};

static GHashTable *modem_list;

static const char *get_sysattr(const char *driver)
{
	unsigned int i;

	for (i = 0; driver_list[i].name; i++) {
		if (g_str_equal(driver_list[i].name, driver) == TRUE)
			return driver_list[i].sysattr;
	}

	return NULL;
}

static void device_info_free(struct device_info* info)
{
	g_free(info->devpath);
	g_free(info->devnode);
	g_free(info->interface);
	g_free(info->number);
	g_free(info->label);
	g_free(info->sysattr);
	g_free(info->subsystem);
	g_free(info);
}

static void serial_device_info_free(struct serial_device_info* info)
{
	g_free(info->devpath);
	g_free(info->devnode);
	g_free(info->subsystem);
	udev_device_unref(info->dev);
	g_free(info);
}

static void destroy_modem(gpointer data)
{
	struct modem_info *modem = data;
	GSList *list;

	DBG("%s", modem->syspath);

	ofono_modem_remove(modem->modem);

	switch (modem->type) {
	case MODEM_TYPE_USB:
	case MODEM_TYPE_PCIE:
		for (list = modem->devices; list; list = list->next) {
			struct device_info *info = list->data;

			DBG("%s", info->devnode);
			device_info_free(info);
		}

		g_slist_free(modem->devices);
		break;
	case MODEM_TYPE_SERIAL:
		serial_device_info_free(modem->serial);
		break;
	}

	g_free(modem->syspath);
	g_free(modem->devname);
	g_free(modem->driver);
	g_free(modem->vendor);
	g_free(modem->model);
	g_free(modem);
}

static gboolean check_remove(gpointer key, gpointer value, gpointer user_data)
{
	struct modem_info *modem = value;
	const char *devpath = user_data;
	GSList *list;

	switch (modem->type) {
	case MODEM_TYPE_USB:
	case MODEM_TYPE_PCIE:
		for (list = modem->devices; list; list = list->next) {
			struct device_info *info = list->data;

			if (g_strcmp0(info->devpath, devpath) == 0)
				return TRUE;
		}
		break;
	case MODEM_TYPE_SERIAL:
		if (g_strcmp0(modem->serial->devpath, devpath) == 0)
			return TRUE;
		break;
	}

	return FALSE;
}

static void remove_device(struct udev_device *device)
{
	const char *syspath;

	syspath = udev_device_get_syspath(device);
	if (syspath == NULL)
		return;

	DBG("%s", syspath);

	g_hash_table_foreach_remove(modem_list, check_remove,
						(char *) syspath);
}

static gint compare_device(gconstpointer a, gconstpointer b)
{
	const struct device_info *info1 = a;
	const struct device_info *info2 = b;

	return g_strcmp0(info1->number, info2->number);
}

/*
 * Here we try to find the "modem device".
 *
 * In this variant we identify the "modem device" as simply the device
 * that has the OFONO_DRIVER property.  If the device node doesn't
 * have this property itself, then we do a brute force search for it
 * through the device hierarchy.
 *
 */
static struct udev_device* get_serial_modem_device(struct udev_device *dev)
{
	const char* driver;

	while (dev) {
		driver = udev_device_get_property_value(dev, "OFONO_DRIVER");
		if (driver)
			return dev;

		dev = udev_device_get_parent(dev);
	}

	return NULL;
}

/*
 * Add 'legacy' device
 *
 * The term legacy is a bit misleading, but this adds devices according
 * to the original ofono model.
 *
 * - We cannot assume that these are USB devices
 * - The modem consists of only a single interface
 * - The device must have an OFONO_DRIVER property from udev
 */
static void add_serial_device(struct udev_device *dev)
{
	const char *syspath, *devpath, *devname, *devnode;
	struct modem_info *modem;
	struct serial_device_info *info;
	const char *subsystem;
	struct udev_device* mdev;
	const char* driver;

	mdev = get_serial_modem_device(dev);
	if (!mdev) {
		DBG("Device is missing required OFONO_DRIVER property");
		return;
	}

	driver = udev_device_get_property_value(mdev, "OFONO_DRIVER");

	syspath = udev_device_get_syspath(mdev);
	devname = udev_device_get_devnode(mdev);
	devpath = udev_device_get_devpath(mdev);

	devnode = udev_device_get_devnode(dev);

	if (!syspath || !devpath)
		return;

	modem = g_hash_table_lookup(modem_list, syspath);
	if (modem == NULL) {
		modem = g_try_new0(struct modem_info, 1);
		if (modem == NULL)
			return;

		modem->type = MODEM_TYPE_SERIAL;
		modem->syspath = g_strdup(syspath);
		modem->devname = g_strdup(devname);
		modem->driver = g_strdup(driver);

		g_hash_table_replace(modem_list, modem->syspath, modem);
	}

	subsystem = udev_device_get_subsystem(dev);

	DBG("%s", syspath);
	DBG("%s", devpath);
	DBG("%s (%s)", devnode, driver);

	info = g_try_new0(struct serial_device_info, 1);
	if (info == NULL)
		return;

	info->devpath = g_strdup(devpath);
	info->devnode = g_strdup(devnode);
	info->subsystem = g_strdup(subsystem);
	info->dev = udev_device_ref(dev);

	modem->serial = info;
}

static void add_device(const char *syspath, const char *devname,
			const char *driver, const char *vendor,
			const char *model, struct udev_device *device,
			enum modem_type type)
{
	struct udev_device *usb_interface;
	const char *devpath, *devnode, *interface, *number;
	const char *label, *sysattr, *subsystem;
	struct modem_info *modem;
	struct device_info *info;
	struct udev_device *parent;

	devpath = udev_device_get_syspath(device);
	if (devpath == NULL)
		return;

	modem = g_hash_table_lookup(modem_list, syspath);
	if (modem == NULL) {
		modem = g_try_new0(struct modem_info, 1);
		if (modem == NULL)
			return;

		modem->type = type;
		modem->syspath = g_strdup(syspath);
		modem->devname = g_strdup(devname);
		modem->driver = g_strdup(driver);
		modem->vendor = g_strdup(vendor);
		modem->model = g_strdup(model);

		modem->sysattr = get_sysattr(driver);

		g_hash_table_replace(modem_list, modem->syspath, modem);
	}

	if (modem->type == MODEM_TYPE_USB) {
		devnode = udev_device_get_devnode(device);
		if (devnode == NULL) {
			devnode = udev_device_get_property_value(device,
							"INTERFACE");
			if (devnode == NULL)
				return;
		}

		usb_interface = udev_device_get_parent_with_subsystem_devtype(
							device, "usb",
							"usb_interface");
		if (usb_interface == NULL)
			return;

		interface = udev_device_get_property_value(usb_interface,
							"INTERFACE");
		number = udev_device_get_property_value(device,
						"ID_USB_INTERFACE_NUM");

		label = udev_device_get_property_value(device, "OFONO_LABEL");
		if (!label)
			label = udev_device_get_property_value(usb_interface,
							"OFONO_LABEL");
	} else {
		devnode = NULL;
		interface = udev_device_get_property_value(device,
							"INTERFACE");
		number = NULL;
		label = NULL;
	}

	/* If environment variable is not set, get value from attributes (or parent's ones) */
	if (number == NULL) {
		number = udev_device_get_sysattr_value(device,
							"bInterfaceNumber");

		if (number == NULL) {
			parent = udev_device_get_parent(device);
			number = udev_device_get_sysattr_value(parent,
							"bInterfaceNumber");
		}
	}

	subsystem = udev_device_get_subsystem(device);

	if (modem->sysattr != NULL)
		sysattr = udev_device_get_sysattr_value(device, modem->sysattr);
	else
		sysattr = NULL;

	DBG("%s", syspath);
	DBG("%s", devpath);
	DBG("%s (%s) %s [%s] ==> %s %s", devnode, driver,
					interface, number, label, sysattr);

	info = g_try_new0(struct device_info, 1);
	if (info == NULL)
		return;

	info->devpath = g_strdup(devpath);
	info->devnode = g_strdup(devnode);
	info->interface = g_strdup(interface);
	info->number = g_strdup(number);
	info->label = g_strdup(label);
	info->sysattr = g_strdup(sysattr);
	info->subsystem = g_strdup(subsystem);

	modem->devices = g_slist_insert_sorted(modem->devices, info,
							compare_device);
}

static struct {
	const char *driver;
	const char *drv;
	const char *vid;
	const char *pid;
} vendor_list[] = {
	{ "isiusb",	"cdc_phonet"			},
	{ "linktop",	"cdc_acm",	"230d"		},
	{ "icera",	"cdc_acm",	"19d2"		},
	{ "icera",	"cdc_ether",	"19d2"		},
	{ "icera",	"cdc_acm",	"04e8", "6872"	},
	{ "icera",	"cdc_ether",	"04e8", "6872"	},
	{ "icera",	"cdc_acm",	"0421", "0633"	},
	{ "icera",	"cdc_ether",	"0421", "0633"	},
	{ "mbm",	"cdc_acm",	"0bdb"		},
	{ "mbm",	"cdc_ether",	"0bdb"		},
	{ "mbm",	"cdc_ncm",	"0bdb"		},
	{ "mbm",	"cdc_acm",	"0fce"		},
	{ "mbm",	"cdc_ether",	"0fce"		},
	{ "mbm",	"cdc_ncm",	"0fce"		},
	{ "mbm",	"cdc_acm",	"413c"		},
	{ "mbm",	"cdc_ether",	"413c"		},
	{ "mbm",	"cdc_ncm",	"413c"		},
	{ "mbim",	"cdc_mbim"			},
	{ "mbm",	"cdc_acm",	"03f0"		},
	{ "mbm",	"cdc_ether",	"03f0"		},
	{ "mbm",	"cdc_ncm",	"03f0"		},
	{ "mbm",	"cdc_acm",	"0930"		},
	{ "mbm",	"cdc_ether",	"0930"		},
	{ "mbm",	"cdc_ncm",	"0930"		},
	{ "hso",	"hso"				},
	{ "gobi",	"qmi_wwan"			},
	{ "gobi",	"qcserial"			},
	{ "sierra",	"qmi_wwan",	"1199"		},
	{ "sierra",	"qcserial",	"1199"		},
	{ "sierra",	"sierra"			},
	{ "sierra",	"sierra_net"			},
	{ "option",	"option",	"0af0"		},
	{ "huawei",	"option",	"201e"		},
	{ "huawei",	"cdc_wdm",	"12d1"		},
	{ "huawei",	"cdc_ether",	"12d1"		},
	{ "huawei",	"qmi_wwan",	"12d1"		},
	{ "huawei",	"option",	"12d1"		},
	{ "speedupcdma","option",	"1c9e", "9e00"	},
	{ "speedup",	"option",	"1c9e"		},
	{ "speedup",	"option",	"2020"		},
	{ "alcatel",	"option",	"1bbb", "0017"	},
	{ "novatel",	"option",	"1410"		},
	{ "zte",	"option",	"19d2"		},
	{ "simcom",	"option",	"05c6", "9000"	},
	{ "sim7x00",	"option",	"1e0e", "9001"	},
	{ "sim7x00",	"qmi_wwan",	"1e0e",	"9001"	},
	{ "telit",	"usbserial",	"1bc7"		},
	{ "telit",	"option",	"1bc7"		},
	{ "telit",	"cdc_acm",	"1bc7", "0021"	},
	{ "telitqmi",	"qmi_wwan",	"1bc7", "1201"	},
	{ "telitqmi",	"option",	"1bc7", "1201"	},
	{ "droid",	"qmi_wwan",	"22b8", "2a70"	},
	{ "droid",	"option",	"22b8", "2a70"	},
	{ "nokia",	"option",	"0421", "060e"	},
	{ "nokia",	"option",	"0421", "0623"	},
	{ "samsung",	"option",	"04e8", "6889"	},
	{ "samsung",	"kalmia"			},
	{ "quectel",	"option",	"05c6", "9090"	},
	{ "quectelqmi",	"qmi_wwan",	"2c7c", "0121"	},
	{ "quectelqmi",	"qcserial",	"2c7c", "0121"	},
	{ "quectelqmi",	"qmi_wwan",	"2c7c", "0125"	},
	{ "quectelqmi",	"qcserial",	"2c7c", "0125"	},
	{ "quectelqmi",	"qmi_wwan",	"2c7c", "0296"	},
	{ "quectelqmi",	"qcserial",	"2c7c", "0296"	},
	{ "ublox",	"cdc_acm",	"1546", "1010"	},
	{ "ublox",	"cdc_ncm",	"1546", "1010"	},
	{ "ublox",	"cdc_acm",	"1546", "1102"	},
	{ "ublox",	"cdc_acm",	"1546", "110a"	},
	{ "ublox",	"cdc_ncm",	"1546", "110a"	},
	{ "ublox",	"rndis_host",	"1546", "1146"	},
	{ "ublox",	"cdc_acm",	"1546", "1146"	},
	{ "gemalto",	"option",	"1e2d",	"0053"	},
	{ "gemalto",	"cdc_wdm",	"1e2d",	"0053"	},
	{ "gemalto",	"qmi_wwan",	"1e2d",	"0053"	},
	{ "gemalto",	"cdc_acm",	"1e2d",	"0061"	},
	{ "gemalto",	"cdc_ether",	"1e2d",	"0061"	},
	{ "gemalto",	"cdc_acm",	"1e2d",	"005b"	},
	{ "gemalto",	"cdc_ether",	"1e2d",	"005b"	},
	{ "telit",	"cdc_ncm",	"1bc7", "0036"	},
	{ "telit",	"cdc_acm",	"1bc7", "0036"	},
	{ "xmm7xxx",	"cdc_acm",	"8087"		},
	{ "xmm7xxx",	"cdc_ncm",	"8087"		},
	{ }
};

static void check_usb_device(struct udev_device *device)
{
	struct udev_device *usb_device;
	const char *syspath, *devname, *driver;
	const char *vendor = NULL, *model = NULL;

	usb_device = udev_device_get_parent_with_subsystem_devtype(device,
							"usb", "usb_device");
	if (usb_device == NULL)
		return;

	syspath = udev_device_get_syspath(usb_device);
	if (syspath == NULL)
		return;

	devname = udev_device_get_devnode(usb_device);
	if (devname == NULL)
		return;

	vendor = udev_device_get_property_value(usb_device, "ID_VENDOR_ID");
	model = udev_device_get_property_value(usb_device, "ID_MODEL_ID");

	driver = udev_device_get_property_value(usb_device, "OFONO_DRIVER");
	if (!driver) {
		struct udev_device *usb_interface =
			udev_device_get_parent_with_subsystem_devtype(
				device, "usb", "usb_interface");

		if (usb_interface)
			driver = udev_device_get_property_value(
					usb_interface, "OFONO_DRIVER");
	}

	if (driver == NULL) {
		const char *drv;
		unsigned int i;

		drv = udev_device_get_property_value(device, "ID_USB_DRIVER");
		if (drv == NULL) {
			drv = udev_device_get_driver(device);
			if (drv == NULL) {
				struct udev_device *parent;

				parent = udev_device_get_parent(device);
				if (parent == NULL)
					return;

				drv = udev_device_get_driver(parent);
				if (drv == NULL)
					return;
			}
		}


		DBG("%s [%s:%s]", drv, vendor, model);

		if (vendor == NULL || model == NULL)
			return;

		for (i = 0; vendor_list[i].driver; i++) {
			if (g_str_equal(vendor_list[i].drv, drv) == FALSE)
				continue;

			if (vendor_list[i].vid) {
				if (!g_str_equal(vendor_list[i].vid, vendor))
					continue;
			}

			if (vendor_list[i].pid) {
				if (!g_str_equal(vendor_list[i].pid, model))
					continue;
			}

			driver = vendor_list[i].driver;
		}

		if (driver == NULL)
			return;
	}

	add_device(syspath, devname, driver, vendor, model, device,
			MODEM_TYPE_USB);
}

static const struct {
	const char *driver;
	const char *drv;
	const char *vid;
	const char *pid;
} pci_driver_list[] = {
	{ "xmm7xxx",	"imc_ipc",	"0x8086",	"0x7560"},
	{ }
};

static void check_pci_device(struct udev_device *device)
{
	const char *syspath, *devname, *driver;
	const char *vendor = NULL, *model = NULL, *drv = NULL;
	unsigned int i;

	syspath = udev_device_get_syspath(device);

	if (syspath == NULL)
		return;

	devname = udev_device_get_devnode(device);
	vendor = udev_device_get_sysattr_value(device, "vendor");
	model = udev_device_get_sysattr_value(device, "device");
	driver = udev_device_get_property_value(device, "OFONO_DRIVER");
	drv = udev_device_get_property_value(device, "DRIVER");
	DBG("%s [%s:%s]", drv, vendor, model);

	if (vendor == NULL || model == NULL || drv == NULL)
		return;

	for (i = 0; pci_driver_list[i].driver; i++) {
		if (g_str_equal(pci_driver_list[i].drv, drv) == FALSE)
			continue;

		if (pci_driver_list[i].vid) {
			if (!g_str_equal(pci_driver_list[i].vid, vendor))
				continue;
		}

		if (pci_driver_list[i].pid) {
			if (!g_str_equal(pci_driver_list[i].pid, model))
				continue;
		}

		driver = pci_driver_list[i].driver;
	}

	if (driver == NULL)
		return;

	add_device(syspath, devname, driver, vendor, model, device,
			MODEM_TYPE_PCIE);
}
static void check_device(struct udev_device *device)
{
	const char *bus;

	bus = udev_device_get_property_value(device, "ID_BUS");
	if (bus == NULL) {
		bus = udev_device_get_subsystem(device);
		if (bus == NULL)
			return;
	}

	if ((g_str_equal(bus, "usb") == TRUE) ||
			(g_str_equal(bus, "usbmisc") == TRUE))
		check_usb_device(device);
	else if (g_str_equal(bus, "pci") == TRUE)
		check_pci_device(device);
	else
		add_serial_device(device);

}

static gboolean create_modem(gpointer key, gpointer value, gpointer user_data)
{
	struct modem_info *modem = value;
	const char *syspath = key;
	unsigned int i;

	if (modem->modem != NULL)
		return FALSE;

	DBG("%s", syspath);

	if (modem->devices == NULL)
		return TRUE;

	DBG("driver=%s", modem->driver);

	modem->modem = ofono_modem_create(NULL, modem->driver);
	if (modem->modem == NULL)
		return TRUE;

	for (i = 0; driver_list[i].name; i++) {
		if (g_str_equal(driver_list[i].name, modem->driver) == FALSE)
			continue;

		if (driver_list[i].setup(modem) == TRUE) {
			ofono_modem_set_string(modem->modem, "SystemPath",
								syspath);
			if (ofono_modem_register(modem->modem) < 0) {
				DBG("could not register modem '%s'", modem->driver);
				return TRUE;
			}

			return FALSE;
		}
	}

	return TRUE;
}

static void enumerate_devices(struct udev *context)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *entry;

	DBG("");

	enumerate = udev_enumerate_new(context);
	if (enumerate == NULL)
		return;

	udev_enumerate_add_match_subsystem(enumerate, "tty");
	udev_enumerate_add_match_subsystem(enumerate, "usb");
	udev_enumerate_add_match_subsystem(enumerate, "usbmisc");
	udev_enumerate_add_match_subsystem(enumerate, "net");
	udev_enumerate_add_match_subsystem(enumerate, "hsi");
	udev_enumerate_add_match_subsystem(enumerate, "pci");

	udev_enumerate_scan_devices(enumerate);

	entry = udev_enumerate_get_list_entry(enumerate);
	while (entry) {
		const char *syspath = udev_list_entry_get_name(entry);
		struct udev_device *device;

		device = udev_device_new_from_syspath(context, syspath);
		if (device != NULL) {
			check_device(device);
			udev_device_unref(device);
		}

		entry = udev_list_entry_get_next(entry);
	}

	udev_enumerate_unref(enumerate);

	g_hash_table_foreach_remove(modem_list, create_modem, NULL);
}

static struct udev *udev_ctx;
static struct udev_monitor *udev_mon;
static guint udev_watch = 0;
static guint udev_delay = 0;

static gboolean check_modem_list(gpointer user_data)
{
	udev_delay = 0;

	DBG("");

	g_hash_table_foreach_remove(modem_list, create_modem, NULL);

	return FALSE;
}

static gboolean udev_event(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct udev_device *device;
	const char *action;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
		ofono_warn("Error with udev monitor channel");
		udev_watch = 0;
		return FALSE;
	}

	device = udev_monitor_receive_device(udev_mon);
	if (device == NULL)
		return TRUE;

	action = udev_device_get_action(device);
	if (action == NULL)
		return TRUE;

	if (g_str_equal(action, "add") == TRUE) {
		if (udev_delay > 0)
			g_source_remove(udev_delay);

		check_device(device);

		udev_delay = g_timeout_add_seconds(1, check_modem_list, NULL);
	} else if (g_str_equal(action, "remove") == TRUE)
		remove_device(device);

	udev_device_unref(device);

	return TRUE;
}

static void udev_start(void)
{
	GIOChannel *channel;
	int fd;

	DBG("");

	if (udev_monitor_enable_receiving(udev_mon) < 0) {
		ofono_error("Failed to enable udev monitor");
		return;
	}

	enumerate_devices(udev_ctx);

	fd = udev_monitor_get_fd(udev_mon);

	channel = g_io_channel_unix_new(fd);
	if (channel == NULL)
		return;

	udev_watch = g_io_add_watch(channel,
				G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
							udev_event, NULL);

	g_io_channel_unref(channel);
}

static int detect_init(void)
{
	udev_ctx = udev_new();
	if (udev_ctx == NULL) {
		ofono_error("Failed to create udev context");
		return -EIO;
	}

	udev_mon = udev_monitor_new_from_netlink(udev_ctx, "udev");
	if (udev_mon == NULL) {
		ofono_error("Failed to create udev monitor");
		udev_unref(udev_ctx);
		udev_ctx = NULL;
		return -EIO;
	}

	modem_list = g_hash_table_new_full(g_str_hash, g_str_equal,
						NULL, destroy_modem);

	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "tty", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "usb", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(udev_mon,
							"usbmisc", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "net", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "hsi", NULL);

	udev_monitor_filter_update(udev_mon);

	udev_start();

	return 0;
}

static void detect_exit(void)
{
	if (udev_delay > 0)
		g_source_remove(udev_delay);

	if (udev_watch > 0)
		g_source_remove(udev_watch);

	if (udev_ctx == NULL)
		return;

	udev_monitor_filter_remove(udev_mon);

	g_hash_table_destroy(modem_list);

	udev_monitor_unref(udev_mon);
	udev_unref(udev_ctx);
}

OFONO_PLUGIN_DEFINE(udevng, "udev hardware detection", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, detect_init, detect_exit)
