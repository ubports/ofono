/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2019 Alexander Couzens <lynxis@fe80.eu>
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

#ifndef __OFONO_DRIVER_COMMON_CALL_LIST
#define __OFONO_DRIVER_COMMON_CALL_LIST

#include <glib.h>

gint ofono_call_compare(gconstpointer a, gconstpointer b);
gint ofono_call_compare_by_status(gconstpointer a, gconstpointer b);

#endif /* __OFONO_DRIVER_COMMON_CALL_LIST */
