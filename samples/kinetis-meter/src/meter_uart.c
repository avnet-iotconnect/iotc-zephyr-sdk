/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * UART meter ingest (see meter_uart.h).
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cJSON.h"

#include "meter_uart.h"

LOG_MODULE_REGISTER(meter_uart, LOG_LEVEL_INF);

#define METER_STALE_MS 15000

static const struct device *const meter_dev =
	DEVICE_DT_GET_OR_NULL(DT_ALIAS(meter_uart));

/* ISR fills a ring of raw bytes; a work item assembles lines and parses. */
RING_BUF_DECLARE(rx_ring, 512);
#include <zephyr/sys/ring_buffer.h>

static char line_buf[256];
static size_t line_len;

static struct meter_values latest;
static int64_t last_frame_ms;
static struct k_mutex lock;

static void parse_line(const char *line)
{
	cJSON *root = cJSON_Parse(line);

	if (root == NULL) {
		LOG_WRN("meter frame not JSON: %.40s", line);
		return;
	}
	k_mutex_lock(&lock, K_FOREVER);
	struct { const char *key; double *dst; } map[] = {
		{ "va", &latest.va }, { "vb", &latest.vb },
		{ "ia", &latest.ia }, { "ib", &latest.ib },
		{ "ptot", &latest.ptot }, { "kwh", &latest.kwh },
		{ "freq", &latest.freq },
	};
	for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
		const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, map[i].key);

		if (cJSON_IsNumber(v)) {
			*map[i].dst = v->valuedouble;
		}
	}
	latest.frames++;
	last_frame_ms = k_uptime_get();
	k_mutex_unlock(&lock);
	cJSON_Delete(root);
}

static void rx_work_fn(struct k_work *work)
{
	uint8_t byte;

	ARG_UNUSED(work);
	while (ring_buf_get(&rx_ring, &byte, 1) == 1) {
		if (byte == '\n' || byte == '\r') {
			if (line_len > 0) {
				line_buf[line_len] = '\0';
				parse_line(line_buf);
				line_len = 0;
			}
		} else if (line_len < sizeof(line_buf) - 1) {
			line_buf[line_len++] = (char)byte;
		} else {
			line_len = 0; /* oversized frame: resync at next newline */
		}
	}
}

static K_WORK_DEFINE(rx_work, rx_work_fn);

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t buf[32];
	int n;

	ARG_UNUSED(user_data);
	if (!uart_irq_update(dev)) {
		return;
	}
	while (uart_irq_rx_ready(dev)) {
		n = uart_fifo_read(dev, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		(void)ring_buf_put(&rx_ring, buf, n);
	}
	k_work_submit(&rx_work);
}

int meter_uart_start(void)
{
	if (meter_dev == NULL || !device_is_ready(meter_dev)) {
		LOG_WRN("meter UART absent; running without a meter");
		return -ENODEV;
	}
	k_mutex_init(&lock);
	uart_irq_callback_user_data_set(meter_dev, uart_isr, NULL);
	uart_irq_rx_enable(meter_dev);
	LOG_INF("meter ingest listening on %s (JSON lines, 115200 8N1)",
		meter_dev->name);
	return 0;
}

bool meter_uart_get(struct meter_values *out)
{
	bool any;

	k_mutex_lock(&lock, K_FOREVER);
	*out = latest;
	out->online = (latest.frames > 0) &&
		      (k_uptime_get() - last_frame_ms) < METER_STALE_MS;
	any = latest.frames > 0;
	k_mutex_unlock(&lock);
	return any;
}
