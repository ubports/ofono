/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2018 Jolla Ltd.
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

#include <ofono/ril-transport.h>
#include <ofono/log.h>

#include <string.h>
#include <errno.h>

static GSList *ril_transports = NULL;

struct grilio_transport *ofono_ril_transport_connect(const char *name,
							GHashTable *params)
{
	if (name) {
		GSList *l;

		for (l = ril_transports; l; l = l->next) {
			const struct ofono_ril_transport *t = l->data;

			if (!strcmp(name, t->name)) {
				return t->connect ? t->connect(params) : NULL;
			}
		}
		ofono_error("Unknown RIL transport: %s", name);
	}
	return NULL;
}

int ofono_ril_transport_register(const struct ofono_ril_transport *t)
{
	if (!t || !t->name) {
		return -EINVAL;
	} else {
		GSList *l;

		for (l = ril_transports; l; l = l->next) {
			const struct ofono_ril_transport *t1 = l->data;

			if (!strcmp(t->name, t1->name)) {
				DBG("%s already registered", t->name);
				return -EALREADY;
			}
		}

		DBG("%s", t->name);
		ril_transports = g_slist_append(ril_transports, (void*)t);
		return 0;
        }
}

void ofono_ril_transport_unregister(const struct ofono_ril_transport *t)
{
	if (t && t->name) {
		DBG("%s", t->name);
		ril_transports = g_slist_remove(ril_transports, t);
	}
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
