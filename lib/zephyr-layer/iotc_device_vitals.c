/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Device "vitals" -- appends a nested "sys" object of operational metrics to any
 * IOTCONNECT telemetry message. Every metric degrades gracefully: if the target
 * lacks the backing support (e.g. no on-chip die-temp node), that field is just
 * omitted, so the same call works on every board.
 *
 * Portable pieces use stock Zephyr APIs only:
 *   CPU%      CONFIG_SCHED_THREAD_USAGE     (busy vs idle cycle deltas)
 *   heap      CONFIG_SYS_HEAP_RUNTIME_STATS (on the system heap)
 *   reset     CONFIG_HWINFO
 *   die temp  a "die-temp" devicetree alias -> SENSOR_CHAN_DIE_TEMP
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>

#include "iotcl_telemetry.h"
#include "iotconnect_vitals.h"

#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

/* Die temperature is opt-in per board: define a "die-temp" DT alias pointing at
 * the SoC's on-chip temperature sensor (and enable CONFIG_SENSOR). Resolve to a
 * plain 0/1 so it can be used in #if without expanding "defined" in a macro. */
#if defined(CONFIG_SENSOR) && DT_NODE_EXISTS(DT_ALIAS(die_temp))
#define IOTC_VITALS_HAS_DIE_TEMP 1
#else
#define IOTC_VITALS_HAS_DIE_TEMP 0
#endif

#if IOTC_VITALS_HAS_DIE_TEMP
#include <zephyr/drivers/sensor.h>
static const struct device *const s_die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp));
#endif

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && (CONFIG_HEAP_MEM_POOL_SIZE > 0)
#include <zephyr/sys/sys_heap.h>
extern struct k_heap _system_heap;   /* Zephyr system heap (k_malloc/k_free) */
#endif

#ifdef CONFIG_IOTCONNECT_FW_VERSION
#define IOTC_VITALS_FW CONFIG_IOTCONNECT_FW_VERSION
#else
#define IOTC_VITALS_FW "0.0.0"
#endif

#if defined(CONFIG_SCHED_THREAD_USAGE)
static uint64_t s_last_exec;   /* total (idle + non-idle) cycles at last sample */
static uint64_t s_last_busy;   /* non-idle cycles at last sample */
#endif

#if defined(CONFIG_HWINFO)
static const char *s_reset_cause = "unknown";
#endif

static int iotc_vitals_init(void)
{
#if defined(CONFIG_HWINFO)
	uint32_t c = 0U;

	if (hwinfo_get_reset_cause(&c) == 0) {
		if (c & RESET_WATCHDOG) {
			s_reset_cause = "watchdog";
		} else if (c & RESET_SOFTWARE) {
			s_reset_cause = "software";
		} else if (c & RESET_POR) {
			s_reset_cause = "power-on";
		} else if (c & RESET_BROWNOUT) {
			s_reset_cause = "brownout";
		} else if (c & RESET_PIN) {
			s_reset_cause = "pin";
		} else if (c & RESET_LOW_POWER_WAKE) {
			s_reset_cause = "wake";
		} else if (c & RESET_CPU_LOCKUP) {
			s_reset_cause = "lockup";
		} else if (c & RESET_DEBUG) {
			s_reset_cause = "debug";
		} else if (c != 0U) {
			s_reset_cause = "other";
		} else {
			s_reset_cause = "none";
		}
	}
#endif
	return 0;
}
SYS_INIT(iotc_vitals_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

void iotc_vitals_append(IotclMessageHandle msg)
{
	if (msg == NULL) {
		return;
	}

	/* Uptime -- always available. */
	iotcl_telemetry_set_number(msg, "sys.uptime_s",
				   (double)k_uptime_get() / 1000.0);

	/* CPU load % since the previous call (busy/total cycle delta). */
#if defined(CONFIG_SCHED_THREAD_USAGE)
	{
		k_thread_runtime_stats_t st;

		if (k_thread_runtime_stats_all_get(&st) == 0) {
			uint64_t d_exec = st.execution_cycles - s_last_exec;
			uint64_t d_busy = st.total_cycles - s_last_busy;

			s_last_exec = st.execution_cycles;
			s_last_busy = st.total_cycles;
			if (d_exec != 0U) {
				iotcl_telemetry_set_number(
					msg, "sys.cpu_pct",
					100.0 * (double)d_busy / (double)d_exec);
			}
		}
	}
#endif

	/* Core clock (MHz). */
#if defined(CONFIG_CPU_CORTEX_M)
	{
		extern uint32_t SystemCoreClock;

		iotcl_telemetry_set_number(msg, "sys.freq_mhz",
					   (double)SystemCoreClock / 1e6);
	}
#else
	iotcl_telemetry_set_number(msg, "sys.freq_mhz",
				   (double)sys_clock_hw_cycles_per_sec() / 1e6);
#endif

	/* System heap (bytes). */
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && (CONFIG_HEAP_MEM_POOL_SIZE > 0)
	{
		struct sys_memory_stats hs;

		if (sys_heap_runtime_stats_get(&_system_heap.heap, &hs) == 0) {
			iotcl_telemetry_set_number(msg, "sys.heap_used",
						   (double)hs.allocated_bytes);
			iotcl_telemetry_set_number(msg, "sys.heap_free",
						   (double)hs.free_bytes);
		}
	}
#endif

	/* On-chip die temperature (deg C) where a board wires the alias. */
#if IOTC_VITALS_HAS_DIE_TEMP
	if (device_is_ready(s_die_temp)) {
		struct sensor_value t;

		if (sensor_sample_fetch(s_die_temp) == 0 &&
		    sensor_channel_get(s_die_temp, SENSOR_CHAN_DIE_TEMP, &t) == 0) {
			iotcl_telemetry_set_number(msg, "sys.temp_c",
						   sensor_value_to_double(&t));
		}
	}
#endif

	/* Last reset cause (captured once at boot). */
#if defined(CONFIG_HWINFO)
	iotcl_telemetry_set_string(msg, "sys.reset_cause", s_reset_cause);
#endif

	/* Firmware version. */
	iotcl_telemetry_set_string(msg, "sys.fw", IOTC_VITALS_FW);
}
