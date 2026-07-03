/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * TLS credential registration seam (implementation).
 *
 * Wraps Zephyr's tls_credentials subsystem (zephyr/net/tls_credentials.h) so the
 * orchestrator can register the broker CA, the DRA CA, and the device
 * certificate + private key under the Kconfig sec tags.
 *
 * Idempotency: with the default volatile (RAM) credentials backend, credentials
 * are re-registered on every boot from the PEM blobs in
 * IotConnectClientConfig.auth_info. tls_credential_add() returns -EEXIST if a
 * (tag,type) slot is already populated; we treat that as success so re-provision
 * across reboots (and double-connects within a boot) is harmless.
 *
 * PSA / secure storage: when CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE is set
 * the device cert/key are expected to already reside in PSA Protected Storage via
 * CONFIG_TLS_CREDENTIALS_BACKEND_PROTECTED_STORAGE (TF-M). In that case the
 * caller passes device_cert/device_key as NULL and we only ensure the CA certs
 * are present; the device material is referenced by its existing sec tag at
 * handshake time.
 */

#include "iotc_tls_credentials.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/tls_credentials.h>

LOG_MODULE_REGISTER(iotc_tls_creds, CONFIG_IOTCONNECT_LOG_LEVEL);

/*
 * Register one credential blob under (tag,type).
 *
 * Treats -EEXIST as success (idempotent re-provision against the volatile
 * backend). Returns 0 on success/already-present, or the failing negative errno.
 */
static int creds_add(sec_tag_t tag, enum tls_credential_type type,
		     const char *what, const void *cred, size_t credlen)
{
	int ret;

	if (cred == NULL || credlen == 0) {
		/* Nothing to register for this slot. */
		return 0;
	}

	ret = tls_credential_add(tag, type, cred, credlen);
	if (ret == -EEXIST) {
		LOG_DBG("%s already present at sec tag %u (idempotent)", what,
			(unsigned int)tag);
		return 0;
	}
	if (ret != 0) {
		LOG_ERR("Failed to add %s at sec tag %u: %d", what,
			(unsigned int)tag, ret);
		return ret;
	}

	LOG_DBG("Registered %s at sec tag %u (%zu bytes)", what,
		(unsigned int)tag, credlen);
	return 0;
}

int iotc_tls_credentials_register(const iotc_tls_creds_t *creds)
{
	int ret;

	if (creds == NULL) {
		return -EINVAL;
	}

	/* --- Broker (cloud) root CA -------------------------------------- */
	ret = creds_add((sec_tag_t)creds->broker_ca_sec_tag,
			TLS_CREDENTIAL_CA_CERTIFICATE, "broker CA",
			creds->broker_ca, creds->broker_ca_len);
	if (ret != 0) {
		return ret;
	}

	/* --- Discovery/Identity HTTPS root CA ---------------------------- */
	ret = creds_add((sec_tag_t)creds->dra_ca_sec_tag,
			TLS_CREDENTIAL_CA_CERTIFICATE, "DRA CA",
			creds->dra_ca, creds->dra_ca_len);
	if (ret != 0) {
		return ret;
	}

#ifdef CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE
	/*
	 * Device cert/key live in PSA Protected Storage
	 * (CONFIG_TLS_CREDENTIALS_BACKEND_PROTECTED_STORAGE / TF-M); the caller
	 * passes them NULL. Skip adding them here -- the handshake references the
	 * existing protected-storage entries by device_sec_tag.
	 *
	 * TODO(board): on secure-element boards the device private key may never
	 * be exportable as PEM and therefore cannot be registered with
	 * tls_credential_add() at all. Such keys must be provisioned once via the
	 * vendor's PSA / TF-M secure provisioning flow (e.g. psa_import_key into
	 * persistent storage, or a factory secure-element personalization step),
	 * after which only device_sec_tag is needed here.
	 */
	if (creds->device_cert != NULL || creds->device_key != NULL) {
		LOG_WRN("PSA protected storage enabled but device cert/key blobs "
			"were supplied; ignoring (expected in protected storage)");
	}
#else
	/*
	 * Volatile (RAM) backend: register the device certificate AND private key
	 * under the SAME device_sec_tag (a cert+key pair). The MQTT mutual-TLS
	 * handshake selects the client identity by this single tag.
	 */
	ret = creds_add((sec_tag_t)creds->device_sec_tag,
			TLS_CREDENTIAL_SERVER_CERTIFICATE, "device cert",
			creds->device_cert, creds->device_cert_len);
	if (ret != 0) {
		return ret;
	}

	ret = creds_add((sec_tag_t)creds->device_sec_tag,
			TLS_CREDENTIAL_PRIVATE_KEY, "device key",
			creds->device_key, creds->device_key_len);
	if (ret != 0) {
		return ret;
	}
#endif /* CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE */

	return 0;
}

/*
 * Best-effort delete; logs but does not abort on per-slot failure (e.g.
 * -ENOENT when a slot was never populated, which is benign).
 */
static void creds_delete(sec_tag_t tag, enum tls_credential_type type,
			 const char *what, const void *cred)
{
	int ret;

	if (cred == NULL) {
		return;
	}

	ret = tls_credential_delete(tag, type);
	if (ret != 0 && ret != -ENOENT) {
		LOG_WRN("Failed to delete %s at sec tag %u: %d", what,
			(unsigned int)tag, ret);
	}
}

void iotc_tls_credentials_clear(const iotc_tls_creds_t *creds)
{
	if (creds == NULL) {
		return;
	}

	creds_delete((sec_tag_t)creds->broker_ca_sec_tag,
		     TLS_CREDENTIAL_CA_CERTIFICATE, "broker CA",
		     creds->broker_ca);

	creds_delete((sec_tag_t)creds->dra_ca_sec_tag,
		     TLS_CREDENTIAL_CA_CERTIFICATE, "DRA CA", creds->dra_ca);

#ifndef CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE
	/*
	 * Only the RAM-backend device material was registered here, so only it is
	 * removed. Device cert/key held in PSA Protected Storage / a secure
	 * element are persistent and are intentionally NOT deleted by this path.
	 */
	creds_delete((sec_tag_t)creds->device_sec_tag,
		     TLS_CREDENTIAL_SERVER_CERTIFICATE, "device cert",
		     creds->device_cert);

	creds_delete((sec_tag_t)creds->device_sec_tag,
		     TLS_CREDENTIAL_PRIVATE_KEY, "device key",
		     creds->device_key);
#endif /* CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE */
}
