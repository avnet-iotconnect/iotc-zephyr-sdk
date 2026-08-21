/*
 * Copyright (c) 2026 Avnet, Inc.  SPDX-License-Identifier: MIT
 *
 * Boot-time Wi-Fi bring-up from stored credentials (CONFIG_IOTCONNECT_WIFI_AUTOCONNECT).
 *
 * Networks are provisioned at runtime through Zephyr's `wifi cred` shell and
 * persisted by the wifi_credentials settings backend, so nothing
 * network-specific is compiled into the image. This thread issues
 * NET_REQUEST_WIFI_CONNECT_STORED (which walks every stored SSID) once the
 * Wi-Fi interface exists, retries until association succeeds, and runs the
 * request again if the link later drops and the driver's own reconnect gives
 * up. NET_REQUEST_WIFI_CONNECT_STORED blocks for up to the per-SSID
 * connection timeout, which is why this is a dedicated thread and not a
 * system-workqueue item.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_credentials.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(iotc_wifi, CONFIG_IOTCONNECT_LOG_LEVEL);

#define IOTC_WIFI_POLL_IDLE  K_SECONDS(5)
#define IOTC_WIFI_POLL_RETRY K_SECONDS(10)

static void iotc_wifi_autoconnect_thread(void *a, void *b, void *c)
{
	struct net_if *iface;
	bool hinted = false;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* The Wi-Fi driver may register its interface after boot (radio
	 * firmware load); wait for it rather than assuming init order. */
	while ((iface = net_if_get_first_wifi()) == NULL) {
		k_sleep(K_SECONDS(1));
	}

	for (;;) {
		if (net_if_oper_state(iface) == NET_IF_OPER_UP) {
			/* Associated; idle and watch for a dropped link. */
			hinted = false;
			k_sleep(IOTC_WIFI_POLL_IDLE);
			continue;
		}

		if (wifi_credentials_is_empty()) {
			if (!hinted) {
				LOG_INF("No Wi-Fi credentials stored. Provision at the shell:");
				LOG_INF("  wifi cred add -s \"<ssid>\" -k 1 -p \"<passphrase>\"");
				LOG_INF("(then `wifi cred auto_connect`, or just wait)");
				hinted = true;
			}
			k_sleep(IOTC_WIFI_POLL_IDLE);
			continue;
		}

		hinted = false;
		LOG_INF("Connecting to stored Wi-Fi network(s)...");
		ret = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);

		/* The request may complete asynchronously. Re-issuing while an
		 * association is in flight aborts it, so give this attempt (and
		 * the driver's own reconnect logic) time to play out before
		 * trying again. */
		for (int i = 0; i < 30; i++) {
			if (net_if_oper_state(iface) == NET_IF_OPER_UP) {
				break;
			}
			k_sleep(K_SECONDS(1));
		}
		if (net_if_oper_state(iface) != NET_IF_OPER_UP) {
			LOG_WRN("Wi-Fi not associated yet (req=%d); retrying", ret);
			k_sleep(IOTC_WIFI_POLL_RETRY);
		}
	}
}

K_THREAD_DEFINE(iotc_wifi_autoconn, 2048, iotc_wifi_autoconnect_thread,
		NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 2000);
