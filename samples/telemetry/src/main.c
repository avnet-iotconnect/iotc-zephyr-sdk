/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * IOTCONNECT telemetry sample.
 *
 * Reference application for the vendor-neutral IOTCONNECT Zephyr SDK. It brings
 * up the network bearer, syncs wall-clock via SNTP (so TLS cert validity checks
 * pass), runs the SDK lifecycle (init_config -> init -> connect -> pump ->
 * disconnect -> deinit), publishes periodic telemetry, and acknowledges
 * cloud-to-device commands and (optionally) OTA jobs.
 *
 * The only board-specific seam in this file is the network bearer bring-up in
 * main(); every such spot is marked with a TODO(board) comment. Bearer + TLS
 * tuning otherwise live in boards/<board>.conf, not here. See
 * docs/porting-new-board.md.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/random/random.h>

/* IOTCONNECT public family API + telemetry convenience builder. */
#include "iotconnect.h"
#include "iotconnect_telemetry.h"
#if defined(CONFIG_IOTCONNECT_IDENTITY_NVS)
#include "iotconnect_identity.h"
#endif

/* Zephyr transport-layer wall-clock seam (SNTP -> CLOCK_REALTIME). */
#include "iotc_time.h"

LOG_MODULE_REGISTER(iotc_telemetry_sample, CONFIG_IOTCONNECT_LOG_LEVEL);

/* Reported back as a "version" telemetry field and used by the OTA handler to
 * decide whether an offered firmware image is newer than what is running. */
#define APP_VERSION "1.0.0"

/* ---------------------------------------------------------------------------
 * L4 connectivity wait (board-agnostic once an iface is up)
 * ------------------------------------------------------------------------- */

/* Signaled by the net_mgmt event handler when the bearer reaches L4. */
static K_SEM_DEFINE(l4_connected_sem, 0, 1);

static struct net_mgmt_event_callback l4_cb;

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_L4_CONNECTED:
		LOG_INF("Network connectivity established (L4 up)");
		k_sem_give(&l4_connected_sem);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_WRN("Network connectivity lost (L4 down)");
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * Placeholder TLS credentials (PEM blobs)
 *
 * TODO: REPLACE the device certificate and private key below with your
 * provisioned per-device credentials. The placeholders WILL fail the TLS
 * handshake. For secure-element / PSA storage, set
 * CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE=y, provision out-of-band, and
 * leave the device cert/key pointers NULL so the SDK references the stored tag
 * instead of re-adding RAM copies.
 *
 * Credential roles (see include/iotconnect.h, lib/zephyr-layer/iotc_tls_credentials.h):
 *   - device_cert / device_key  : mutual-TLS client identity (one sec tag).
 *   - broker_ca                 : cloud root CA verifying the MQTT broker.
 *                                 AWS  -> Starfield Services Root CA G2.
 *                                 Azure-> DigiCert Global Root G2.
 *   - DRA CA                    : GoDaddy Secure Server Certificate (G2) root
 *                                 verifying the Discovery/Identity HTTPS host.
 *
 * NOTE on the DRA CA: when CONFIG_IOTCONNECT_DRA is enabled the discovery/
 * identity endpoint is fronted by a different chain than the MQTT broker, so the
 * app supplies BOTH auth_info.ca_cert (broker CA) and auth_info.dra_ca (the
 * discovery HTTPS CA). The SDK registers the DRA CA under
 * CONFIG_IOTCONNECT_SEC_TAG_DRA_CA *before* discovery runs. If your DRA CA is
 * provisioned out-of-band under that sec tag, leave auth_info.dra_ca NULL.
 * ------------------------------------------------------------------------- */

/*
 * Real per-device credentials are generated into device_credentials.h by
 * creds/gen_creds_header.py from the IOTCONNECT device package + the AWS/GoDaddy
 * root CAs. That file defines: device_cert_pem, device_key_pem, broker_ca_pem
 * (Amazon Root CA 1 + Starfield G2), dra_ca_pem (GoDaddy Root G2).
 * It is git-ignored and must NOT be committed (contains the private key).
 */
#include "device_credentials.h"

/* ---------------------------------------------------------------------------
 * SDK callbacks
 * ------------------------------------------------------------------------- */

static void on_connection_status(IotConnectMqttStatus status)
{
	switch (status) {
	case IOTC_CS_MQTT_CONNECTED:
		LOG_INF("IOTCONNECT status: MQTT connected");
		break;
	case IOTC_CS_MQTT_DISCONNECTED:
		LOG_WRN("IOTCONNECT status: MQTT disconnected");
		break;
	case IOTC_CS_MQTT_DELIVERED:
		LOG_INF("IOTCONNECT status: message delivered");
		break;
	case IOTC_CS_MQTT_SEND_FAILED:
		LOG_ERR("IOTCONNECT status: message send failed");
		break;
	case IOTC_CS_UNDEFINED:
	default:
		LOG_WRN("IOTCONNECT status: undefined (%d)", (int)status);
		break;
	}
}

/* Command callback. iotcl_c2d_get_* return values are only valid for the
 * duration of this callback. */
static void on_command(IotclC2dEventData data)
{
	const char *cmd = iotcl_c2d_get_command(data);
	const char *ack = iotcl_c2d_get_ack_id(data);

	LOG_INF("C2D command received: %s", cmd ? cmd : "(null)");

	/* TODO: dispatch on the command string and act on it here. */

	if (ack) {
		LOG_INF("Acking command (ack_id=%s)", ack);
		(void)iotcl_mqtt_send_cmd_ack(ack,
					      IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK,
					      NULL);
	}
}

/* OTA callback. The full download/flash path is gated behind
 * CONFIG_IOTCONNECT_OTA; the baseline simply acks the offer. */
static void on_ota(IotclC2dEventData data)
{
	const char *url = iotcl_c2d_get_ota_url(data, 0);
	const char *ver = iotcl_c2d_get_ota_sw_version(data);
	const char *ack = iotcl_c2d_get_ack_id(data);

	LOG_INF("OTA offered: version=%s url=%s",
		ver ? ver : "(null)", url ? url : "(null)");

#ifdef CONFIG_IOTCONNECT_OTA
	/* TODO(ota): download from `url` into the MCUboot secondary slot via
	 * DFU, mark the image for swap, then reboot. Report progress/failure
	 * with iotcl_mqtt_send_ota_ack(ack, IOTCL_C2D_EVT_OTA_*, msg). The
	 * baseline below only acknowledges the download intent. */
#endif /* CONFIG_IOTCONNECT_OTA */

	if (ack) {
		LOG_INF("Acking OTA (ack_id=%s)", ack);
		(void)iotcl_mqtt_send_ota_ack(ack,
					      IOTCL_C2D_EVT_OTA_DOWNLOAD_DONE,
					      NULL);
	}
}

/* ---------------------------------------------------------------------------
 * Telemetry
 * ------------------------------------------------------------------------- */

static int publish_telemetry(void)
{
	IotclMessageHandle msg = iotc_telemetry_begin();

	if (!msg) {
		LOG_ERR("Failed to allocate telemetry message");
		return -ENOMEM;
	}

	/* A simple changing value plus the firmware version. */
	double value = (double)(sys_rand32_get() % 100);

	(void)iotcl_telemetry_set_number(msg, "random", value);
	(void)iotcl_telemetry_set_string(msg, "version", APP_VERSION);

	/* iotc_telemetry_send() serializes, publishes, AND destroys the handle
	 * in all cases -- no manual destroy on either path. */
	int ret = iotc_telemetry_send(msg, false);

	if (ret) {
		LOG_ERR("Telemetry send failed (%d)", ret);
	} else {
		LOG_INF("Telemetry sent: random=%d version=%s",
			(int)value, APP_VERSION);
	}

	return ret;
}

/* ---------------------------------------------------------------------------
 * Network bring-up (board-specific seam)
 * ------------------------------------------------------------------------- */

static int network_up(void)
{
	struct net_if *iface;

	/* TODO(board): interface selection is bearer-specific.
	 *   - Native IP (Ethernet / Wi-Fi):  net_if_get_default()
	 *   - Cellular over PPP (FRDM-MCXN947 reference): the PPP iface, e.g.
	 *     net_if_get_first_by_type(&NET_L2_GET_NAME(PPP)).
	 * Some bearers also need a modem power-on / dial sequence and the modem
	 * `modem` devicetree alias before the iface exists. Adjust per board. */
	iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("No network interface available");
		return -ENODEV;
	}

	/* Watch for L4 connectivity (driven by conn_mgr / NET_CONNECTION_MANAGER). */
	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);

	/* TODO(board): for bearers with CONFIG_NET_CONFIG_AUTO_INIT=n (e.g. the
	 * cellular reference board) the interface must be brought up explicitly
	 * here. For DHCP-style native bearers this may be a no-op or replaced by
	 * net_dhcpv4_start(iface). */
	if (!net_if_is_up(iface)) {
		int ret = net_if_up(iface);

		if (ret && ret != -EALREADY) {
			LOG_ERR("net_if_up failed (%d)", ret);
			return ret;
		}
	}

	/* Nudge the connectivity layer to (re)connect managed ifaces. */
	conn_mgr_mon_resend_status();

	LOG_INF("Waiting for network connectivity...");
	k_sem_take(&l4_connected_sem, K_FOREVER);

	return 0;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
	int ret;

	LOG_INF("IOTCONNECT telemetry sample starting (app v%s)", APP_VERSION);

	/* 1. Bring up the bearer and wait for L4 connectivity. */
	ret = network_up();
	if (ret) {
		LOG_ERR("Network bring-up failed (%d)", ret);
		return ret;
	}

	/* 2. Sync wall-clock before any TLS handshake (mbedTLS checks cert
	 * validity against time()). */
	ret = iotc_time_sync(CONFIG_IOTCONNECT_SNTP_SERVER,
			     CONFIG_IOTCONNECT_SNTP_TIMEOUT_MS);
	if (ret) {
		LOG_ERR("SNTP time sync failed (%d); TLS will likely fail", ret);
		return ret;
	}
	LOG_INF("Wall-clock synced (epoch=%lld)", (long long)iotc_time_now());

	/* 3. Build the SDK config. */
	IotConnectClientConfig config;

	iotconnect_sdk_init_config(&config);

#if defined(CONFIG_IOTCONNECT_CT_AWS)
	config.connection_type = IOTC_CT_AWS;
#elif defined(CONFIG_IOTCONNECT_CT_AZURE)
	config.connection_type = IOTC_CT_AZURE;
#else
	config.connection_type = IOTC_CT_UNDEFINED;
#endif

	/* X.509 mutual TLS: broker CA + device cert/key. The broker/DRA CA roots
	 * are public and always compiled-in. See the credential notes above. */
	config.auth_info.type = IOTC_AT_X509;
	config.auth_info.ca_cert = broker_ca_pem;
	config.auth_info.ca_cert_len = sizeof(broker_ca_pem);
	config.auth_info.dra_ca = dra_ca_pem;
	config.auth_info.dra_ca_len = sizeof(dra_ca_pem);

	/* NULL identity fields fall back to the CONFIG_IOTCONNECT_* defaults. */
	config.cpid = NULL;
	config.env = NULL;
	config.duid = NULL;

#if defined(CONFIG_IOTCONNECT_IDENTITY_NVS)
	/* Prefer identity provisioned into NVS (`iotc cred ...`); fall back to the
	 * compiled-in device_credentials.h when the device is not yet provisioned. */
	struct iotc_identity id;

	if (iotc_identity_load(&id) == 0) {
		LOG_INF("Using NVS-provisioned identity (duid=%s)", id.duid);
		config.cpid = (char *)id.cpid;
		config.env = (char *)id.env;
		config.duid = (char *)id.duid;
		config.auth_info.data.cert_info.device_cert = id.device_cert;
		config.auth_info.data.cert_info.device_cert_len = id.device_cert_len;
		config.auth_info.data.cert_info.device_key = id.device_key;
		config.auth_info.data.cert_info.device_key_len = id.device_key_len;
	} else
#endif
	{
		LOG_INF("Using compiled-in device credentials");
		config.auth_info.data.cert_info.device_cert = device_cert_pem;
		config.auth_info.data.cert_info.device_cert_len = sizeof(device_cert_pem);
		config.auth_info.data.cert_info.device_key = device_key_pem;
		config.auth_info.data.cert_info.device_key_len = sizeof(device_key_pem);
	}

	config.status_cb = on_connection_status;
	config.cmd_cb = on_command;
	config.ota_cb = on_ota;
	config.verbose = true;

	/* 4. Init the SDK (runs DRA discovery/identity when CONFIG_IOTCONNECT_DRA). */
	ret = iotconnect_sdk_init(&config);
	if (ret) {
		LOG_ERR("iotconnect_sdk_init failed (%d)", ret);
		return ret;
	}

	/* Total run duration; negative means run forever. */
	const int duration_min = CONFIG_APP_TELEMETRY_DURATION_MIN;
	const int64_t duration_ms = (duration_min < 0)
					    ? 0
					    : (int64_t)duration_min * 60 * 1000;

	/* 5. Connect / pump / disconnect loop. Reconnects if the link drops. */
	while (true) {
		ret = iotconnect_sdk_connect();
		if (ret) {
			LOG_ERR("iotconnect_sdk_connect failed (%d); retrying", ret);
			k_sleep(K_SECONDS(5));
			continue;
		}

		const int64_t start = k_uptime_get();

		while (iotconnect_sdk_is_connected()) {
			(void)publish_telemetry();
			k_sleep(K_SECONDS(CONFIG_APP_TELEMETRY_INTERVAL_SEC));

			if (duration_min >= 0 &&
			    (k_uptime_get() - start) >= duration_ms) {
				LOG_INF("Telemetry run duration reached");
				break;
			}
		}

		iotconnect_sdk_disconnect();

		if (duration_min >= 0) {
			break;
		}

		LOG_WRN("Disconnected; reconnecting...");
		k_sleep(K_SECONDS(5));
	}

	/* 6. Tear down. */
	iotconnect_sdk_deinit();

	LOG_INF("IOTCONNECT telemetry sample done");
	return 0;
}