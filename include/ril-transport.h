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

#ifndef __OFONO_RIL_TRANSPORT_H
#define __OFONO_RIL_TRANSPORT_H

#include <ofono/types.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct grilio_transport;

/*
 * The api_version field makes it possible to keep using old plugins
 * even if struct ofono_ril_transport gets extended with new callbacks.
 */

#define OFONO_RIL_TRANSPORT_API_VERSION  (0)

/*
 * The connect callback takes a (char*) -> (char*) hashtable containing
 * transport-specific connection parameters. The caller receives a reference
 * i.e. it has to unref the returned object.
 */
struct ofono_ril_transport {
	const char *name;
	int api_version;        /* OFONO_RIL_TRANSPORT_API_VERSION */
	struct grilio_transport *(*connect)(GHashTable *params);
};

int ofono_ril_transport_register(const struct ofono_ril_transport *t);
void ofono_ril_transport_unregister(const struct ofono_ril_transport *t);

struct grilio_transport *ofono_ril_transport_connect(const char *name,
							GHashTable *params);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_RIL_TRANSPORT_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
