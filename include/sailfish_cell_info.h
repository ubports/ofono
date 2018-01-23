/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2018 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef SAILFISH_CELL_INFO_H
#define SAILFISH_CELL_INFO_H

#include <glib.h>

enum sailfish_cell_type {
	SAILFISH_CELL_TYPE_GSM,
	SAILFISH_CELL_TYPE_WCDMA,
	SAILFISH_CELL_TYPE_LTE
};

#define SAILFISH_CELL_INVALID_VALUE (INT_MAX)

struct sailfish_cell_info_gsm {
	int mcc;            /* Mobile Country Code (0..999) */
	int mnc;            /* Mobile Network Code (0..999) */
	int lac;            /* Location Area Code (0..65535) */
	int cid;            /* GSM Cell Identity (0..65535) TS 27.007 */
	int arfcn;          /* 16-bit GSM Absolute RF channel number */
	int bsic;           /* 6-bit Base Station Identity Code */
	int signalStrength; /* (0-31, 99) TS 27.007 */
	int bitErrorRate;   /* (0-7, 99) TS 27.007 */
	int timingAdvance;  /* Timing Advance. 1 period = 48/13 us */
};

struct sailfish_cell_info_wcdma {
	int mcc;            /* Mobile Country Code (0..999) */
	int mnc;            /* Mobile Network Code (0..999) */
	int lac;            /* Location Area Code (0..65535) */
	int cid;            /* UMTS Cell Identity (0..268435455) TS 25.331 */
	int psc;            /* Primary Scrambling Code (0..511) TS 25.331) */
	int uarfcn;         /* 16-bit UMTS Absolute RF Channel Number */
	int signalStrength; /* (0-31, 99) TS 27.007 */
	int bitErrorRate;   /* (0-7, 99) TS 27.007 */
};

struct sailfish_cell_info_lte {
	int mcc;            /* Mobile Country Code (0..999) */
	int mnc;            /* Mobile Network Code (0..999) */
	int ci;             /* Cell Identity */
	int pci;            /* Physical cell id (0..503) */
	int tac;            /* Tracking area code */
	int earfcn;         /* 18-bit LTE Absolute RC Channel Number */
	int signalStrength; /* (0-31, 99) TS 27.007 8.5 */
	int rsrp;           /* Reference Signal Receive Power TS 36.133 */
	int rsrq;           /* Reference Signal Receive Quality TS 36.133 */
	int rssnr;          /* Reference Signal-to-Noise Ratio TS 36.101*/
	int cqi;            /* Channel Quality Indicator TS 36.101 */
	int timingAdvance;  /* (Distance = 300m/us) TS 36.321 */
};

struct sailfish_cell {
	enum sailfish_cell_type type;
	gboolean registered;
	union {
		struct sailfish_cell_info_gsm gsm;
		struct sailfish_cell_info_wcdma wcdma;
		struct sailfish_cell_info_lte lte;
	} info;
};

struct sailfish_cell_info {
	const struct sailfish_cell_info_proc *proc;
	GSList *cells;
};

typedef void (*sailfish_cell_info_cb_t)(struct sailfish_cell_info *info,
								void *arg);

struct sailfish_cell_info_proc {
	void (*ref)(struct sailfish_cell_info *info);
	void (*unref)(struct sailfish_cell_info *info);
	gulong (*add_cells_changed_handler)(struct sailfish_cell_info *info,
					sailfish_cell_info_cb_t cb, void *arg);
	void (*remove_handler)(struct sailfish_cell_info *info, gulong id);
};

/* Utilities */
gint sailfish_cell_compare_func(gconstpointer v1, gconstpointer v2);
gint sailfish_cell_compare_location(const struct sailfish_cell *c1,
					const struct sailfish_cell *c2);

/* Cell info object API */
struct sailfish_cell_info *sailfish_cell_info_ref
				(struct sailfish_cell_info *info);
void sailfish_cell_info_unref(struct sailfish_cell_info *info);
gulong sailfish_cell_info_add_cells_changed_handler
				(struct sailfish_cell_info *info,
					sailfish_cell_info_cb_t cb, void *arg);
void sailfish_cell_info_remove_handler(struct sailfish_cell_info *info,
					gulong id);

#endif /* SAILFISH_CELINFO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
