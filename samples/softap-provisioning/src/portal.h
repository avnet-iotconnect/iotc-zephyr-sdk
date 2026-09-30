/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Soft-AP provisioning portal: the device raises its own Wi-Fi access point
 * and serves a one-page web portal where a phone/laptop provisions the
 * home-network credentials and the IOTCONNECT identity. Completing the
 * portal reboots the device into normal station mode.
 */

#ifndef PORTAL_H
#define PORTAL_H

/*
 * Bring up the Soft-AP (SSID "IOTC-RW612-<mac4>"), the DHCPv4 server and the
 * HTTP portal, then block. The portal ends with a device reboot triggered
 * from the web page; this function only returns on bring-up failure
 * (negative errno).
 */
int portal_run(void);

#endif /* PORTAL_H */
