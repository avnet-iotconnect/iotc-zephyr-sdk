/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * IOTCONNECT Zephyr SDK lifecycle orchestrator.
 *
 * Vendor-neutral glue between the public family API (include/iotconnect.h +
 * include/iotconnect_telemetry.h) and the portable iotc-c-lib protocol core,
 * driving the Zephyr transport seam (lib/zephyr-layer/*). This file owns a
 * single static IotConnectClientConfig copy whose strings are deep-copied at
 * init() (via iotcl_strdup) and freed at deinit().
 *
 * Flow:
 *   init_config -> init (validate + iotcl_init + DRA discovery/identity)
 *               -> connect (register TLS creds + MQTT/TLS connect + subscribe)
 *               -> send_telemetry_* / iotc_telemetry_begin/send
 *               -> disconnect -> deinit
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "iotconnect.h"
#include "iotconnect_telemetry.h"
#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
#include "iotconnect_vitals.h"
#endif

/* iotc-c-lib core */
#include "iotcl.h"
#include "iotcl_c2d.h"
#include "iotcl_cfg.h"
#include "iotcl_telemetry.h"
#include "iotcl_util.h"

/* Zephyr transport seam */
#include "iotc_mqtt_client.h"
#include "iotc_tls_credentials.h"
#if defined(CONFIG_IOTCONNECT_DRA)
#include "iotc_dra_client.h"
#endif

LOG_MODULE_REGISTER(iotconnect, CONFIG_IOTCONNECT_LOG_LEVEL);

/* --------------------------------------------------------------------------
 * Singleton state
 * -------------------------------------------------------------------------- */

/*
 * The SDK is a global singleton. We keep a deep copy of the caller's config so
 * the caller need not keep their struct (or the strings it points at) alive
 * after iotconnect_sdk_init() returns. The copied strings are owned here and
 * freed in iotconnect_sdk_deinit().
 */
static IotConnectClientConfig s_config;
static bool s_inited;

/*
 * Sec tags for the MQTT/TLS connection, in {device, broker_ca} order. Kept in
 * static storage because iotc_mqtt_client_connect() borrows the pointer for the
 * lifetime of the connection.
 */
static const int s_mqtt_sec_tags[] = {
	CONFIG_IOTCONNECT_SEC_TAG_DEVICE,
	CONFIG_IOTCONNECT_SEC_TAG_BROKER_CA,
};

/* TLS credential descriptor, retained so deinit() can clear what we registered. */
static iotc_tls_creds_t s_creds;
static bool s_creds_registered;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static const char *value_or_default(const char *value, const char *fallback)
{
	if (value != NULL && value[0] != '\0') {
		return value;
	}
	return fallback;
}

/* Deep-copy a string into SDK-owned storage. Returns NULL for NULL/empty in. */
static char *dup_str(const char *src)
{
	if (src == NULL || src[0] == '\0') {
		return NULL;
	}
	return iotcl_strdup(src);
}

static void free_config_strings(void)
{
	iotcl_free((void *)s_config.env);
	iotcl_free((void *)s_config.cpid);
	iotcl_free((void *)s_config.duid);

	iotcl_free((void *)s_config.auth_info.ca_cert);
	iotcl_free((void *)s_config.auth_info.dra_ca);
	iotcl_free((void *)s_config.auth_info.data.cert_info.device_cert);
	iotcl_free((void *)s_config.auth_info.data.cert_info.device_key);
	/* symmetric_key aliases device_cert in the union; do not double free. */

	s_config.env = NULL;
	s_config.cpid = NULL;
	s_config.duid = NULL;
	s_config.auth_info.ca_cert = NULL;
	s_config.auth_info.dra_ca = NULL;
	s_config.auth_info.data.cert_info.device_cert = NULL;
	s_config.auth_info.data.cert_info.device_key = NULL;
}

/* Translate the transport-layer event into the family status enum + forward. */
static void mqtt_status_forwarder(iotc_mqtt_evt_t evt)
{
	IotConnectMqttStatus status;

	switch (evt) {
	case IOTC_MQTT_EVT_CONNECTED:
		status = IOTC_CS_MQTT_CONNECTED;
		break;
	case IOTC_MQTT_EVT_DISCONNECTED:
		status = IOTC_CS_MQTT_DISCONNECTED;
		break;
	case IOTC_MQTT_EVT_DELIVERED:
		status = IOTC_CS_MQTT_DELIVERED;
		break;
	case IOTC_MQTT_EVT_SEND_FAILED:
		status = IOTC_CS_MQTT_SEND_FAILED;
		break;
	default:
		status = IOTC_CS_UNDEFINED;
		break;
	}

	if (s_config.status_cb != NULL) {
		s_config.status_cb(status);
	}
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

void iotconnect_sdk_init_config(IotConnectClientConfig *c)
{
	if (c == NULL) {
		return;
	}

	memset(c, 0, sizeof(*c));
	c->qos = 1; /* mirror the family default */
}

int iotconnect_sdk_init(IotConnectClientConfig *c)
{
	int ret;

	if (c == NULL) {
		IOTCL_ERROR(IOTCL_ERR_MISSING_VALUE, "init: NULL config");
		return IOTCL_ERR_MISSING_VALUE;
	}

	/* Validate the unsupported combination up front: symmetric key auth is
	 * an Azure-only scheme and is not valid against AWS. */
	if (c->auth_info.type == IOTC_AT_SYMMETRIC_KEY &&
	    c->connection_type == IOTC_CT_AWS) {
		IOTCL_ERROR(IOTCL_ERR_CONFIG_ERROR,
			    "init: symmetric key auth is not supported with AWS");
		return IOTCL_ERR_CONFIG_ERROR;
	}

	/* Redirect iotc-c-lib (and its cJSON) allocations onto the Zephyr heap.
	 * MUST happen before any other library call (it also rehooks cJSON). */
	iotcl_configure_dynamic_memory(k_malloc, k_free);

	/* Resolve identity from the runtime config, falling back to Kconfig. */
	const char *cpid = value_or_default(c->cpid, CONFIG_IOTCONNECT_CPID);
	const char *env = value_or_default(c->env, CONFIG_IOTCONNECT_ENV);
	const char *duid = value_or_default(c->duid, CONFIG_IOTCONNECT_DUID);

	if (cpid == NULL || cpid[0] == '\0' || duid == NULL || duid[0] == '\0') {
		IOTCL_ERROR(IOTCL_ERR_MISSING_VALUE,
			    "init: CPID and DUID are required");
		return IOTCL_ERR_MISSING_VALUE;
	}

	/* Deep-copy the caller's config so we no longer reference their memory. */
	memset(&s_config, 0, sizeof(s_config));
	s_config.connection_type = c->connection_type;
	s_config.qos = (c->qos != 0) ? c->qos : 1;
	s_config.ota_cb = c->ota_cb;
	s_config.cmd_cb = c->cmd_cb;
	s_config.status_cb = c->status_cb;
	s_config.verbose = c->verbose;

	s_config.cpid = dup_str(cpid);
	s_config.env = dup_str(env);
	s_config.duid = dup_str(duid);

	s_config.auth_info.type = c->auth_info.type;
	s_config.auth_info.ca_cert = dup_str(c->auth_info.ca_cert);
	s_config.auth_info.ca_cert_len =
		(s_config.auth_info.ca_cert != NULL)
			? strlen(s_config.auth_info.ca_cert) + 1
			: 0;

	s_config.auth_info.dra_ca = dup_str(c->auth_info.dra_ca);
	s_config.auth_info.dra_ca_len =
		(s_config.auth_info.dra_ca != NULL)
			? strlen(s_config.auth_info.dra_ca) + 1
			: 0;

	if (c->auth_info.type == IOTC_AT_SYMMETRIC_KEY) {
		s_config.auth_info.data.symmetric_key =
			dup_str(c->auth_info.data.symmetric_key);
	} else {
		s_config.auth_info.data.cert_info.device_cert =
			dup_str(c->auth_info.data.cert_info.device_cert);
		s_config.auth_info.data.cert_info.device_cert_len =
			(s_config.auth_info.data.cert_info.device_cert != NULL)
				? strlen(s_config.auth_info.data.cert_info
						 .device_cert) +
					  1
				: 0;
		s_config.auth_info.data.cert_info.device_key =
			dup_str(c->auth_info.data.cert_info.device_key);
		s_config.auth_info.data.cert_info.device_key_len =
			(s_config.auth_info.data.cert_info.device_key != NULL)
				? strlen(s_config.auth_info.data.cert_info
						 .device_key) +
					  1
				: 0;
	}

	if (s_config.cpid == NULL || s_config.duid == NULL) {
		IOTCL_ERROR(IOTCL_ERR_OUT_OF_MEMORY,
			    "init: failed to copy device identity");
		free_config_strings();
		return IOTCL_ERR_OUT_OF_MEMORY;
	}

	/* Configure + init the iotc-c-lib core in CUSTOM mode. CUSTOM is used
	 * because DRA discovery/identity populates the MQTT config at runtime;
	 * topics are not built from a template here. */
	IotclClientConfig cfg;
	iotcl_init_client_config(&cfg);
	cfg.device.instance_type = IOTCL_DCT_CUSTOM;
	cfg.device.cpid = s_config.cpid;
	cfg.device.duid = s_config.duid;
	cfg.mqtt_send_cb = iotc_mqtt_client_publish;
	cfg.events.cmd_cb = s_config.cmd_cb;
	cfg.events.ota_cb = s_config.ota_cb;
	cfg.time_fn = iotcl_default_time;

	ret = iotcl_init(&cfg);
	if (ret != IOTCL_SUCCESS) {
		IOTCL_ERROR(ret, "init: iotcl_init failed");
		free_config_strings();
		return ret;
	}

#if defined(CONFIG_IOTCONNECT_DRA)
	/* The discovery/identity HTTPS GET below verifies the server against
	 * CONFIG_IOTCONNECT_SEC_TAG_DRA_CA, so the DRA CA MUST be registered first
	 * (the broker CA + device creds are registered later, in connect()). A NULL
	 * dra_ca means it was provisioned out-of-band; creds_add() then skips it. */
	memset(&s_creds, 0, sizeof(s_creds));
	s_creds.dra_ca = s_config.auth_info.dra_ca;
	s_creds.dra_ca_len = s_config.auth_info.dra_ca_len;
	s_creds.dra_ca_sec_tag = CONFIG_IOTCONNECT_SEC_TAG_DRA_CA;

	ret = iotc_tls_credentials_register(&s_creds);
	if (ret != 0) {
		IOTCL_ERROR(IOTCL_ERR_FAILED,
			    "init: DRA CA registration failed (%d)", ret);
		iotcl_deinit();
		free_config_strings();
		return ret;
	}
	s_creds_registered = true;

	/* Runtime broker discovery: resolves host/client_id/username/topics
	 * into the library's MQTT config, read later via iotcl_mqtt_get_config(). */
	iotc_dra_config_t dra = {
		.platform = (s_config.connection_type == IOTC_CT_AZURE)
				    ? IOTC_DRA_PF_AZURE
				    : IOTC_DRA_PF_AWS,
		.discovery_host = CONFIG_IOTCONNECT_DRA_DISCOVERY_HOST,
		.cpid = s_config.cpid,
		.env = s_config.env,
		.duid = s_config.duid,
		.dra_ca_sec_tag = CONFIG_IOTCONNECT_SEC_TAG_DRA_CA,
		.timeout_ms = CONFIG_IOTCONNECT_DRA_HTTP_TIMEOUT_MS,
	};

	ret = iotc_dra_run(&dra);
	if (ret != IOTCL_SUCCESS) {
		IOTCL_ERROR(ret, "init: DRA discovery/identity failed");
		iotcl_deinit();
		free_config_strings();
		return ret;
	}

	/* AWS username workaround (carried from the SDK family): the identity
	 * response can carry a username for AWS that the broker rejects on
	 * CONNECT. AWS uses mutual-TLS auth and expects no username, so drop it. */
	if (s_config.connection_type == IOTC_CT_AWS) {
		IotclMqttConfig *mc = iotcl_mqtt_get_config();

		if (mc != NULL && mc->username != NULL) {
			iotcl_free(mc->username);
			mc->username = NULL;
		}
	}
#endif /* CONFIG_IOTCONNECT_DRA */

	s_inited = true;
	IOTCL_INFO("IOTCONNECT SDK initialized (cpid=%s duid=%s)", s_config.cpid,
		   s_config.duid);
	return IOTCL_SUCCESS;
}

int iotconnect_sdk_connect(void)
{
	int ret;

	if (!s_inited) {
		IOTCL_ERROR(IOTCL_ERR_CONFIG_MISSING,
			    "connect: SDK not initialized");
		return IOTCL_ERR_CONFIG_MISSING;
	}

	IotclMqttConfig *mc = iotcl_mqtt_get_config();

	if (mc == NULL || mc->host == NULL || mc->client_id == NULL) {
		IOTCL_ERROR(IOTCL_ERR_CONFIG_MISSING,
			    "connect: MQTT config not resolved (run init/DRA first)");
		return IOTCL_ERR_CONFIG_MISSING;
	}

	/* Register the broker CA + device credentials under the Kconfig sec tags.
	 * NB: do NOT memset s_creds here -- init() may already have registered the
	 * DRA CA into it, and tls_credential_add() is idempotent (re-registering
	 * the DRA slot is harmless). When the device cert/key live in PSA Protected
	 * Storage they are provisioned out-of-band, so we skip RAM copies of them. */
	s_creds.broker_ca = s_config.auth_info.ca_cert;
	s_creds.broker_ca_len = s_config.auth_info.ca_cert_len;
	s_creds.broker_ca_sec_tag = CONFIG_IOTCONNECT_SEC_TAG_BROKER_CA;
	s_creds.device_sec_tag = CONFIG_IOTCONNECT_SEC_TAG_DEVICE;

#if !defined(CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE)
	if (s_config.auth_info.type != IOTC_AT_SYMMETRIC_KEY) {
		s_creds.device_cert =
			s_config.auth_info.data.cert_info.device_cert;
		s_creds.device_cert_len =
			s_config.auth_info.data.cert_info.device_cert_len;
		s_creds.device_key =
			s_config.auth_info.data.cert_info.device_key;
		s_creds.device_key_len =
			s_config.auth_info.data.cert_info.device_key_len;
	}
#endif

	ret = iotc_tls_credentials_register(&s_creds);
	if (ret != 0) {
		IOTCL_ERROR(IOTCL_ERR_FAILED,
			    "connect: TLS credential registration failed (%d)",
			    ret);
		return ret;
	}
	s_creds_registered = true;

	const iotc_mqtt_config_t mqtt_cfg = {
		.host = mc->host,
		.port = (uint16_t)CONFIG_IOTCONNECT_MQTT_PORT,
		.client_id = mc->client_id,
		.username = mc->username,
		.sub_c2d = mc->sub_c2d,
		.sec_tags = s_mqtt_sec_tags,
		.sec_tag_count = ARRAY_SIZE(s_mqtt_sec_tags),
		.peer_verify = CONFIG_IOTCONNECT_TLS_PEER_VERIFY,
		.qos = s_config.qos,
		.status_cb = mqtt_status_forwarder,
	};

	ret = iotc_mqtt_client_connect(&mqtt_cfg);
	if (ret != 0) {
		IOTCL_ERROR(IOTCL_ERR_FAILED, "connect: MQTT connect failed (%d)",
			    ret);
		return ret;
	}

	IOTCL_INFO("IOTCONNECT connected to %s:%d", mc->host,
		   (int)CONFIG_IOTCONNECT_MQTT_PORT);
	return IOTCL_SUCCESS;
}

bool iotconnect_sdk_is_connected(void)
{
	return iotc_mqtt_client_is_connected();
}

void iotconnect_sdk_disconnect(void)
{
	iotc_mqtt_client_disconnect();
}

void iotconnect_sdk_deinit(void)
{
	if (!s_inited) {
		return;
	}

	iotcl_deinit();

	if (s_creds_registered) {
		iotc_tls_credentials_clear(&s_creds);
		s_creds_registered = false;
		memset(&s_creds, 0, sizeof(s_creds));
	}

	free_config_strings();
	memset(&s_config, 0, sizeof(s_config));
	s_inited = false;
}

/* --------------------------------------------------------------------------
 * Telemetry convenience (single-value)
 * -------------------------------------------------------------------------- */

int iotconnect_sdk_send_telemetry_number(const char *path, double value)
{
	int ret;

	if (path == NULL) {
		return IOTCL_ERR_MISSING_VALUE;
	}

	IotclMessageHandle msg = iotcl_telemetry_create();

	if (msg == NULL) {
		IOTCL_ERROR(IOTCL_ERR_OUT_OF_MEMORY,
			    "send_telemetry_number: create failed");
		return IOTCL_ERR_OUT_OF_MEMORY;
	}

	ret = iotcl_telemetry_set_number(msg, path, value);
	if (ret == IOTCL_SUCCESS) {
#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
		iotc_vitals_append(msg);
#endif
		ret = iotcl_mqtt_send_telemetry(msg, false);
	}

	iotcl_telemetry_destroy(msg);
	return ret;
}

int iotconnect_sdk_send_telemetry_string(const char *path, const char *value)
{
	int ret;

	if (path == NULL || value == NULL) {
		return IOTCL_ERR_MISSING_VALUE;
	}

	IotclMessageHandle msg = iotcl_telemetry_create();

	if (msg == NULL) {
		IOTCL_ERROR(IOTCL_ERR_OUT_OF_MEMORY,
			    "send_telemetry_string: create failed");
		return IOTCL_ERR_OUT_OF_MEMORY;
	}

	ret = iotcl_telemetry_set_string(msg, path, value);
	if (ret == IOTCL_SUCCESS) {
#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
		iotc_vitals_append(msg);
#endif
		ret = iotcl_mqtt_send_telemetry(msg, false);
	}

	iotcl_telemetry_destroy(msg);
	return ret;
}

/* --------------------------------------------------------------------------
 * Telemetry builder helpers (include/iotconnect_telemetry.h)
 * -------------------------------------------------------------------------- */

IotclMessageHandle iotc_telemetry_begin(void)
{
	return iotcl_telemetry_create();
}

int iotc_telemetry_send(IotclMessageHandle msg, bool pretty)
{
	int ret;

	if (msg == NULL) {
		return IOTCL_ERR_MISSING_VALUE;
	}

#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
	/* Ride a "sys" object of device vitals along with every telemetry send. */
	iotc_vitals_append(msg);
#endif

	ret = iotcl_mqtt_send_telemetry(msg, pretty);

	/* Destroy the handle in all cases so the caller never leaks it. */
	iotcl_telemetry_destroy(msg);
	return ret;
}
