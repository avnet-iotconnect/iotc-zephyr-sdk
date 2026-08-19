/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * IOTCONNECT Zephyr quickstart.
 *
 * A ready-to-flash binary: no build toolchain, no credentials baked in. Flash
 * the .hex, open the serial console, and provision the device to your own
 * IOTCONNECT account entirely from the prompt:
 *
 *   1. iotcprov provision <duid>      -- device generates its OWN key + cert
 *   2. Register the printed cert in IOTCONNECT (Create Device, Self-Signed)
 *   3. iotc config  + paste iotcDeviceConfig.json   -- sets cpid/env/duid
 *   4. kernel reboot cold             -- connects as your device
 *
 * Only the public CA roots are compiled in (quickstart_credentials.h); the
 * per-device key is generated on-chip and stored in NVS.
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

#include "iotconnect.h"
#include "iotcl.h"
#include "iotcl_telemetry.h"
#include "iotconnect_identity.h"
#include "iotc_time.h"
#include "quickstart_credentials.h"
#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
#include "iotconnect_vitals.h"
#endif

LOG_MODULE_REGISTER(quickstart, LOG_LEVEL_INF);

static void print_guide(const char *reason)
{
	printk("\n");
	printk("========================================================\n");
	printk("  IOTCONNECT QUICKSTART -- device is not provisioned\n");
	printk("  (%s)\n", reason);
	printk("--------------------------------------------------------\n");
	printk("  1) Generate this device's identity ON the device:\n");
	printk("        iotcprov provision <your-duid>\n");
	printk("  2) In IOTCONNECT: Create Device (Self-Signed) and paste\n");
	printk("     the certificate it printed.\n");
	printk("  3) Paste the downloaded iotcDeviceConfig.json:\n");
	printk("        iotc config\n");
	printk("        { ...paste the json block... }\n");
	printk("  4) Connect:  kernel reboot cold\n");
	printk("========================================================\n\n");
}

/* --- Network bring-up ----------------------------------------------------- */
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
	k_sem_take(&l4_connected_sem, K_FOREVER);
	return 0;
}

int main(void)
{
	struct iotc_identity id;
	int ret;

	printk("\nIOTCONNECT Zephyr quickstart\n");

	/* Device identity comes entirely from NVS (provisioned at the prompt). */
	if (iotc_identity_load(&id) != 0) {
		print_guide("no identity stored in NVS");
		return 0; /* stay alive at the shell for provisioning */
	}
	LOG_INF("Provisioned as duid=%s -- bringing up network", id.duid);

	if (network_up() != 0) {
		LOG_ERR("network did not come up");
		return 0;
	}
	ret = iotc_time_sync(CONFIG_IOTCONNECT_SNTP_SERVER, CONFIG_IOTCONNECT_SNTP_TIMEOUT_MS);
	if (ret) {
		LOG_ERR("SNTP sync failed (%d)", ret);
		return 0;
	}

	IotConnectClientConfig config;

	iotconnect_sdk_init_config(&config);
	config.connection_type = IOTC_CT_AWS;
	config.cpid = (char *)id.cpid;
	config.env = (char *)id.env;
	config.duid = (char *)id.duid;
	config.auth_info.type = IOTC_AT_X509;
	config.auth_info.ca_cert = broker_ca_pem;         /* public roots, compiled in */
	config.auth_info.ca_cert_len = sizeof(broker_ca_pem);
	config.auth_info.dra_ca = dra_ca_pem;
	config.auth_info.dra_ca_len = sizeof(dra_ca_pem);
	config.auth_info.data.cert_info.device_cert = id.device_cert;     /* from NVS */
	config.auth_info.data.cert_info.device_cert_len = id.device_cert_len;
	config.auth_info.data.cert_info.device_key = id.device_key;       /* from NVS */
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
