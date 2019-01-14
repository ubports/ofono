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

#ifndef FAKE_WATCH_H
#define FAKE_WATCH_H

#include <ofono/watch.h>

enum fake_watch_signal {
	FAKE_WATCH_SIGNAL_MODEM_CHANGED,
	FAKE_WATCH_SIGNAL_ONLINE_CHANGED,
	FAKE_WATCH_SIGNAL_SIM_CHANGED,
	FAKE_WATCH_SIGNAL_SIM_STATE_CHANGED,
	FAKE_WATCH_SIGNAL_ICCID_CHANGED,
	FAKE_WATCH_SIGNAL_IMSI_CHANGED,
	FAKE_WATCH_SIGNAL_SPN_CHANGED,
	FAKE_WATCH_SIGNAL_NETREG_CHANGED,
	FAKE_WATCH_SIGNAL_COUNT
};

void fake_watch_signal_queue(struct ofono_watch *w, enum fake_watch_signal id);
void fake_watch_emit_queued_signals(struct ofono_watch *w);
void fake_watch_set_ofono_sim(struct ofono_watch *w, struct ofono_sim *sim);
void fake_watch_set_ofono_iccid(struct ofono_watch *w, const char *iccid);
void fake_watch_set_ofono_imsi(struct ofono_watch *w, const char *imsi);
void fake_watch_set_ofono_spn(struct ofono_watch *w, const char *spn);
void fake_watch_set_ofono_netreg(struct ofono_watch *w,
					struct ofono_netreg *netreg);

#endif /* FAKE_WATCH_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
