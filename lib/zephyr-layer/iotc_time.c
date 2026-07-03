/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Wall-clock seam implementation. Obtains UTC over SNTP
 * (zephyr/net/sntp.h) and applies it to the POSIX realtime clock via
 * clock_settime(CLOCK_REALTIME, ...). mbedTLS reads time() during certificate
 * validity checks, so the clock MUST be set before the first TLS handshake.
 *
 * Ordering contract (important):
 *   1. Bring up the network bearer and wait for L4 connectivity.
 *   2. Call iotc_time_sync() -> sets CLOCK_REALTIME from SNTP.
 *   3. ONLY THEN perform any TLS handshake (DRA HTTPS GET / MQTT CONNECT).
 * If the clock is left at the epoch, mbedTLS will reject otherwise-valid
 * server certificates as "not yet valid".
 *
 * clock_settime() requires CONFIG_POSIX_TIMERS, which CONFIG_IOTCONNECT
 * selects (see Kconfig).
 */

#include "iotc_time.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/net/sntp.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(iotc_time, CONFIG_IOTCONNECT_LOG_LEVEL);

/* Set true once an SNTP sync has succeeded at least once this boot. */
static bool s_time_synced;

int iotc_time_sync(const char *sntp_server, uint32_t timeout_ms)
{
	struct sntp_time st;
	int rc;

	if (sntp_server == NULL) {
		LOG_ERR("SNTP server is NULL");
		return -EINVAL;
	}

	LOG_INF("Synchronizing wall-clock via SNTP server '%s' (timeout %u ms)",
		sntp_server, timeout_ms);

	/* sntp_simple() resolves the server, performs a single request and
	 * blocks up to timeout_ms. Returns 0 on success or a negative -errno.
	 */
	rc = sntp_simple(sntp_server, timeout_ms, &st);
	if (rc != 0) {
		LOG_ERR("SNTP sync failed (%d)", rc);
		return rc;
	}

	struct timespec ts = {
		.tv_sec = (time_t)st.seconds,
		.tv_nsec = 0,
	};

	rc = clock_settime(CLOCK_REALTIME, &ts);
	if (rc != 0) {
		/* clock_settime() reports failure via errno; surface a negative
		 * code so callers see a consistent negative-on-error contract.
		 */
		int err = (errno != 0) ? -errno : -EIO;

		LOG_ERR("clock_settime(CLOCK_REALTIME) failed (rc=%d errno=%d)",
			rc, errno);
		return err;
	}

	/* TODO(board): if this board has a battery-backed RTC, the SNTP step
	 * above can be skipped (or used only to seed/correct the RTC). Provide
	 * an RTC-backed alternative here that sets s_time_synced from the RTC's
	 * valid-time state instead of from a network round-trip.
	 */
	s_time_synced = true;

	LOG_INF("Wall-clock set to UTC epoch %lld", (long long)st.seconds);
	return 0;
}

bool iotc_time_is_synced(void)
{
	return s_time_synced;
}

int64_t iotc_time_now(void)
{
	/* Wraps time(); returns 0 (the epoch) if the clock was never set. */
	return (int64_t)time(NULL);
}
