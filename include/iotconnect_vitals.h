/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Device "vitals" -- a small, portable operational-telemetry sidecar.
 */
#ifndef IOTCONNECT_VITALS_H
#define IOTCONNECT_VITALS_H

#include "iotcl_telemetry.h"   /* IotclMessageHandle */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Append a nested "sys" object of device operational metrics to an existing,
 * open telemetry message (call it before you serialize/send):
 *
 *   sys.cpu_pct      CPU load % since the previous call (busy vs idle cycles)
 *   sys.freq_mhz     core clock in MHz
 *   sys.heap_used    system-heap bytes allocated
 *   sys.heap_free    system-heap bytes free
 *   sys.temp_c       on-chip die temperature (only where a board wires the
 *                    "die-temp" devicetree alias)
 *   sys.uptime_s     seconds since boot
 *   sys.reset_cause  cause of the last reset (power-on/watchdog/software/...)
 *   sys.fw           firmware version (CONFIG_IOTCONNECT_FW_VERSION)
 *
 * Every field degrades gracefully: a metric with no backing support on the
 * current target is simply omitted, so the same call works on every board.
 * No-op if @p msg is NULL.
 *
 * When CONFIG_IOTCONNECT_DEVICE_VITALS is set, the SDK's iotc_telemetry_send()
 * calls this automatically; call it yourself if you build/serialize messages
 * directly (e.g. iotcl_mqtt_send_telemetry()).
 */
void iotc_vitals_append(IotclMessageHandle msg);

#ifdef __cplusplus
}
#endif

#endif /* IOTCONNECT_VITALS_H */
