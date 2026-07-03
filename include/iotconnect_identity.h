/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * NVS-backed device identity for the IOTCONNECT Zephyr SDK.
 *
 * The Python Lite SDK loads identity from files (iotcDeviceConfig.json + PEMs).
 * MCUs usually have no filesystem, so this layer stores the per-device identity
 * (cpid/env/duid + device cert/key) in Zephyr settings (NVS backend) under the
 * "iotc/" subtree. Creds then survive an application reflash and can be swapped
 * without recompiling -- provisioned once via the `iotc cred` shell commands
 * (enable CONFIG_IOTCONNECT_SHELL).
 *
 * The broker/DRA CA roots are public and stay compiled-in; only the per-device
 * secrets live in NVS.
 *
 * Typical app flow:
 *   struct iotc_identity id;
 *   if (iotc_identity_load(&id) == 0) {  // provisioned in NVS
 *       config.cpid = (char *)id.cpid; ... config cert/key from id
 *   } else {                             // fall back to compiled-in header
 *       ... use device_credentials.h
 *   }
 */

#ifndef IOTCONNECT_IDENTITY_H
#define IOTCONNECT_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolved device identity. String/buffer members point at internal static
 * storage owned by the identity module -- valid for the life of the program;
 * do not free. cert/key lengths include the trailing NUL (mbedTLS PEM parse
 * expects a NUL-terminated buffer counted in the length). */
struct iotc_identity {
	const char *cpid;
	const char *env;
	const char *duid;
	const uint8_t *device_cert;
	size_t device_cert_len;
	const uint8_t *device_key;
	size_t device_key_len;
};

/*
 * Load the device identity from Zephyr settings (subtree "iotc/").
 * Initializes the settings subsystem on first call.
 *
 * Returns 0 and fills *id if a COMPLETE identity is provisioned (duid + cpid +
 * env + device cert + device key all present). Returns -ENOENT if not
 * provisioned (the app should fall back to compiled-in credentials), or a
 * negative errno on a settings/flash error.
 */
int iotc_identity_load(struct iotc_identity *id);

/*
 * Persist one identity field ("cpid","env","duid","cert","key") to the active
 * storage backend and return 0 on success (negative errno otherwise). The
 * backend is chosen at build time: TF-M builds seal the value in hardware-backed
 * PSA Protected Storage; other builds use Zephyr settings/NVS. Used by the
 * provisioning paths (`iotc cred`, `iotc config`, `iotcprov provision`).
 */
int iotc_kv_save(const char *name, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* IOTCONNECT_IDENTITY_H */
