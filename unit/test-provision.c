/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2014 Jolla. All rights reserved.
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

#include <glib.h>
struct ofono_modem;
#include <gprs-provision.h>
#include "plugins/mbpi.h"
#include "plugins/provision.h"


void get_and_print_settings(const char *mcc, const char *mnc,
		const char *spn)
{
	struct ofono_gprs_provision_data *settings;
	int count;
	int i;
	provision_get_settings(mcc, mnc, spn, &settings, &count);
	g_print("Found %d contexts for (%s/%s/%s):\n", count, mcc, mnc, spn);
	for (i = 0; i < count; i++){
		struct ofono_gprs_provision_data ap = settings[i];
		g_print("  Name: %s\n", ap.name);
		g_print("  APN: %s\n", ap.apn);
		g_print("  Type: %s\n", mbpi_ap_type(ap.type));
		if (ap.username)
			g_print("  Username: %s\n", ap.username);
		if (ap.password)
			g_print("  Password: %s\n", ap.password);
		if (ap.message_proxy)
			g_print("  MMS proxy: %s\n", ap.message_proxy);
		if (ap.message_center)
			g_print("  MMS center: %s\n", ap.message_center);
		g_print("----------\n");
	}
}

static void test_get_settings(void)
{
	/* not in database */
	get_and_print_settings("999", "999", NULL);

	/* partial and case-insensitive matching */
	get_and_print_settings("244", "91", "sonera");
	get_and_print_settings("244", "91", "sONErA");
	get_and_print_settings("244", "91", "sone");
	get_and_print_settings("244", "91", "nera");

	/* related to Sonera/Finland network */
	get_and_print_settings("244", "91", NULL);
	get_and_print_settings("244", "91", "sonera");
	get_and_print_settings("244", "91", "aina");

	/* related to DNA/Finland network */
	get_and_print_settings("244", "03", NULL);
	get_and_print_settings("244", "03", "dna");
	get_and_print_settings("244", "03", "aina");
	get_and_print_settings("244", "04", NULL);
	get_and_print_settings("244", "04", "dna");
	get_and_print_settings("244", "04", "aina");

	/* related to O2/UK network */
	get_and_print_settings("234", "10", NULL);
	get_and_print_settings("234", "10", "o2");
	get_and_print_settings("234", "10", "tesco");
	get_and_print_settings("234", "10", "giffgaff");

	/* related to E-Plus/Germany network */
	get_and_print_settings("262", "03", NULL);
	get_and_print_settings("262", "03", "E-Plus");
	get_and_print_settings("262", "03", "simyo");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/testprovision/get_settings", test_get_settings);
	return g_test_run();
}
