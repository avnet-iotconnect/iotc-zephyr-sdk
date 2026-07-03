/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Device REST API (DRA) HTTPS client seam. Performs the two-step
 * Discovery -> Identity GET sequence over a Zephyr TLS socket using the HTTP
 * client (zephyr/net/http/client.h), handing raw response bodies to the
 * iotc-c-lib DRA parsers which populate the library's MQTT config. After a
 * successful run the orchestrator reads connection params via
 * iotcl_mqtt_get_config().
 *
 * Compiled only when CONFIG_IOTCONNECT_DRA=y.
 */
#ifndef IOTC_DRA_CLIENT_H
#define IOTC_DRA_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/** Platform string for the discovery URL builder. */
typedef enum {
	IOTC_DRA_PF_AWS = 0,
	IOTC_DRA_PF_AZURE,
} iotc_dra_platform_t;

/**
 * Parameters for the DRA bootstrap. cpid/env/duid identify the device;
 * dra_ca_sec_tag verifies the HTTPS endpoint (GoDaddy G2).
 */
typedef struct {
	iotc_dra_platform_t platform;
	const char *discovery_host; /* CONFIG_IOTCONNECT_DRA_DISCOVERY_HOST */
	const char *cpid;
	const char *env;
	const char *duid;
	int dra_ca_sec_tag;         /* CONFIG_IOTCONNECT_SEC_TAG_DRA_CA */
	int timeout_ms;             /* CONFIG_IOTCONNECT_DRA_HTTP_TIMEOUT_MS */
} iotc_dra_config_t;

/**
 * Run discovery + identity and populate the iotc-c-lib MQTT config in place.
 * On success, iotcl_mqtt_get_config() returns the resolved host/client_id/
 * username/topics. The library MUST already be initialized in
 * IOTCL_DCT_CUSTOM mode before calling this. Returns 0 (IOTCL_SUCCESS) or an
 * error code (HTTP failure or IOTCL_ERR_*).
 */
int iotc_dra_run(const iotc_dra_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* IOTC_DRA_CLIENT_H */
