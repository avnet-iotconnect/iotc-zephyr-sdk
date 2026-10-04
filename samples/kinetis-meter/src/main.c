/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * IOTCONNECT Kinetis-M meter demo (FRDM-RW612).
 *
 * One firmware that exercises the full meter-host architecture:
 *   - Soft-AP web portal for Wi-Fi + cloud onboarding (no console needed)
 *   - UART ingest from an external metrology board (meter.* telemetry)
 *   - Onboard P3T1755 ambient temperature sensor
 *   - Cloud-to-device LED commands (led-on / led-off / led-toggle)
 *   - OTA firmware updates via MCUboot when built with sysbuild
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_credentials.h>

#include "iotconnect.h"
#include "iotcl.h"
#include "iotcl_c2d.h"
#include "iotcl_telemetry.h"
#include "iotconnect_ca_roots.h"
#include "iotconnect_identity.h"
#include "iotc_time.h"
#if defined(CONFIG_IOTCONNECT_OTA_MCUBOOT)
#include "iotconnect_ota.h"
#endif
#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
#include "iotconnect_vitals.h"
#endif

#include "meter_uart.h"
#include "portal.h"

LOG_MODULE_REGISTER(kinetis_meter, LOG_LEVEL_INF);

/* --- LED -------------------------------------------------------------------- */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});
static int led_state;

static void set_led(int on)
{
	if (led.port != NULL) {
		(void)gpio_pin_set_dt(&led, on);
	}
	led_state = on;
	LOG_INF("LED -> %s", on ? "ON" : "OFF");
}

static bool ci_contains(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);

	for (; *hay != '\0'; hay++) {
		size_t i;

		for (i = 0; i < nl && hay[i] != '\0' &&
			    tolower((int)hay[i]) == tolower((int)needle[i]); i++) {
		}
		if (i == nl) {
			return true;
		}
	}
	return false;
}

static void on_command(IotclC2dEventData data)
{
	const char *cmd = iotcl_c2d_get_command(data);
	const char *ack = iotcl_c2d_get_ack_id(data);
	int status = IOTCL_C2D_EVT_CMD_FAILED;

	LOG_INF("C2D command: %s", cmd ? cmd : "(null)");
	if (cmd != NULL) {
		if (ci_contains(cmd, "toggle")) {
			set_led(!led_state);
			status = IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK;
		} else if (ci_contains(cmd, "off")) {
			set_led(0);
			status = IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK;
		} else if (ci_contains(cmd, "on")) {
			set_led(1);
			status = IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK;
		}
	}
	if (ack != NULL) {
		(void)iotcl_mqtt_send_cmd_ack(ack, status, NULL);
	}
}

/* --- Onboard temperature (P3T1755, DT alias ambient-temp0) ------------------ */
static const struct device *const temp_dev =
	DEVICE_DT_GET_OR_NULL(DT_ALIAS(ambient_temp0));

static bool read_temp_c(double *out)
{
	struct sensor_value v;

	if (temp_dev == NULL || !device_is_ready(temp_dev) ||
	    sensor_sample_fetch(temp_dev) != 0 ||
	    sensor_channel_get(temp_dev, SENSOR_CHAN_AMBIENT_TEMP, &v) != 0) {
		return false;
	}
	*out = sensor_value_to_double(&v);
	return true;
}

/* --- Network bring-up -------------------------------------------------------- */
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
	int ret;

	printk("\nIOTCONNECT Kinetis-M meter demo\n");

	if (led.port != NULL) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	(void)meter_uart_start();

	if (iotc_identity_load(&id) != 0 || wifi_credentials_is_empty()) {
		LOG_INF("Unprovisioned -- starting setup portal");
		(void)portal_run();
		return 0;
	}

	LOG_INF("Provisioned as duid=%s -- bringing up network", id.duid);
	ret = network_up();
	if (ret == -ETIMEDOUT) {
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
	config.auth_info.ca_cert = broker_ca_pem;
	config.auth_info.ca_cert_len = sizeof(broker_ca_pem);
	config.auth_info.dra_ca = dra_ca_pem;
	config.auth_info.dra_ca_len = sizeof(dra_ca_pem);
	config.auth_info.data.cert_info.device_cert = id.device_cert;
	config.auth_info.data.cert_info.device_cert_len = id.device_cert_len;
	config.auth_info.data.cert_info.device_key = id.device_key;
	config.auth_info.data.cert_info.device_key_len = id.device_key_len;
	config.cmd_cb = on_command;
	config.verbose = true;
#if defined(CONFIG_IOTCONNECT_OTA_MCUBOOT)
	config.ota_cb = iotc_ota_handle;
#endif

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
#if defined(CONFIG_IOTCONNECT_OTA_MCUBOOT)
		iotc_ota_confirm_if_pending();
#endif
		while (iotconnect_sdk_is_connected()) {
			IotclMessageHandle msg = iotcl_telemetry_create();

			if (msg != NULL) {
				struct meter_values m;
				double t;

				iotcl_telemetry_set_number(msg, "led", led_state);
				if (read_temp_c(&t)) {
					iotcl_telemetry_set_number(msg, "temp_c", t);
				}
				if (meter_uart_get(&m)) {
					iotcl_telemetry_set_number(msg, "meter.va", m.va);
					iotcl_telemetry_set_number(msg, "meter.vb", m.vb);
					iotcl_telemetry_set_number(msg, "meter.ia", m.ia);
					iotcl_telemetry_set_number(msg, "meter.ib", m.ib);
					iotcl_telemetry_set_number(msg, "meter.ptot", m.ptot);
					iotcl_telemetry_set_number(msg, "meter.kwh", m.kwh);
					iotcl_telemetry_set_number(msg, "meter.freq", m.freq);
					iotcl_telemetry_set_number(msg, "meter.frames", m.frames);
					iotcl_telemetry_set_number(msg, "meter.online",
								   m.online ? 1 : 0);
				}
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
