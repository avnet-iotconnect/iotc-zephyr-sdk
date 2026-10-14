/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Wi-Fi credential store with oldest-entry eviction (see iotc_wifi.h).
 */

#include <string.h>

#include <zephyr/logging/log.h>

#include "iotc_wifi.h"

LOG_MODULE_REGISTER(iotc_wifi_cred, CONFIG_IOTCONNECT_LOG_LEVEL);

struct oldest_ssid {
	const char *skip;
	size_t skip_len;
	char ssid[WIFI_SSID_MAX_LEN];
	size_t ssid_len;
};

static void oldest_ssid_cb(void *ctx, const char *ssid, size_t ssid_len)
{
	struct oldest_ssid *o = ctx;

	if (o->ssid_len != 0 ||
	    (ssid_len == o->skip_len && memcmp(ssid, o->skip, ssid_len) == 0)) {
		return;
	}
	memcpy(o->ssid, ssid, ssid_len);
	o->ssid_len = ssid_len;
}

int iotc_wifi_credentials_store(struct wifi_credentials_personal *creds)
{
	int ret = wifi_credentials_set_personal_struct(creds);

	if (ret != -ENOBUFS) {
		return ret;
	}

	struct oldest_ssid o = {
		.skip = creds->header.ssid,
		.skip_len = creds->header.ssid_len,
	};

	wifi_credentials_for_each_ssid(oldest_ssid_cb, &o);
	if (o.ssid_len == 0) {
		return ret;
	}
	LOG_WRN("Credential store full; evicting \"%.*s\"",
		(int)o.ssid_len, o.ssid);
	ret = wifi_credentials_delete_by_ssid(o.ssid, o.ssid_len);
	if (ret != 0) {
		return ret;
	}
	return wifi_credentials_set_personal_struct(creds);
}
