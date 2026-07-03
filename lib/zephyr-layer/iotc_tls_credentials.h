/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * TLS credential registration seam. Wraps zephyr/net/tls_credentials.h so the
 * orchestrator can register the broker CA, the DRA CA, and the device
 * certificate + private key under the Kconfig sec tags. PSA-friendly: when
 * CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE is set the credentials are
 * expected to already live in PSA Protected Storage and registration of the
 * device cert/key may be skipped.
 */
#ifndef IOTC_TLS_CREDENTIALS_H
#define IOTC_TLS_CREDENTIALS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Credential blobs (NUL-terminated PEM) + the sec tags to register them under.
 * Any pointer may be NULL to skip that credential (e.g. when it is provisioned
 * out-of-band / in protected storage). The device cert and key MUST share
 * device_sec_tag.
 */
typedef struct {
	const char *broker_ca;     size_t broker_ca_len;
	int broker_ca_sec_tag;

	const char *dra_ca;        size_t dra_ca_len;
	int dra_ca_sec_tag;

	const char *device_cert;   size_t device_cert_len;
	const char *device_key;    size_t device_key_len;
	int device_sec_tag;
} iotc_tls_creds_t;

/**
 * Register the provided credentials. Idempotent: -EEXIST from
 * tls_credential_add() is treated as success. Returns 0 on success or the
 * first failing -errno.
 */
int iotc_tls_credentials_register(const iotc_tls_creds_t *creds);

/** Remove previously registered credentials (best-effort). */
void iotc_tls_credentials_clear(const iotc_tls_creds_t *creds);

#ifdef __cplusplus
}
#endif

#endif /* IOTC_TLS_CREDENTIALS_H */
