/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * UART meter ingest: reads newline-terminated JSON frames from an external
 * metrology board (e.g. an NXP Kinetis-M smart-meter reference design) and
 * holds the latest readings for the telemetry loop.
 *
 * Two frame formats are understood (115200 8N1), detected per line:
 *
 * 1. JSON lines (any metering firmware can printf this):
 *   {"va":119.8,"vb":120.1,"ia":0.81,"ib":0.79,"ptot":181.2,"kwh":22.24,"freq":60.01}
 *    Unknown keys are ignored; absent keys keep their previous value.
 *
 * 2. The NXP EasyEVSE (TWR-KM35) polled protocol: the ingest sends the
 *    poll character '0' every few seconds and parses the reply
 *      <I_RMS>[1]<U_RMS>[2]<P>[3]<Status_Index>[4]\r
 *    mapping [1]->ia, [2]->va, [3]->ptot, [4]->state.
 */

#ifndef METER_UART_H
#define METER_UART_H

#include <stdbool.h>
#include <stdint.h>

struct meter_values {
	double va, vb, ia, ib, ptot, kwh, freq;
	double state;         /* EasyEVSE Status_Index (0 when unused) */
	uint32_t frames;      /* frames parsed since boot */
	bool online;          /* a frame arrived within the last 15 s */
};

/* Start the ingest on the devicetree alias `meter_uart`. Returns 0, or
 * -ENODEV when the UART is absent/disabled (demo runs without a meter). */
int meter_uart_start(void);

/* Snapshot the latest readings. Returns false until the first frame. */
bool meter_uart_get(struct meter_values *out);

#endif /* METER_UART_H */
