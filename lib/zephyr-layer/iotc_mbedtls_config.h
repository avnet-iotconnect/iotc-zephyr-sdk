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
 * Asymmetric TLS record buffers.
 *
 * OUTBOUND records we produce are small (MQTT publishes, HTTP GET, handshake
 * messages incl. the device certificate -- all well under 4 KB), and a TLS
 * sender may always fragment, so a reduced out buffer is safe and reclaims
 * RAM on every target.
 *
 * INBOUND is different: peers choose the record size, and AWS endpoints
 * ignore the RFC 6066 max_fragment_length request Zephyr advertises. S3
 * (OTA downloads) sends full 16384-byte records, which are FATAL to a
 * session whose in buffer is smaller (mbedTLS aborts with
 * MBEDTLS_ERR_SSL_BAD_INPUT_DATA, "requesting more data than fits").
 * The inbound buffer must therefore stay at the full 16 KB except on the
 * RAM-tight TF-M non-secure partition, which does not use HTTPS OTA and
 * where MQTT/DRA records stay small.
 */
#if defined(CONFIG_BUILD_WITH_TFM)
#undef MBEDTLS_SSL_IN_CONTENT_LEN
#define MBEDTLS_SSL_IN_CONTENT_LEN 8192
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048
#else
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#endif

