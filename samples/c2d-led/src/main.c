/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * c2d-led demo -- cloud-to-device LED control over Avnet /IOTCONNECT.
 *
 * Connects with the iotc-zephyr-sdk, then drives the board LED (alias led0)
 * from C2D commands and reports the LED state back as telemetry:
 *   - command containing "toggle"  -> invert the LED
 *   - command containing "off"     -> LED off
 *   - command containing "on"      -> LED on   (checked after "off")
 * Every command is ACKed. A periodic heartbeat publishes {"led": 0|1}.
 *
 * Send a command from the IOTCONNECT device console, e.g. command name
 * "led-on" / "led-off" / "led-toggle".
 */

#include <stdbool.h>
#include <stddef.h>
#include <ctype.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/conn_mgr_monitor.h>

#include "iotconnect.h"
#include "iotcl.h"
#include "iotcl_c2d.h"
#include "iotc_time.h"
#include "device_credentials.h"   /* git-ignored; from the SDK sample's src/ */

LOG_MODULE_REGISTER(c2d_led, LOG_LEVEL_INF);

#define HEARTBEAT_SEC 10

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static int led_state;

static K_SEM_DEFINE(l4_connected_sem, 0, 1);
static struct net_mgmt_event_callback l4_cb;
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

static void set_led(int on)
{
	led_state = on ? 1 : 0;
	gpio_pin_set_dt(&led, led_state);
	LOG_INF("LED -> %s", led_state ? "ON" : "OFF");
}

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("Network connectivity established (L4 up)");
		k_sem_give(&l4_connected_sem);
	} else if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		LOG_WRN("Network connectivity lost (L4 down)");
	}
}

/* Case-insensitive substring search (picolibc may lack strcasestr). */
static bool ci_contains(const char *haystack, const char *needle)
{
	for (const char *h = haystack; *h != '\0'; h++) {
		size_t i = 0;

		while (h[i] != '\0' && needle[i] != '\0' &&
		       tolower((unsigned char)h[i]) ==
			       tolower((unsigned char)needle[i])) {
			i++;
		}
		if (needle[i] == '\0') {
			return true;
		}
	}
	return false;
}

/* --- IOTCONNECT callbacks ------------------------------------------------- */

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
		} else {
			LOG_WRN("Unrecognized LED command");
		}
	}

	if (ack != NULL) {
		(void)iotcl_mqtt_send_cmd_ack(ack, status,
			status == IOTCL_C2D_EVT_CMD_FAILED ? "unknown command" : NULL);
	}

	/* Reflect the new state immediately. */
	(void)iotconnect_sdk_send_telemetry_number("led", (double)led_state);
}

static void on_connection_status(IotConnectMqttStatus status)
{
	LOG_INF("MQTT status: %d", (int)status);
}

/* --- Network bring-up ----------------------------------------------------- */

static int network_up(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		LOG_ERR("No default network interface");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);

	if (!net_if_is_up(iface)) {
		int ret = net_if_up(iface);

		if (ret && ret != -EALREADY) {
			LOG_ERR("net_if_up failed (%d)", ret);
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
	int ret;

	LOG_INF("c2d-led demo starting");

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED gpio not ready");
		return 0;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	ret = network_up();
	if (ret) {
		return 0;
	}

	ret = iotc_time_sync(CONFIG_IOTCONNECT_SNTP_SERVER,
			     CONFIG_IOTCONNECT_SNTP_TIMEOUT_MS);
	if (ret) {
		LOG_ERR("SNTP sync failed (%d); TLS will likely fail", ret);
		return 0;
	}

	IotConnectClientConfig config;

	iotconnect_sdk_init_config(&config);
#if defined(CONFIG_IOTCONNECT_CT_AWS)
	config.connection_type = IOTC_CT_AWS;
#elif defined(CONFIG_IOTCONNECT_CT_AZURE)
	config.connection_type = IOTC_CT_AZURE;
#endif
	config.cpid = NULL;  /* fall back to Kconfig defaults */
	config.env = NULL;
	config.duid = NULL;

	config.auth_info.type = IOTC_AT_X509;
	config.auth_info.ca_cert = broker_ca_pem;
	config.auth_info.ca_cert_len = sizeof(broker_ca_pem);
	config.auth_info.dra_ca = dra_ca_pem;
	config.auth_info.dra_ca_len = sizeof(dra_ca_pem);
	config.auth_info.data.cert_info.device_cert = device_cert_pem;
	config.auth_info.data.cert_info.device_cert_len = sizeof(device_cert_pem);
	config.auth_info.data.cert_info.device_key = device_key_pem;
	config.auth_info.data.cert_info.device_key_len = sizeof(device_key_pem);

	config.status_cb = on_connection_status;
	config.cmd_cb = on_command;
	config.verbose = true;

	ret = iotconnect_sdk_init(&config);
	if (ret) {
		LOG_ERR("iotconnect_sdk_init failed (%d)", ret);
		return 0;
	}

	while (true) {
		ret = iotconnect_sdk_connect();
		if (ret) {
			LOG_ERR("connect failed (%d); retrying", ret);
			k_sleep(K_SECONDS(5));
			continue;
		}

		while (iotconnect_sdk_is_connected()) {
			(void)iotconnect_sdk_send_telemetry_number("led",
								   (double)led_state);
			k_sleep(K_SECONDS(HEARTBEAT_SEC));
		}

		iotconnect_sdk_disconnect();
		LOG_WRN("Disconnected; reconnecting...");
	}

	return 0;
}
