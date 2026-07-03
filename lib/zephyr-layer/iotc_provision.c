/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * On-device key + self-signed certificate generation (feasibility spike).
 *
 * Generates an EC P-256 keypair on the device (PSA/CAAM-backed RNG) and
 * self-signs an X.509 certificate with CN=<duid>. The certificate is printed
 * for the user to register in IOTCONNECT (Self-Signed auth); the private key
 * can then be kept on-device. This is the "device generates its own identity"
 * model -- the private key never has to touch a PC.
 *
 * Requires (board .conf / overlay):
 *   CONFIG_MBEDTLS_X509_CRT_WRITE_C=y
 *   CONFIG_MBEDTLS_PK_WRITE_C=y
 *   CONFIG_MBEDTLS_PEM_WRITE_C=y
 *   plus EC/ECDSA (PSA_WANT_* / MBEDTLS_ECP) as needed by the crypto config.
 */

#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <psa/crypto.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>

#include "iotconnect_provision.h"
#include "iotconnect_identity.h"	/* iotc_kv_save() -- backend-neutral store */

LOG_MODULE_REGISTER(iotc_provision, CONFIG_IOTCONNECT_LOG_LEVEL);

int iotc_provision_selfsigned(const char *duid, char *key_pem, size_t key_pem_sz,
			      char *cert_pem, size_t cert_pem_sz)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key_id = 0;
	mbedtls_pk_context pk;
	mbedtls_x509write_cert crt;
	char name[96];
	const unsigned char serial[] = {0x01};
	psa_status_t ps;
	int ret = 0;

	if (duid == NULL || key_pem == NULL || cert_pem == NULL) {
		return -EINVAL;
	}
	(void)psa_crypto_init();
	mbedtls_pk_init(&pk);
	mbedtls_x509write_crt_init(&crt);

	/* 1. Generate an EC P-256 keypair on-device (PSA / hardware RNG). */
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	ps = psa_generate_key(&attr, &key_id);
	if (ps != PSA_SUCCESS) {
		LOG_ERR("psa_generate_key failed (%d)", (int)ps);
		ret = -EIO;
		goto out;
	}
	/* Copy the key material out of PSA into a pk context (the key was created
	 * with EXPORT usage) so we can BOTH write its private-key PEM and sign the
	 * self-signed cert with it. */
	ret = mbedtls_pk_copy_from_psa(key_id, &pk);
	if (ret) {
		goto out;
	}
	ret = mbedtls_pk_write_key_pem(&pk, (unsigned char *)key_pem, key_pem_sz);
	if (ret) {
		goto out;
	}

	/* 2. Self-sign a certificate CN=<duid>, signed by the same key. */
	snprintf(name, sizeof(name), "CN=%s", duid);
	mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
	mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
	mbedtls_x509write_crt_set_subject_key(&crt, &pk);
	mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
	ret = mbedtls_x509write_crt_set_subject_name(&crt, name);
	if (ret) {
		goto out;
	}
	ret = mbedtls_x509write_crt_set_issuer_name(&crt, name);
	if (ret) {
		goto out;
	}
	ret = mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char *)serial,
						   sizeof(serial));
	if (ret) {
		goto out;
	}
	ret = mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20360101000000");
	if (ret) {
		goto out;
	}
	ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
	if (ret) {
		goto out;
	}
	ret = mbedtls_x509write_crt_pem(&crt, (unsigned char *)cert_pem, cert_pem_sz);
out:
	mbedtls_x509write_crt_free(&crt);
	mbedtls_pk_free(&pk);
	if (key_id != 0) {
		(void)psa_destroy_key(key_id); /* spike: volatile key; real flow persists it */
	}
	if (ret) {
		LOG_ERR("selfsigned gen failed: -0x%04x", (unsigned int)-ret);
		return ret;
	}
	return 0;
}

#ifdef CONFIG_IOTCONNECT_SHELL
#include <zephyr/shell/shell.h>
#if defined(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

static char sp_key[512];
static char sp_crt[1024];

static int cmd_keygen(const struct shell *sh, size_t argc, char **argv)
{
	int ret = iotc_provision_selfsigned(argv[1], sp_key, sizeof(sp_key),
					    sp_crt, sizeof(sp_crt));

	if (ret) {
		shell_error(sh, "keygen failed (-0x%04x)", (unsigned int)-ret);
		return ret;
	}
	shell_print(sh, "Generated EC P-256 key + self-signed cert for CN=%s", argv[1]);
	shell_print(sh, "Register THIS certificate in IOTCONNECT (Self-Signed):\n%s", sp_crt);
	shell_print(sh, "(private key generated on-device; %zu bytes)", strlen(sp_key));
	return 0;
}

/* Generate key+cert on-device AND store the full identity in NVS so the device
 * connects with its own key after a reboot (device does its own keygen). */
static int cmd_provision(const struct shell *sh, size_t argc, char **argv)
{
#if !defined(CONFIG_SETTINGS) && !defined(CONFIG_BUILD_WITH_TFM) && \
	!defined(CONFIG_IOTCONNECT_IDENTITY_DISK)
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_error(sh, "provision requires storage (CONFIG_IOTCONNECT_IDENTITY_NVS / TF-M / DISK)");
	return -ENOTSUP;
#else
	const char *duid = argv[1];
	const char *cpid = (argc > 2) ? argv[2] : CONFIG_IOTCONNECT_CPID;
	const char *env = (argc > 3) ? argv[3] : CONFIG_IOTCONNECT_ENV;
	int ret;

	if (env[0] == '\0') {
		env = "poc";
	}
	ret = iotc_provision_selfsigned(duid, sp_key, sizeof(sp_key), sp_crt, sizeof(sp_crt));
	if (ret) {
		shell_error(sh, "on-device keygen failed (-0x%04x)", (unsigned int)-ret);
		return ret;
	}
	/* Store the device-generated identity via the active backend (TF-M PSA
	 * Protected Storage or settings/NVS). cert/key lengths include the trailing
	 * NUL that mbedTLS PEM parsing expects. */
	(void)iotc_kv_save("duid", duid, strlen(duid));
	(void)iotc_kv_save("cpid", cpid, strlen(cpid));
	(void)iotc_kv_save("env", env, strlen(env));
	(void)iotc_kv_save("cert", sp_crt, strlen(sp_crt) + 1);
	(void)iotc_kv_save("key", sp_key, strlen(sp_key) + 1);

#if defined(CONFIG_BUILD_WITH_TFM)
	shell_print(sh, "On-device identity generated and stored in hardware-sealed "
			"storage (PSA Protected Storage) for '%s'.", duid);
#elif defined(CONFIG_IOTCONNECT_IDENTITY_DISK)
	shell_print(sh, "On-device identity generated and persisted to the reserved "
			"eMMC/SD region for '%s'.", duid);
#else
	shell_print(sh, "On-device identity generated and stored in NVS for '%s'.", duid);
#endif
	shell_print(sh, "The private key was created on this device (never left it).");
	shell_print(sh, "");
	shell_print(sh, "NEXT STEPS:");
	shell_print(sh, "1) In /IOTCONNECT: Devices -> Create Device");
	shell_print(sh, "     Unique ID = %s   Auth = Self-Signed", duid);
	shell_print(sh, "   Paste this certificate:");
	shell_print(sh, "%s", sp_crt);
	shell_print(sh, "2) Download iotcDeviceConfig.json from the device's Info panel, then");
	shell_print(sh, "   paste it here to set cpid/env/duid:");
	shell_print(sh, "     iotc config");
	shell_print(sh, "     { ...paste the JSON block... }");
	shell_print(sh, "3) Connect:  reboot (kernel reboot cold, or power-cycle the");
	shell_print(sh, "   board if the target has no software reset) -> comes up as '%s'", duid);
	return 0;
#endif
}

SHELL_STATIC_SUBCMD_SET_CREATE(iotc_prov_cmds,
	SHELL_CMD_ARG(keygen, NULL, "<duid>  generate key+cert on-device (print only)", cmd_keygen, 2, 0),
	SHELL_CMD_ARG(provision, NULL, "<duid> [cpid] [env]  generate on-device + store to NVS",
		      cmd_provision, 2, 2),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(iotcprov, &iotc_prov_cmds, "IOTCONNECT on-device key/cert generation", NULL);
#endif /* CONFIG_IOTCONNECT_SHELL */
