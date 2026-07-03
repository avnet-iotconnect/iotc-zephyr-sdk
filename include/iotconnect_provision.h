/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * On-device key + self-signed certificate generation for IOTCONNECT.
 */

#ifndef IOTCONNECT_PROVISION_H
#define IOTCONNECT_PROVISION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generate an EC P-256 keypair on-device and self-sign an X.509 certificate
 * with subject/issuer CN=<duid>. Outputs NUL-terminated PEM strings.
 *
 * The certificate is what you register in IOTCONNECT (Self-Signed auth). The
 * private key is generated on the device from a hardware-backed RNG.
 *
 * Returns 0 on success, or a negative errno / mbedTLS error code.
 */
int iotc_provision_selfsigned(const char *duid, char *key_pem, size_t key_pem_sz,
			      char *cert_pem, size_t cert_pem_sz);

#ifdef __cplusplus
}
#endif

#endif /* IOTCONNECT_PROVISION_H */
