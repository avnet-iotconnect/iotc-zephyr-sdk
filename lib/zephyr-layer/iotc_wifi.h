/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Wi-Fi credential helpers (CONFIG_WIFI_CREDENTIALS).
 */

#ifndef IOTC_WIFI_H
#define IOTC_WIFI_H

#include <zephyr/net/wifi_credentials.h>

/* Store a network. When the credential store is full
 * (CONFIG_WIFI_CREDENTIALS_MAX_ENTRIES), the oldest stored network
 * (never the one being stored) is evicted and the store retried, so
 * reprovisioning can never dead-end on "no space left". */
int iotc_wifi_credentials_store(struct wifi_credentials_personal *creds);

#endif /* IOTC_WIFI_H */
