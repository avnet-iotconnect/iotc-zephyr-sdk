/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Public API for the vendor-neutral IOTCONNECT Zephyr SDK.
 *
 * This header mirrors the IOTCONNECT generic-SDK family contract
 * (IotConnectClientConfig + the init_config/init/connect/is_connected/
 * disconnect/deinit lifecycle and the OTA/command/status callbacks) so that
 * applications port across the family with minimal change. It adds an explicit
 * iotconnect_sdk_send_telemetry() convenience; the underlying iotc-c-lib
 * telemetry builder (iotcl_telemetry_*) remains available via
 * include/iotconnect_telemetry.h for richer payloads.
 *
 * Ownership: the SDK is a global singleton. iotconnect_sdk_init() deep-copies
 * the caller's config strings, so the caller need not keep the struct alive
 * after init. iotconnect_sdk_deinit() frees them.
 */
#ifndef IOTCONNECT_H
#define IOTCONNECT_H

#include <stdbool.h>
#include <stddef.h>

/* iotc-c-lib C2D types (IotclC2dEventData + the command/OTA callback typedefs). */
#include "iotcl_c2d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Enums (family contract)
 * -------------------------------------------------------------------------- */

/** Cloud back end; selects DRA discovery builder + username handling. */
typedef enum {
	IOTC_CT_UNDEFINED = 0,
	IOTC_CT_AWS = 1,
	IOTC_CT_AZURE = 2,
} IotConnectConnectionType;

/** Device authentication scheme. X509 for AWS+Azure; symmetric key Azure-only. */
typedef enum {
	IOTC_AT_UNDEFINED = 0,
	IOTC_AT_X509 = 1,
	IOTC_AT_SYMMETRIC_KEY = 2,
} IotConnectAuthType;

/** Connection status reported to the application status callback. */
typedef enum {
	IOTC_CS_UNDEFINED = 0,
	IOTC_CS_MQTT_CONNECTED = 1,
	IOTC_CS_MQTT_DISCONNECTED = 2,
	IOTC_CS_MQTT_DELIVERED = 3,
	IOTC_CS_MQTT_SEND_FAILED = 4,
} IotConnectMqttStatus;

/* --------------------------------------------------------------------------
 * Callback typedefs
 * -------------------------------------------------------------------------- */

/* Command and OTA callbacks come straight from iotc-c-lib so app code shares
 * the exact iotcl_c2d_get_* accessors across the SDK family:
 *   typedef void (*IotclCommandCallback)(IotclC2dEventData data);
 *   typedef void (*IotclOtaCallback)(IotclC2dEventData data);
 */
typedef void (*IotConnectStatusCallback)(IotConnectMqttStatus status);

/* --------------------------------------------------------------------------
 * Configuration structs (family contract)
 * -------------------------------------------------------------------------- */

/**
 * TLS / auth material.
 *
 * On Zephyr there is no filesystem; the char* fields carry in-memory PEM blobs
 * (NUL-terminated) rather than file paths. The Zephyr device-client registers
 * them as TLS credentials under the sec tags from Kconfig. ca_cert is the cloud
 * (broker) root CA; for AWS this is Starfield G2, for Azure DigiCert G2.
 */
typedef struct {
	IotConnectAuthType type;

	/* Broker root CA, PEM. If NULL the SDK falls back to a sec tag already
	 * provisioned out-of-band (e.g. PSA protected storage). */
	const char *ca_cert;
	size_t ca_cert_len;

	/* Discovery/Identity (DRA) HTTPS root CA, PEM. Used only when
	 * CONFIG_IOTCONNECT_DRA is enabled. The discovery endpoint
	 * (discovery.iotconnect.io) is fronted by a different chain than the MQTT
	 * broker, so this is normally a distinct CA (GoDaddy/Starfield G2). It is
	 * registered under CONFIG_IOTCONNECT_SEC_TAG_DRA_CA *before* discovery runs
	 * in iotconnect_sdk_init(). If NULL the SDK assumes the DRA CA was
	 * provisioned out-of-band under that sec tag. */
	const char *dra_ca;
	size_t dra_ca_len;

	union {
		struct {
			const char *device_cert; /* PEM */
			size_t device_cert_len;
			const char *device_key;  /* PEM */
			size_t device_key_len;
		} cert_info;
		const char *symmetric_key; /* base64; Azure only */
	} data;
} IotConnectAuthInfo;

/**
 * Top-level application config. Mirror of the family IotConnectClientConfig.
 * Fields left NULL/0 fall back to their CONFIG_IOTCONNECT_* defaults.
 */
typedef struct {
	IotConnectConnectionType connection_type;
	const char *env;  /* falls back to CONFIG_IOTCONNECT_ENV */
	const char *cpid; /* falls back to CONFIG_IOTCONNECT_CPID */
	const char *duid; /* falls back to CONFIG_IOTCONNECT_DUID */
	int qos;          /* default 1 */

	IotConnectAuthInfo auth_info;

	/* iotc-c-lib callbacks: void(*)(IotclC2dEventData). NULL = ignore. */
	IotclOtaCallback ota_cb;
	IotclCommandCallback cmd_cb;
	IotConnectStatusCallback status_cb;

	bool verbose;
} IotConnectClientConfig;

/* --------------------------------------------------------------------------
 * Lifecycle (family contract: 6 functions + telemetry convenience)
 * -------------------------------------------------------------------------- */

/**
 * Zero the config and apply defaults (memset 0, qos = 1). Call FIRST.
 */
void iotconnect_sdk_init_config(IotConnectClientConfig *c);

/**
 * Validate + deep-copy config, init iotc-c-lib (iotcl_init), and (when
 * CONFIG_IOTCONNECT_DRA) run discovery/identity to resolve the broker/topics.
 * Does NOT connect. Returns 0 (IOTCL_SUCCESS) or an IOTCL_ERR_* code.
 */
int iotconnect_sdk_init(IotConnectClientConfig *c);

/**
 * Register TLS credentials and open the MQTT/TLS connection, subscribe to the
 * C2D topic, and start the message-pump thread. Returns 0 on success.
 */
int iotconnect_sdk_connect(void);

/** Report MQTT transport connection state. */
bool iotconnect_sdk_is_connected(void);

/**
 * Convenience telemetry send. Builds a single-value telemetry message and
 * publishes it via iotcl_mqtt_send_telemetry(). For multi-field/nested payloads
 * use the iotc-c-lib builder in include/iotconnect_telemetry.h. Returns 0 or
 * an IOTCL_ERR_* code.
 */
int iotconnect_sdk_send_telemetry_number(const char *path, double value);
int iotconnect_sdk_send_telemetry_string(const char *path, const char *value);

/** Graceful MQTT disconnect; stops the message pump. */
void iotconnect_sdk_disconnect(void);

/** Tear down iotc-c-lib and free copied config strings. Safe if not inited. */
void iotconnect_sdk_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* IOTCONNECT_H */
