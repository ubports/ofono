/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2021 Jolla Ltd.
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

#ifndef OFONO_CELL_INFO_H
#define OFONO_CELL_INFO_H

/* This API exists since mer/1.24+git2 */

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

enum ofono_cell_type {
	OFONO_CELL_TYPE_GSM,
	OFONO_CELL_TYPE_WCDMA,
	OFONO_CELL_TYPE_LTE
};

#define OFONO_CELL_INVALID_VALUE (INT_MAX)

struct ofono_cell_info_gsm {
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

struct ofono_cell_info_wcdma {
	int mcc;            /* Mobile Country Code (0..999) */
	int mnc;            /* Mobile Network Code (0..999) */
	int lac;            /* Location Area Code (0..65535) */
	int cid;            /* UMTS Cell Identity (0..268435455) TS 25.331 */
	int psc;            /* Primary Scrambling Code (0..511) TS 25.331) */
	int uarfcn;         /* 16-bit UMTS Absolute RF Channel Number */
	int signalStrength; /* (0-31, 99) TS 27.007 */
	int bitErrorRate;   /* (0-7, 99) TS 27.007 */
};

struct ofono_cell_info_lte {
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

typedef struct ofono_cell {
	enum ofono_cell_type type;
	ofono_bool_t registered;
	union {
		struct ofono_cell_info_gsm gsm;
		struct ofono_cell_info_wcdma wcdma;
		struct ofono_cell_info_lte lte;
	} info;
} *ofono_cell_ptr;

struct ofono_cell_info {
	const struct ofono_cell_info_proc *proc;
	const ofono_cell_ptr *cells; /* NULL-terminated */
};

typedef void (*ofono_cell_info_cb_t)(struct ofono_cell_info *ci, void *data);

struct ofono_cell_info_proc {
	void (*ref)(struct ofono_cell_info *ci);
	void (*unref)(struct ofono_cell_info *ci);
	unsigned long (*add_change_handler)(struct ofono_cell_info *ci,
					ofono_cell_info_cb_t cb, void *data);
	void (*remove_handler)(struct ofono_cell_info *ci, unsigned long id);
	void (*set_update_interval)(struct ofono_cell_info *ci, int ms);
	void (*set_enabled)(struct ofono_cell_info *ci, ofono_bool_t enabled);
};

/* Wrappers for ofono_cell_info objects */
struct ofono_cell_info *ofono_cell_info_ref(struct ofono_cell_info *ci);
void ofono_cell_info_unref(struct ofono_cell_info *ci);
unsigned long ofono_cell_info_add_change_handler(struct ofono_cell_info *ci,
					ofono_cell_info_cb_t cb, void *data);
void ofono_cell_info_remove_handler(struct ofono_cell_info *ci,
					unsigned long id);
void ofono_cell_info_set_update_interval(struct ofono_cell_info *ci, int ms);
void ofono_cell_info_set_enabled(struct ofono_cell_info *ci, ofono_bool_t on);
int ofono_cell_compare_location(const struct ofono_cell *c1,
					const struct ofono_cell *c2);

#ifdef __cplusplus
}
#endif

#endif /* OFONO_CELL_INFO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
