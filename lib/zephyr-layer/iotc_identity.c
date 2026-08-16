/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * NVS-backed device identity (Zephyr settings, subtree "iotc/").
 * See include/iotconnect_identity.h.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_BUILD_WITH_TFM)
#include <psa/protected_storage.h>
#elif defined(CONFIG_IOTCONNECT_IDENTITY_DISK)
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/byteorder.h>
#if defined(CONFIG_SOC_MIMX9352)
#include <fsl_clock.h>
#endif
#else
#include <zephyr/settings/settings.h>
#endif

#include "iotconnect_identity.h"

LOG_MODULE_REGISTER(iotc_identity, CONFIG_IOTCONNECT_LOG_LEVEL);

/* Per-device identity. On TF-M builds these are backed by hardware PSA Protected
 * Storage (no non-secure flash driver -- which does not work from the NS world
 * on this SoC); otherwise by Zephyr settings/NVS. iotc_kv_save() is the single
 * write path used by the provisioning commands. */
static char s_cpid[64];
static char s_env[32];
static char s_duid[128];
/* Buffers sized for the on-device EC P-256 identity (cert ~454 B, key ~228 B).
 * On the RAM-tight TF-M non-secure partition these are deliberately small; the
 * non-TF-M (settings/NVS) builds have more RAM headroom but reuse the same
 * sizes for simplicity. Raise them if you provision a larger (e.g. RSA) key. */
#if defined(CONFIG_BUILD_WITH_TFM)
/* Tight EC P-256 sizes for the RAM-constrained TF-M non-secure partition
 * (cert ~454 B, key ~228 B). Raise for RSA identities. */
#define IOTC_CERT_BUF_SIZE 768
#define IOTC_KEY_BUF_SIZE  384
#else
#define IOTC_CERT_BUF_SIZE 2048
#define IOTC_KEY_BUF_SIZE  2048
#endif
static uint8_t s_cert[IOTC_CERT_BUF_SIZE];
static size_t s_cert_len;
static uint8_t s_key[IOTC_KEY_BUF_SIZE];
static size_t s_key_len;

#if defined(CONFIG_BUILD_WITH_TFM)
/* --- TF-M PSA Protected Storage backend (single packed asset) -------------
 * The TF-M PS filesystem on this SoC (frdmmcxn947: 16 KB PS area, 8 KB block,
 * PS_MAX_ASSET_SIZE=2048) reserves a full max-asset slot per stored asset, so
 * 5 separate assets (cpid/env/duid/cert/key) overflow the block and psa_ps_set
 * returns PSA_ERROR_INSUFFICIENT_STORAGE (-142) on the 4th. We therefore pack
 * the whole identity into ONE PS asset (~840 bytes for an EC identity, one
 * slot, well under PS_MAX_ASSET_SIZE). Wire format: for each field, a
 * little-endian uint16 length followed by the bytes, in the fixed order
 * cpid, env, duid, cert, key (strings stored without their NUL). */

#define IOTC_PS_BLOB_UID  ((psa_storage_uid_t)0x494F544300ULL)	/* "IOTC" + 0 */

/* Shared serialization buffer (pack at provision time, unpack at boot -- never
 * concurrent). Sized to PS_MAX_ASSET_SIZE, the hard cap on a single PS asset. */
static uint8_t s_ps_blob[IOTC_CERT_BUF_SIZE + IOTC_KEY_BUF_SIZE +
			 sizeof(s_cpid) + sizeof(s_env) + sizeof(s_duid) + 16];

static void blob_put_u16(size_t *off, uint16_t v)
{
	s_ps_blob[(*off)++] = (uint8_t)(v & 0xFF);
	s_ps_blob[(*off)++] = (uint8_t)(v >> 8);
}

static int blob_append(size_t *off, const void *p, size_t n)
{
	if (*off + 2 + n > sizeof(s_ps_blob)) {
		return -ENOMEM;
	}
	blob_put_u16(off, (uint16_t)n);
	memcpy(&s_ps_blob[*off], p, n);
	*off += n;
	return 0;
}

/* Re-serialize the in-RAM identity fields and store them as one PS asset. */
static int ps_pack_store(void)
{
	size_t off = 0;
	int rc = blob_append(&off, s_cpid, strlen(s_cpid)) ||
		 blob_append(&off, s_env, strlen(s_env)) ||
		 blob_append(&off, s_duid, strlen(s_duid)) ||
		 blob_append(&off, s_cert, s_cert_len) ||
		 blob_append(&off, s_key, s_key_len);
	psa_status_t st;

	if (rc) {
		printk("[iotc] identity too large for a single PS asset\n");
		return -ENOMEM;
	}
	/* Drop any legacy per-field assets (0x..01..05) written by older firmware
	 * so they do not keep consuming PS slots next to the packed blob. */
	for (psa_storage_uid_t u = 0x494F544301ULL; u <= 0x494F544305ULL; u++) {
		(void)psa_ps_remove(u);
	}
	st = psa_ps_set(IOTC_PS_BLOB_UID, off, s_ps_blob, 0);
	if (st != PSA_SUCCESS) {
		LOG_ERR("PS store failed (%d)", (int)st);
		return -EIO;
	}
	return 0;
}

/* Backend write path: update the matching in-RAM field, then rewrite the blob.
 * (Data may alias one of the s_* buffers, so use memmove.) */
int iotc_kv_save(const char *name, const void *data, size_t len)
{
	if (!strcmp(name, "cpid")) {
		len = MIN(len, sizeof(s_cpid) - 1); memmove(s_cpid, data, len); s_cpid[len] = '\0';
	} else if (!strcmp(name, "env")) {
		len = MIN(len, sizeof(s_env) - 1); memmove(s_env, data, len); s_env[len] = '\0';
	} else if (!strcmp(name, "duid")) {
		len = MIN(len, sizeof(s_duid) - 1); memmove(s_duid, data, len); s_duid[len] = '\0';
	} else if (!strcmp(name, "cert")) {
		len = MIN(len, sizeof(s_cert)); memmove(s_cert, data, len); s_cert_len = len;
	} else if (!strcmp(name, "key")) {
		len = MIN(len, sizeof(s_key)); memmove(s_key, data, len); s_key_len = len;
	} else {
		return -EINVAL;
	}
	return ps_pack_store();
}

static void kv_delete(const char *name)
{
	ARG_UNUSED(name);
	(void)psa_ps_remove(IOTC_PS_BLOB_UID);	/* one asset holds the whole identity */
}

static int blob_take(size_t *off, size_t total, void *dst, size_t cap, size_t *outn)
{
	uint16_t n;

	if (*off + 2 > total) {
		return -EINVAL;
	}
	n = (uint16_t)(s_ps_blob[*off] | (s_ps_blob[*off + 1] << 8));
	*off += 2;
	if (*off + n > total || n > cap) {
		return -EINVAL;
	}
	memcpy(dst, &s_ps_blob[*off], n);
	*off += n;
	*outn = n;
	return 0;
}

int iotc_identity_load(struct iotc_identity *id)
{
	struct psa_storage_info_t info;
	size_t got = 0, off = 0, n;
	psa_status_t st;

	if (id == NULL) {
		return -EINVAL;
	}
	st = psa_ps_get_info(IOTC_PS_BLOB_UID, &info);
	if (st != PSA_SUCCESS) {
		return -ENOENT; /* not provisioned -> caller prints the guide */
	}
	if (psa_ps_get(IOTC_PS_BLOB_UID, 0, MIN(info.size, sizeof(s_ps_blob)), s_ps_blob,
		       &got) != PSA_SUCCESS) {
		return -ENOENT;
	}
	if (blob_take(&off, got, s_cpid, sizeof(s_cpid) - 1, &n)) { return -ENOENT; }
	s_cpid[n] = '\0';
	if (blob_take(&off, got, s_env, sizeof(s_env) - 1, &n)) { return -ENOENT; }
	s_env[n] = '\0';
	if (blob_take(&off, got, s_duid, sizeof(s_duid) - 1, &n)) { return -ENOENT; }
	s_duid[n] = '\0';
	if (blob_take(&off, got, s_cert, sizeof(s_cert), &s_cert_len)) { return -ENOENT; }
	if (blob_take(&off, got, s_key, sizeof(s_key), &s_key_len)) { return -ENOENT; }

	if (s_duid[0] == '\0' || s_cpid[0] == '\0' || s_env[0] == '\0' ||
	    s_cert_len == 0 || s_key_len == 0) {
		return -ENOENT; /* not (fully) provisioned -> caller falls back */
	}
	id->cpid = s_cpid;
	id->env = s_env;
	id->duid = s_duid;
	id->device_cert = s_cert;
	id->device_cert_len = s_cert_len;
	id->device_key = s_key;
	id->device_key_len = s_key_len;
	return 0;
}

#elif defined(CONFIG_IOTCONNECT_IDENTITY_DISK)
/* --- Raw SD/eMMC disk-sector backend (bare-metal, no NVS flash) -----------
 * AP-class targets like the i.MX93 A55 run Zephyr from DRAM with no on-chip
 * NVS flash, but boot from -- and can write -- an SD/eMMC card. Persist the
 * packed identity to a reserved sector region (default 4 MB) via the block
 * API. The boot container is at the raw 32 KB offset, well below this region,
 * so no filesystem or partition table is needed. On-region layout:
 *   u32 magic | u32 payload_len | { u16 len, bytes } x5 (cpid,env,duid,cert,key)
 */
#define IOTC_DISK_SECT_SZ   512u
#define IOTC_DISK_SECT_CNT  16u			/* 8 KB reserved region */
#define IOTC_DISK_MAGIC     0x44435449u		/* 'ITCD' little-endian */

static uint8_t s_disk_buf[IOTC_DISK_SECT_SZ * IOTC_DISK_SECT_CNT] __aligned(4);
static bool s_disk_inited;

static int disk_prepare(void)
{
	if (s_disk_inited) {
		return 0;
	}
#if defined(CONFIG_SOC_MIMX9352)
	/* Our standalone SPSDK boot (ROM -> SPL -> ATF -> Zephyr, no U-Boot) leaves
	 * the uSDHC root clocks unconfigured, so CLOCK_GetIpFreq() reads 0 and the SD
	 * host init fails with -ENOTSUP. Zephyr's i.MX93 SD support otherwise assumes
	 * U-Boot set these clocks up. Configure both uSDHC roots here from
	 * SysPll1Pfd1 (800 MHz) / 2 = 400 MHz. (Harmless if already configured.)
	 *
	 * NOTE: the board doc warns against using the boot controller (uSDHC2, the SD
	 * card slot) from Zephyr when the ROM booted from it -- the first data-line
	 * transfer stalls (ADMA never completes). We therefore persist identity to the
	 * on-SOM eMMC on uSDHC1 (disk "SD2"), a separate controller the ROM did not
	 * touch. We still clock uSDHC2 in case a board is used SD-only. */
	CLOCK_SetRootClockMux(kCLOCK_Root_Usdhc1,
			      kCLOCK_Usdhc1_ClockRoot_MuxSysPll1Pfd1);
	CLOCK_SetRootClockDiv(kCLOCK_Root_Usdhc1, 2);
	CLOCK_PowerOnRootClock(kCLOCK_Root_Usdhc1);
	CLOCK_SetRootClockMux(kCLOCK_Root_Usdhc2,
			      kCLOCK_Usdhc2_ClockRoot_MuxSysPll1Pfd1);
	CLOCK_SetRootClockDiv(kCLOCK_Root_Usdhc2, 2);
	CLOCK_PowerOnRootClock(kCLOCK_Root_Usdhc2);
#endif
	int rc = disk_access_init(CONFIG_IOTCONNECT_IDENTITY_DISK_NAME);

	if (rc) {
		LOG_ERR("disk_access_init(%s) failed (%d)",
			CONFIG_IOTCONNECT_IDENTITY_DISK_NAME, rc);
		return -EIO;
	}
	s_disk_inited = true;
	return 0;
}

static int disk_put(size_t *off, const void *p, size_t n)
{
	if (*off + 2 + n > sizeof(s_disk_buf)) {
		return -ENOMEM;
	}
	s_disk_buf[(*off)++] = (uint8_t)(n & 0xFF);
	s_disk_buf[(*off)++] = (uint8_t)(n >> 8);
	memcpy(&s_disk_buf[*off], p, n);
	*off += n;
	return 0;
}

/* Re-serialize the in-RAM identity fields and write the reserved sector region. */
static int disk_pack_store(void)
{
	size_t off = 8; /* leave room for magic + payload_len */
	int rc = disk_put(&off, s_cpid, strlen(s_cpid)) ||
		 disk_put(&off, s_env, strlen(s_env)) ||
		 disk_put(&off, s_duid, strlen(s_duid)) ||
		 disk_put(&off, s_cert, s_cert_len) ||
		 disk_put(&off, s_key, s_key_len);

	if (rc) {
		printk("[iotc] identity too large for the disk region\n");
		return -ENOMEM;
	}
	sys_put_le32(IOTC_DISK_MAGIC, s_disk_buf);
	sys_put_le32((uint32_t)(off - 8), s_disk_buf + 4);
	memset(&s_disk_buf[off], 0, sizeof(s_disk_buf) - off); /* don't leak stale bytes */
	if (disk_prepare()) {
		return -EIO;
	}
	if (disk_access_write(CONFIG_IOTCONNECT_IDENTITY_DISK_NAME, s_disk_buf,
			      CONFIG_IOTCONNECT_IDENTITY_DISK_SECTOR, IOTC_DISK_SECT_CNT)) {
		LOG_ERR("disk write failed");
		return -EIO;
	}
	/* Flush the device's write cache: eMMC parts buffer writes internally
	 * and an abrupt power-off can drop them (hardware-observed: identity
	 * vanished across a power cycle without this). */
	(void)disk_access_ioctl(CONFIG_IOTCONNECT_IDENTITY_DISK_NAME,
				DISK_IOCTL_CTRL_SYNC, NULL);
	return 0;
}

int iotc_kv_save(const char *name, const void *data, size_t len)
{
	if (!strcmp(name, "cpid")) {
		len = MIN(len, sizeof(s_cpid) - 1); memmove(s_cpid, data, len); s_cpid[len] = '\0';
	} else if (!strcmp(name, "env")) {
		len = MIN(len, sizeof(s_env) - 1); memmove(s_env, data, len); s_env[len] = '\0';
	} else if (!strcmp(name, "duid")) {
		len = MIN(len, sizeof(s_duid) - 1); memmove(s_duid, data, len); s_duid[len] = '\0';
	} else if (!strcmp(name, "cert")) {
		len = MIN(len, sizeof(s_cert)); memmove(s_cert, data, len); s_cert_len = len;
	} else if (!strcmp(name, "key")) {
		len = MIN(len, sizeof(s_key)); memmove(s_key, data, len); s_key_len = len;
	} else {
		return -EINVAL;
	}
	return disk_pack_store();
}

static void kv_delete(const char *name)
{
	ARG_UNUSED(name);
	memset(s_disk_buf, 0, sizeof(s_disk_buf)); /* clears the magic -> not provisioned */
	if (disk_prepare() == 0) {
		(void)disk_access_write(CONFIG_IOTCONNECT_IDENTITY_DISK_NAME, s_disk_buf,
					CONFIG_IOTCONNECT_IDENTITY_DISK_SECTOR, IOTC_DISK_SECT_CNT);
	}
}

int iotc_identity_load(struct iotc_identity *id)
{
	const uint8_t *p;
	uint32_t plen;
	size_t off = 0, n = 0;

	if (id == NULL) {
		return -EINVAL;
	}
	if (disk_prepare()) {
		return -ENOENT;
	}
	if (disk_access_read(CONFIG_IOTCONNECT_IDENTITY_DISK_NAME, s_disk_buf,
			     CONFIG_IOTCONNECT_IDENTITY_DISK_SECTOR, IOTC_DISK_SECT_CNT)) {
		return -ENOENT;
	}
	if (sys_get_le32(s_disk_buf) != IOTC_DISK_MAGIC) {
		return -ENOENT; /* blank/unprovisioned -> caller prints the guide */
	}
	plen = sys_get_le32(s_disk_buf + 4);
	if (plen > sizeof(s_disk_buf) - 8) {
		return -ENOENT;
	}
	p = s_disk_buf + 8;

#define IOTC_DISK_TAKE(dst, cap)                                        \
	do {                                                           \
		if (off + 2 > plen) { return -ENOENT; }                \
		n = (size_t)(p[off] | (p[off + 1] << 8)); off += 2;    \
		if (off + n > plen || n > (cap)) { return -ENOENT; }   \
		memcpy((dst), &p[off], n); off += n;                   \
	} while (0)

	IOTC_DISK_TAKE(s_cpid, sizeof(s_cpid) - 1); s_cpid[n] = '\0';
	IOTC_DISK_TAKE(s_env, sizeof(s_env) - 1);   s_env[n] = '\0';
	IOTC_DISK_TAKE(s_duid, sizeof(s_duid) - 1); s_duid[n] = '\0';
	IOTC_DISK_TAKE(s_cert, sizeof(s_cert));     s_cert_len = n;
	IOTC_DISK_TAKE(s_key, sizeof(s_key));       s_key_len = n;
#undef IOTC_DISK_TAKE

	if (s_duid[0] == '\0' || s_cpid[0] == '\0' || s_env[0] == '\0' ||
	    s_cert_len == 0 || s_key_len == 0) {
		return -ENOENT;
	}
	id->cpid = s_cpid;
	id->env = s_env;
	id->duid = s_duid;
	id->device_cert = s_cert;
	id->device_cert_len = s_cert_len;
	id->device_key = s_key;
	id->device_key_len = s_key_len;
	return 0;
}

#else
/* --- Zephyr settings/NVS backend (non-TF-M) ------------------------------- */

int iotc_kv_save(const char *name, const void *data, size_t len)
{
	char path[24];

	snprintf(path, sizeof(path), "iotc/%s", name);
	return settings_save_one(path, data, len);
}

static void kv_delete(const char *name)
{
	char path[24];

	snprintf(path, sizeof(path), "iotc/%s", name);
	(void)settings_delete(path);
}

static int load_str(settings_read_cb read_cb, void *cb_arg, char *dst, size_t dstsz, size_t len)
{
	ssize_t r;

	if (len >= dstsz) {
		len = dstsz - 1;
	}
	r = read_cb(cb_arg, dst, len);
	if (r < 0) {
		return (int)r;
	}
	dst[r] = '\0';
	return 0;
}

static int load_blob(settings_read_cb read_cb, void *cb_arg, uint8_t *dst, size_t dstsz,
		     size_t *lenout, size_t len)
{
	ssize_t r;

	if (len > dstsz) {
		return -ENOMEM;
	}
	r = read_cb(cb_arg, dst, len);
	if (r < 0) {
		return (int)r;
	}
	*lenout = (size_t)r;
	return 0;
}

static int iotc_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "cpid", &next) && !next) {
		return load_str(read_cb, cb_arg, s_cpid, sizeof(s_cpid), len);
	}
	if (settings_name_steq(name, "env", &next) && !next) {
		return load_str(read_cb, cb_arg, s_env, sizeof(s_env), len);
	}
	if (settings_name_steq(name, "duid", &next) && !next) {
		return load_str(read_cb, cb_arg, s_duid, sizeof(s_duid), len);
	}
	if (settings_name_steq(name, "cert", &next) && !next) {
		return load_blob(read_cb, cb_arg, s_cert, sizeof(s_cert), &s_cert_len, len);
	}
	if (settings_name_steq(name, "key", &next) && !next) {
		return load_blob(read_cb, cb_arg, s_key, sizeof(s_key), &s_key_len, len);
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(iotc_identity, "iotc", NULL, iotc_settings_set, NULL, NULL);

int iotc_identity_load(struct iotc_identity *id)
{
	int rc;

	if (id == NULL) {
		return -EINVAL;
	}
	rc = settings_subsys_init();
	if (rc) {
		LOG_ERR("settings_subsys_init failed (%d)", rc);
		return rc;
	}
	rc = settings_load_subtree("iotc");
	if (rc) {
		LOG_ERR("settings_load_subtree(iotc) failed (%d)", rc);
		return rc;
	}
	if (s_duid[0] == '\0' || s_cpid[0] == '\0' || s_env[0] == '\0' ||
	    s_cert_len == 0 || s_key_len == 0) {
		return -ENOENT; /* not (fully) provisioned -> caller falls back */
	}
	id->cpid = s_cpid;
	id->env = s_env;
	id->duid = s_duid;
	id->device_cert = s_cert;
	id->device_cert_len = s_cert_len;
	id->device_key = s_key;
	id->device_key_len = s_key_len;
	return 0;
}

#endif /* CONFIG_BUILD_WITH_TFM */

/* --- provisioning shell (iotc cred ...) ----------------------------------- */

#ifdef CONFIG_IOTCONNECT_SHELL

#include <zephyr/shell/shell.h>
#include "cJSON.h"

/* Minimal, dependency-free base64 decoder (skips whitespace/newlines, stops at
 * padding). Returns 0 on success. */
static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '+') {
		return 62;
	}
	if (c == '/') {
		return 63;
	}
	return -1;
}

static int b64_decode(uint8_t *out, size_t outsz, size_t *olen, const char *in)
{
	uint32_t buf = 0;
	int bits = 0;
	size_t o = 0;

	for (const char *p = in; *p != '\0'; p++) {
		int v;

		if (*p == '=') {
			break;
		}
		v = b64_val(*p);
		if (v < 0) {
			continue; /* skip whitespace / stray chars */
		}
		buf = (buf << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= outsz) {
				return -ENOMEM;
			}
			out[o++] = (uint8_t)((buf >> bits) & 0xFF);
		}
	}
	*olen = o;
	return 0;
}

static int save_str(const struct shell *sh, const char *key, const char *val, char *dst,
		    size_t dstsz)
{
	size_t n = strlen(val);
	int rc;

	if (n >= dstsz) {
		shell_error(sh, "%s too long (max %zu)", key, dstsz - 1);
		return -ENOMEM;
	}
	memcpy(dst, val, n);
	dst[n] = '\0';
	rc = iotc_kv_save(key, dst, n);
	if (rc) {
		shell_error(sh, "save %s failed (%d)", key, rc);
	} else {
		shell_print(sh, "%s set", key);
	}
	return rc;
}

static int save_pem(const struct shell *sh, const char *key, const char *b64, uint8_t *dst,
		    size_t dstsz, size_t *lenout)
{
	size_t olen;
	int rc;

	rc = b64_decode(dst, dstsz - 1, &olen, b64);
	if (rc) {
		shell_error(sh, "%s base64 decode failed (%d)", key, rc);
		return rc;
	}
	dst[olen] = '\0';        /* mbedTLS PEM parse wants a NUL-terminated buffer */
	*lenout = olen + 1;      /* length includes the NUL */
	rc = iotc_kv_save(key, dst, *lenout);
	if (rc) {
		shell_error(sh, "save %s failed (%d)", key, rc);
	} else {
		shell_print(sh, "%s set (%zu bytes)", key, *lenout);
	}
	return rc;
}

static int cmd_cpid(const struct shell *sh, size_t argc, char **argv)
{
	return save_str(sh, "cpid", argv[1], s_cpid, sizeof(s_cpid));
}
static int cmd_env(const struct shell *sh, size_t argc, char **argv)
{
	return save_str(sh, "env", argv[1], s_env, sizeof(s_env));
}
static int cmd_duid(const struct shell *sh, size_t argc, char **argv)
{
	return save_str(sh, "duid", argv[1], s_duid, sizeof(s_duid));
}
static int cmd_cert(const struct shell *sh, size_t argc, char **argv)
{
	return save_pem(sh, "cert", argv[1], s_cert, sizeof(s_cert), &s_cert_len);
}
static int cmd_key(const struct shell *sh, size_t argc, char **argv)
{
	return save_pem(sh, "key", argv[1], s_key, sizeof(s_key), &s_key_len);
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "cpid: %s", s_cpid[0] ? s_cpid : "(unset)");
	shell_print(sh, "env : %s", s_env[0] ? s_env : "(unset)");
	shell_print(sh, "duid: %s", s_duid[0] ? s_duid : "(unset)");
	shell_print(sh, "cert: %zu bytes", s_cert_len);
	shell_print(sh, "key : %zu bytes %s", s_key_len, s_key_len ? "(hidden)" : "");
	return 0;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	kv_delete("cpid");
	kv_delete("env");
	kv_delete("duid");
	kv_delete("cert");
	kv_delete("key");
	memset(s_cpid, 0, sizeof(s_cpid));
	memset(s_env, 0, sizeof(s_env));
	memset(s_duid, 0, sizeof(s_duid));
	memset(s_cert, 0, sizeof(s_cert));
	memset(s_key, 0, sizeof(s_key));
	s_cert_len = 0;
	s_key_len = 0;
	shell_print(sh, "identity erased");
	return 0;
}

/* Guided onboarding: store the identity strings and print a personalized,
 * step-by-step recipe (key-gen, portal, load, connect) for this DUID. */
static int cmd_setup(const struct shell *sh, size_t argc, char **argv)
{
	const char *duid = argv[1];
	char cpid_buf[sizeof(s_cpid)];
	char env_buf[sizeof(s_env)];
	const char *cpid = (argc > 2) ? argv[2] : (s_cpid[0] ? s_cpid : CONFIG_IOTCONNECT_CPID);
	const char *env = (argc > 3) ? argv[3] : (s_env[0] ? s_env : CONFIG_IOTCONNECT_ENV);

	/* snapshot (cpid/env may alias s_cpid/s_env, which save_str overwrites) */
	strncpy(cpid_buf, cpid, sizeof(cpid_buf) - 1);
	cpid_buf[sizeof(cpid_buf) - 1] = '\0';
	strncpy(env_buf, env[0] ? env : "poc", sizeof(env_buf) - 1);
	env_buf[sizeof(env_buf) - 1] = '\0';

	if (save_str(sh, "duid", duid, s_duid, sizeof(s_duid)) ||
	    save_str(sh, "cpid", cpid_buf, s_cpid, sizeof(s_cpid)) ||
	    save_str(sh, "env", env_buf, s_env, sizeof(s_env))) {
		return -EIO;
	}

	shell_print(sh, "");
	shell_print(sh, "=== IOTCONNECT onboarding: device '%s' ===", duid);
	shell_print(sh, "Stored to NVS:  cpid=%s  env=%s  duid=%s",
		    s_cpid[0] ? s_cpid : "(EMPTY - pass it: iotc setup <duid> <cpid>)", s_env, duid);
	shell_print(sh, "");
	shell_print(sh, "STEP 1  Generate this device's key + certificate on your PC:");
	shell_print(sh, "  openssl ecparam -name prime256v1 -genkey -noout -out %s-key.pem", duid);
	shell_print(sh, "  openssl req -x509 -new -key %s-key.pem -out %s-cert.pem \\", duid, duid);
	shell_print(sh, "      -days 3650 -subj \"/CN=%s\"", duid);
	shell_print(sh, "");
	shell_print(sh, "STEP 2  Create the device in the IOTCONNECT web portal:");
	shell_print(sh, "  Devices -> Create Device");
	shell_print(sh, "    Unique ID  : %s   (must match the cert CN)", duid);
	shell_print(sh, "    Auth type  : Self-Signed");
	shell_print(sh, "    Certificate: paste the contents of %s-cert.pem", duid);
	shell_print(sh, "    Template   : your telemetry template");
	shell_print(sh, "");
	shell_print(sh, "STEP 3  Load the credentials onto this board (base64, one line each):");
	shell_print(sh, "  base64 -w0 %s-cert.pem    then:  iotc cred cert <paste>", duid);
	shell_print(sh, "  base64 -w0 %s-key.pem     then:  iotc cred key  <paste>", duid);
	shell_print(sh, "");
	shell_print(sh, "STEP 4  Connect:  kernel reboot cold   (reconnects as '%s')", duid);
	shell_print(sh, "");
	shell_print(sh, "Check stored identity anytime with:  iotc cred show");
	return 0;
}

/* --- `iotc config`: paste iotcDeviceConfig.json to set cpid/env/duid -------
 * Uses shell bypass mode so the pasted JSON (with quotes/spaces/newlines) is
 * captured verbatim, then parsed with cJSON. */
static char cfg_buf[512];
static size_t cfg_len;
static int cfg_depth;
static bool cfg_started;

static void store_field(const char *key, const char *val, char *mirror, size_t mirror_sz)
{
	(void)iotc_kv_save(key, val, strlen(val));
	strncpy(mirror, val, mirror_sz - 1);
	mirror[mirror_sz - 1] = '\0';
}

static void config_apply(const struct shell *sh, const char *json)
{
	cJSON *root = cJSON_Parse(json);
	const cJSON *cpid, *env, *uid, *pf, *disc;

	if (root == NULL) {
		shell_error(sh, "invalid JSON");
		return;
	}
	cpid = cJSON_GetObjectItemCaseSensitive(root, "cpid");
	env = cJSON_GetObjectItemCaseSensitive(root, "env");
	uid = cJSON_GetObjectItemCaseSensitive(root, "uid");
	if (!cJSON_IsString(uid)) {
		uid = cJSON_GetObjectItemCaseSensitive(root, "did");
	}
	pf = cJSON_GetObjectItemCaseSensitive(root, "pf");
	disc = cJSON_GetObjectItemCaseSensitive(root, "disc");

	if (cJSON_IsString(cpid)) {
		store_field("cpid", cpid->valuestring, s_cpid, sizeof(s_cpid));
	}
	if (cJSON_IsString(env)) {
		store_field("env", env->valuestring, s_env, sizeof(s_env));
	}
	if (cJSON_IsString(uid)) {
		store_field("duid", uid->valuestring, s_duid, sizeof(s_duid));
	}
	shell_print(sh, "Stored from iotcDeviceConfig.json: cpid=%s env=%s duid=%s",
		    s_cpid, s_env, s_duid);
	shell_print(sh, "Cloud + discovery host are set at BUILD time -- verify they match:");
	if (cJSON_IsString(pf)) {
		shell_print(sh, "  json cloud=%s   build=%s", pf->valuestring,
			    IS_ENABLED(CONFIG_IOTCONNECT_CT_AWS) ? "aws" : "azure");
	}
	if (cJSON_IsString(disc)) {
		shell_print(sh, "  json disc =%s   build=%s", disc->valuestring,
			    CONFIG_IOTCONNECT_DRA_DISCOVERY_HOST);
	}
	cJSON_Delete(root);
}

static void cfg_bypass(const struct shell *sh, uint8_t *data, size_t len, void *user_data)
{
	ARG_UNUSED(user_data);
	for (size_t i = 0; i < len; i++) {
		char c = (char)data[i];

		if (cfg_len < sizeof(cfg_buf) - 1) {
			cfg_buf[cfg_len++] = c;
		}
		if (c == '{') {
			cfg_depth++;
			cfg_started = true;
		} else if (c == '}' && cfg_started) {
			if (--cfg_depth <= 0) {
				cfg_buf[cfg_len] = '\0';
				shell_set_bypass(sh, NULL, NULL);
				config_apply(sh, cfg_buf);
				return;
			}
		}
	}
	if (cfg_len >= sizeof(cfg_buf) - 1) {
		shell_set_bypass(sh, NULL, NULL);
		shell_error(sh, "config too long / no closing brace");
	}
}

static int cmd_config(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	cfg_len = 0;
	cfg_depth = 0;
	cfg_started = false;
	shell_print(sh, "Paste iotcDeviceConfig.json (the { ... } block) now:");
	shell_set_bypass(sh, cfg_bypass, NULL);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(iotc_cred_cmds,
	SHELL_CMD_ARG(cpid, NULL, "<cpid>  set company id", cmd_cpid, 2, 0),
	SHELL_CMD_ARG(env, NULL, "<env>  set environment", cmd_env, 2, 0),
	SHELL_CMD_ARG(duid, NULL, "<duid>  set device unique id", cmd_duid, 2, 0),
	SHELL_CMD_ARG(cert, NULL, "<base64-PEM>  set device certificate", cmd_cert, 2, 0),
	SHELL_CMD_ARG(key, NULL, "<base64-PEM>  set device private key", cmd_key, 2, 0),
	SHELL_CMD(show, NULL, "show provisioned identity", cmd_show),
	SHELL_CMD(clear, NULL, "erase provisioned identity", cmd_clear),
	SHELL_SUBCMD_SET_END
);
SHELL_STATIC_SUBCMD_SET_CREATE(iotc_cmds,
	SHELL_CMD_ARG(setup, NULL, "<duid> [cpid] [env]  guided device onboarding", cmd_setup, 2, 2),
	SHELL_CMD(config, NULL, "paste iotcDeviceConfig.json to set cpid/env/duid", cmd_config),
	SHELL_CMD(cred, &iotc_cred_cmds, "manual credential set/show/clear", NULL),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(iotc, &iotc_cmds, "IOTCONNECT device provisioning", NULL);

#endif /* CONFIG_IOTCONNECT_SHELL */
