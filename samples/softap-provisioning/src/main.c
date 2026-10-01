/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * IOTCONNECT Soft-AP provisioning demo.
 *
 * Out-of-box onboarding with no serial console and no toolchain: an
 * unprovisioned device raises its own Wi-Fi access point and serves a
 * one-page portal (http://192.168.4.1) where a phone provisions the home
 * Wi-Fi, generates the device identity on-chip, and pastes the
 * iotcDeviceConfig.json -- then the device reboots and runs the normal
 * quickstart telemetry loop as a station.
 *
 * Provisioned already (Wi-Fi credentials AND identity in flash)? The portal
 * never starts; the device behaves exactly like the quickstart.
 */

#include <stdbool.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/wifi_credentials.h>

#include "iotconnect.h"
#include "iotcl.h"
#include "iotcl_telemetry.h"
#include "iotconnect_identity.h"
#include "iotconnect_ca_roots.h"
#include "iotc_time.h"
#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
#include "iotconnect_vitals.h"
#endif

#include "portal.h"

LOG_MODULE_REGISTER(softap_prov, LOG_LEVEL_INF);

/* --- Network bring-up (station path) -------------------------------------- */
static K_SEM_DEFINE(l4_connected_sem, 0, 1);
static struct net_mgmt_event_callback l4_cb;
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		k_sem_give(&l4_connected_sem);
	}
}

static int network_up(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		return -ENODEV;
	}
	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);
	if (!net_if_is_up(iface)) {
		int ret = net_if_up(iface);

		if (ret && ret != -EALREADY) {
			return ret;
		}
	}
	conn_mgr_mon_resend_status();
	LOG_INF("Waiting for network connectivity...");

	/* Field fallback: if the stored network cannot be joined (moved
	 * device, replaced router, changed passphrase), reopen the setup
	 * portal instead of retrying forever. Identity is untouched, so the
	 * portal only needs the Wi-Fi step. */
	if (k_sem_take(&l4_connected_sem,
		       K_SECONDS(CONFIG_SOFTAP_PROV_WIFI_FALLBACK_TIMEOUT)) != 0) {
		LOG_WRN("No connectivity after %d s -- reopening the setup portal",
			CONFIG_SOFTAP_PROV_WIFI_FALLBACK_TIMEOUT);
		return -ETIMEDOUT;
	}
	return 0;
}

int main(void)
{
	struct iotc_identity id;
	bool have_identity, have_wifi;
	int ret;

	printk("\nIOTCONNECT Soft-AP provisioning demo\n");

	have_identity = (iotc_identity_load(&id) == 0);
	have_wifi = !wifi_credentials_is_empty();

	if (!have_identity || !have_wifi) {
		LOG_INF("Unprovisioned (%s%s%s missing) -- starting setup portal",
			have_wifi ? "" : "Wi-Fi",
			(!have_wifi && !have_identity) ? " + " : "",
			have_identity ? "" : "identity");
		(void)portal_run();
		return 0; /* portal only returns on bring-up failure */
	}

	LOG_INF("Provisioned as duid=%s -- bringing up network", id.duid);
	ret = network_up();
	if (ret == -ETIMEDOUT) {
		/* Stored network unreachable: offer the portal so the Wi-Fi
		 * can be changed from a phone; everything else is kept. */
		(void)portal_run();
		return 0;
	}
	if (ret != 0) {
		LOG_ERR("network did not come up");
		return 0;
	}
	for (int attempt = 1;; attempt++) {
		ret = iotc_time_sync(CONFIG_IOTCONNECT_SNTP_SERVER,
				     CONFIG_IOTCONNECT_SNTP_TIMEOUT_MS);
		if (ret == 0) {
			break;
		}
		if (attempt == 10) {
			LOG_ERR("SNTP sync failed (%d)", ret);
			return 0;
		}
		k_sleep(K_SECONDS(3));
	}

	IotConnectClientConfig config;

	iotconnect_sdk_init_config(&config);
	config.connection_type = IOTC_CT_AWS;
	config.cpid = (char *)id.cpid;
	config.env = (char *)id.env;
	config.duid = (char *)id.duid;
	config.auth_info.type = IOTC_AT_X509;
	config.auth_info.ca_cert = broker_ca_pem; /* public roots, compiled in */
	config.auth_info.ca_cert_len = sizeof(broker_ca_pem);
	config.auth_info.dra_ca = dra_ca_pem;
	config.auth_info.dra_ca_len = sizeof(dra_ca_pem);
	config.auth_info.data.cert_info.device_cert = id.device_cert;
	config.auth_info.data.cert_info.device_cert_len = id.device_cert_len;
	config.auth_info.data.cert_info.device_key = id.device_key;
	config.auth_info.data.cert_info.device_key_len = id.device_key_len;
	config.verbose = true;

	ret = iotconnect_sdk_init(&config);
	if (ret) {
		LOG_ERR("iotconnect_sdk_init failed (%d)", ret);
		return 0;
	}

	while (true) {
		if (iotconnect_sdk_connect() != 0) {
			k_sleep(K_SECONDS(5));
			continue;
		}
		while (iotconnect_sdk_is_connected()) {
			IotclMessageHandle msg = iotcl_telemetry_create();

			if (msg != NULL) {
				iotcl_telemetry_set_number(msg, "random",
							   (double)(sys_rand32_get() % 100));
				iotcl_telemetry_set_string(msg, "version", "1.0.0");
#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
				iotc_vitals_append(msg);
#endif
				(void)iotcl_mqtt_send_telemetry(msg, false);
				iotcl_telemetry_destroy(msg);
			}
			k_sleep(K_SECONDS(10));
		}
		iotconnect_sdk_disconnect();
		LOG_WRN("Disconnected; reconnecting...");
	}
	return 0;
}
