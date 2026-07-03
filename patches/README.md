# Zephyr-module edits for TF-M (`/ns`) builds

These edits touch upstream Zephyr modules (which `west update` restores to their
manifest revision), so **re-apply them after any `west update`**. They are
**not** needed for non-TF-M boards.

## Required — TLS handshake fix

| Patch | Module | Why |
|---|---|---|
| `mbedtls-ssl-premaster-ecp-max-bytes.patch` | `modules/crypto/mbedtls` | On PSA-only builds the legacy ECP module is disabled, so `MBEDTLS_ECP_MAX_BYTES` collapses to **1**. That sizes the TLS ECDHE premaster buffer (`union mbedtls_ssl_premaster_secret._pms_ecdh`) to 1 byte, and `psa_raw_key_agreement()` rejects the 32-byte P-256 shared secret with `PSA_ERROR_BUFFER_TOO_SMALL` → handshake aborts (mbedTLS `-0x7F80`). The patch floors the buffer at 66 bytes (P-521). |

## Required to build the N947 `/ns` image from source — secure/NS RAM rebalance

The default TF-M split is secure 192 KB / non-secure **128 KB** of the 320 KB
`sram0`. The N947 `/ns` quickstart config uses ~134 KB of NS RAM (mbedTLS heap +
k-heap + RX pool + stacks), so it needs the boundary moved to **0x20028000**
(secure 160 KB / NS **160 KB**). Heavier `/ns` demos (`click-telemetry`) use the
same headroom. Apply **both edits together** — the TF-M MPC/SAU boundary
(`region_defs.h`) and the Zephyr board `sram0` split (DTS) must match, or the NS
app faults on RAM it can't reach. (Flash-only users of the prebuilt
`tfm_merged.hex` need nothing — the boundary is baked into the image.)

| Patch | Module | Why |
|---|---|---|
| `tfm-frdmmcxn947-ram-rebalance-region-defs.patch` | `modules/tee/tf-m/trusted-firmware-m` | `S_DATA_SIZE 0x30000 → 0x28000`: hands the extra 32 KB to the non-secure world (TF-M secure only uses ~117 KB). Sets the MPC/SAU secure/NS split. |
| `zephyr-frdmmcxn947-ns-ram-rebalance-dts.patch` | `zephyr` | Matching board DTS: `non_secure_ram` → `0x20028000`, 160 KB (and `secure_ram` 160 KB). |

## Apply

```sh
# Required (all TF-M /ns builds):
cd <zephyrproject>/modules/crypto/mbedtls
git apply <path>/iotc-zephyr-sdk/patches/mbedtls-ssl-premaster-ecp-max-bytes.patch

# Optional (only if building click-telemetry or other RAM-heavy /ns demos):
cd <zephyrproject>/modules/tee/tf-m/trusted-firmware-m
git apply <path>/iotc-zephyr-sdk/patches/tfm-frdmmcxn947-ram-rebalance-region-defs.patch
cd <zephyrproject>/zephyr
git apply <path>/iotc-zephyr-sdk/patches/zephyr-frdmmcxn947-ns-ram-rebalance-dts.patch
```

Then do a pristine build (`west build -p always ...`) so the TF-M secure image
picks up the boundary change.

> **Note:** an earlier `tfm-frdmmcxn947-crypto-engine-buf.patch` (bumping
> `CRYPTO_ENGINE_BUF_SIZE`) was a red herring and has been dropped — the default
> 0x3000 buffer handles RSA-2048 cert verification fine. The real handshake fix
> is the premaster-union sizing above.
>
> These are pragmatic fixes pending upstream resolution (the premaster sizing is
> a known mbedTLS/TF-PSA-Crypto integration gap). Track them if you bump the
> Zephyr/mbedTLS/TF-M revision.
