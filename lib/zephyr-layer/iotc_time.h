/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Wall-clock seam. Uses SNTP (zephyr/net/sntp.h) to obtain UTC and applies it
 * to the POSIX realtime clock (clock_settime), which mbedTLS reads via time()
 * during certificate validity checks. Must run after L4 connectivity and
 * before the first TLS handshake.
 */
#ifndef IOTC_TIME_H
#define IOTC_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One-shot SNTP sync: query the server and set CLOCK_REALTIME. Returns 0 on
 * success or a negative -errno. On success the system clock holds UTC.
 */
int iotc_time_sync(const char *sntp_server, uint32_t timeout_ms);

/** True once iotc_time_sync() has succeeded at least once this boot. */
bool iotc_time_is_synced(void);

/** Current UTC epoch seconds (0 if never synced). Wraps time(). */
int64_t iotc_time_now(void);

#ifdef __cplusplus
}
#endif

#endif /* IOTC_TIME_H */
