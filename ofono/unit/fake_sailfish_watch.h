/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Jolla Ltd.
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

#ifndef SAILFISH_FAKE_WATCH_H
#define SAILFISH_FAKE_WATCH_H

#include "sailfish_watch.h"

enum sailfish_watch_signal {
	WATCH_SIGNAL_MODEM_CHANGED,
	WATCH_SIGNAL_ONLINE_CHANGED,
	WATCH_SIGNAL_SIM_CHANGED,
	WATCH_SIGNAL_SIM_STATE_CHANGED,
	WATCH_SIGNAL_ICCID_CHANGED,
	WATCH_SIGNAL_IMSI_CHANGED,
	WATCH_SIGNAL_SPN_CHANGED,
	WATCH_SIGNAL_NETREG_CHANGED,
	WATCH_SIGNAL_COUNT
};

void fake_sailfish_watch_signal_queue(struct sailfish_watch *watch,
					enum sailfish_watch_signal id);
void fake_sailfish_watch_emit_queued_signals(struct sailfish_watch *watch);
void fake_sailfish_watch_set_ofono_sim(struct sailfish_watch *watch,
					struct ofono_sim *sim);
void fake_sailfish_watch_set_ofono_iccid(struct sailfish_watch *watch,
					const char *iccid);
void fake_sailfish_watch_set_ofono_imsi(struct sailfish_watch *watch,
					const char *imsi);
void fake_sailfish_watch_set_ofono_spn(struct sailfish_watch *watch,
					const char *spn);
void fake_sailfish_watch_set_ofono_netreg(struct sailfish_watch *watch,
					struct ofono_netreg *netreg);

#endif /* FAKE_SAILFISH_WATCH_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
