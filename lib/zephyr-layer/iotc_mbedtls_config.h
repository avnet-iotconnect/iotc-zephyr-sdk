/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * mbedTLS USER config appended after the Zephyr-generated config, used when
 * CONFIG_IOTCONNECT_ONDEVICE_KEYGEN=y to enable X.509 certificate creation
 * (the Kconfig exposes MBEDTLS_X509_CRT_WRITE_C but not its MBEDTLS_X509_CREATE_C
 * prerequisite). Point CONFIG_MBEDTLS_USER_CONFIG_FILE at this header.
 */

#ifndef MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CREATE_C
#endif

/*
 * Asymmetric TLS record buffers for the RAM-tight TF-M non-secure partition.
 * The INBOUND buffer stays at the Kconfig CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN so
 * it can hold the server's Certificate flight. OUTBOUND handshake messages
 * (ClientHello, ClientKeyExchange, Finished -- the DRA handshake is server-auth
 * so there is no client cert) are all < 2 KB, so shrinking the out buffer
 * reclaims ~6 KB of RAM to enlarge the network RX pool -- needed so the
 * multi-segment server certificate is not dropped on a busy LAN, which stalls
 * the handshake into a connect() timeout.
 */
#undef MBEDTLS_SSL_IN_CONTENT_LEN
#define MBEDTLS_SSL_IN_CONTENT_LEN 8192
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048

