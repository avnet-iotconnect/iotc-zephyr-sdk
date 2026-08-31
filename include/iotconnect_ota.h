/*
 * Copyright (c) 2026 Avnet, Inc.  SPDX-License-Identifier: MIT
 *
 * IOTCONNECT OTA -> MCUboot glue (CONFIG_IOTCONNECT_OTA_MCUBOOT).
 *
 * Usage:
 *   config.ota_cb = iotc_ota_handle;         // route platform OTA events
 *   ...connect...
 *   iotc_ota_confirm_if_pending();           // once, after MQTT is up
 *
 * iotc_ota_handle() downloads the signed image into the MCUboot secondary
 * slot over HTTPS, persists the OTA ack id, marks a TEST swap and reboots.
 * iotc_ota_confirm_if_pending() runs in the NEW firmware: it makes the image
 * permanent and completes the persisted ack with DOWNLOAD_DONE (or reports
 * DOWNLOAD_FAILED from the old firmware if MCUboot reverted).
 */

#ifndef IOTCONNECT_OTA_H
#define IOTCONNECT_OTA_H

#include "iotcl_c2d.h"

#ifdef __cplusplus
extern "C" {
#endif

void iotc_ota_handle(IotclC2dEventData data);
void iotc_ota_confirm_if_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* IOTCONNECT_OTA_H */
