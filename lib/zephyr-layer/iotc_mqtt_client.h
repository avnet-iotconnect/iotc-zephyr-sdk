/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Zephyr MQTT-over-TLS transport seam. Narrow header consumed by the
 * orchestrator (lib/iotconnect.c); the implementation owns a struct mqtt_client
 * (zephyr/net/mqtt.h), connects over TLS using sec tags, subscribes to the C2D
 * topic, runs the poll/keepalive loop on its own thread, and forwards inbound
 * PUBLISH payloads into iotc-c-lib's C2D processor. It also provides the
 * 2-arg publish primitive registered as iotcl_cfg.mqtt_send_cb.
 */
#ifndef IOTC_MQTT_CLIENT_H
#define IOTC_MQTT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Transport status, surfaced to the orchestrator's status forwarding. */
typedef enum {
	IOTC_MQTT_EVT_CONNECTED = 0,
	IOTC_MQTT_EVT_DISCONNECTED,
	IOTC_MQTT_EVT_SEND_FAILED,
	IOTC_MQTT_EVT_DELIVERED,
} iotc_mqtt_evt_t;

/** Status callback the orchestrator installs (maps to IotConnectMqttStatus). */
typedef void (*iotc_mqtt_status_cb_t)(iotc_mqtt_evt_t evt);

/**
 * Connect parameters derived by the orchestrator from iotcl_mqtt_get_config()
 * (host/client_id/username/sub_c2d) after DRA, plus the resolved TLS sec tags.
 * Strings are owned by the caller and must remain valid until disconnect.
 */
typedef struct {
	const char *host;        /* broker host (also TLS SNI) */
	uint16_t port;           /* CONFIG_IOTCONNECT_MQTT_PORT (8883) */
	const char *client_id;   /* iotcl_mqtt_get_config()->client_id */
	const char *username;    /* iotcl_mqtt_get_config()->username (NULL for AWS) */
	const char *sub_c2d;     /* iotcl_mqtt_get_config()->sub_c2d topic */

	const int *sec_tags;     /* {device, broker_ca} */
	size_t sec_tag_count;
	int peer_verify;         /* CONFIG_IOTCONNECT_TLS_PEER_VERIFY */
	int qos;                 /* default publish QoS */

	iotc_mqtt_status_cb_t status_cb; /* may be NULL */
} iotc_mqtt_config_t;

/**
 * Open the TLS+MQTT connection, subscribe to sub_c2d, and start the message
 * pump thread. Blocks until CONNACK (or timeout). Returns 0 on success or a
 * negative -errno / IOTCL_ERR_* style code.
 */
int iotc_mqtt_client_connect(const iotc_mqtt_config_t *cfg);

/** Graceful disconnect; stops the pump thread. */
void iotc_mqtt_client_disconnect(void);

/** Current connection state. */
bool iotc_mqtt_client_is_connected(void);

/**
 * Publish primitive. Registered as iotcl_cfg.mqtt_send_cb (2-arg form). The
 * implementation does strlen(json_str) itself. Publishes at the configured QoS.
 */
void iotc_mqtt_client_publish(const char *topic, const char *json_str);

#ifdef __cplusplus
}
#endif

#endif /* IOTC_MQTT_CLIENT_H */
