/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Thin convenience wrapper over the iotc-c-lib telemetry builder. This header
 * intentionally re-exports the canonical iotcl_telemetry_* API so application
 * code uses the exact same idiom as the rest of the IOTCONNECT SDK family:
 *
 *   IotclMessageHandle msg = iotcl_telemetry_create();
 *   iotcl_telemetry_set_number(msg, "temperature", 21.5);
 *   iotcl_telemetry_set_string(msg, "status", "ok");
 *   iotcl_mqtt_send_telemetry(msg, false);
 *   iotcl_telemetry_destroy(msg);
 *
 * The only value added here is a couple of guarded helpers for the common
 * single-message case so callers do not have to remember the create/destroy
 * dance. These wrap the iotc-c-lib functions verbatim; there is no separate
 * serialization or transport here -- iotcl_mqtt_send_telemetry routes through
 * the SDK's registered mqtt_send_cb.
 */
#ifndef IOTCONNECT_TELEMETRY_H
#define IOTCONNECT_TELEMETRY_H

#include <stdbool.h>

/* Canonical builder + send API (handles, set_*, create/destroy, send). */
#include "iotcl_telemetry.h"
#include "iotcl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Begin a telemetry message. Equivalent to iotcl_telemetry_create(); returns
 * NULL on OOM. Caller must finish with iotc_telemetry_send() (which destroys
 * the handle) or iotcl_telemetry_destroy() on the error path.
 */
IotclMessageHandle iotc_telemetry_begin(void);

/**
 * Serialize + publish the message to the reporting topic via the SDK's
 * registered transport, then DESTROY the handle (so the caller never leaks it).
 * Wraps iotcl_mqtt_send_telemetry() followed by iotcl_telemetry_destroy().
 * Returns 0 (IOTCL_SUCCESS) or an IOTCL_ERR_* code; the handle is destroyed in
 * all cases.
 */
int iotc_telemetry_send(IotclMessageHandle msg, bool pretty);

#ifdef __cplusplus
}
#endif

#endif /* IOTCONNECT_TELEMETRY_H */
