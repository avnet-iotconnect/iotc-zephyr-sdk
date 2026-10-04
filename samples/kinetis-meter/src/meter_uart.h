/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * UART meter ingest: reads newline-terminated JSON frames from an external
 * metrology board (e.g. an NXP Kinetis-M smart-meter reference design) and
 * holds the latest readings for the telemetry loop.
 *
 * Frame contract (115200 8N1, one JSON object per line, plain numbers):
 *   {"va":119.8,"vb":120.1,"ia":0.81,"ib":0.79,"ptot":181.2,"kwh":22.24,"freq":60.01}
 * Unknown keys are ignored; absent keys keep their previous value.
 */

#ifndef METER_UART_H
#define METER_UART_H

#include <stdbool.h>
#include <stdint.h>

struct meter_values {
	double va, vb, ia, ib, ptot, kwh, freq;
	uint32_t frames;      /* frames parsed since boot */
	bool online;          /* a frame arrived within the last 15 s */
};

/* Start the ingest on the devicetree alias `meter_uart`. Returns 0, or
 * -ENODEV when the UART is absent/disabled (demo runs without a meter). */
int meter_uart_start(void);

/* Snapshot the latest readings. Returns false until the first frame. */
bool meter_uart_get(struct meter_values *out);

#endif /* METER_UART_H */
