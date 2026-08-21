# NVS device provisioning

The IOTCONNECT Zephyr SDK can load a device's identity (CPID / environment /
DUID + device certificate and private key) from **Zephyr settings backed by
NVS** instead of compiling it in. This mirrors the Python Lite SDK's
"identity is data, not code" model in a way that fits MCUs with no filesystem:

- Provision **once** over the serial console; credentials persist in flash.
- Credentials **survive an application reflash** (they live in the settings
  partition, not the app image).
- **Swap a device's identity without recompiling** — useful for fleets and for
  moving a board between IOTCONNECT devices/environments.

The broker and DRA **CA roots stay compiled-in** — they are public and shared
across devices; only the per-device secrets live in NVS.

## Enable it

```ini
# Identity source
CONFIG_IOTCONNECT_IDENTITY_NVS=y
CONFIG_IOTCONNECT_SHELL=y      # the `iotc cred` provisioning commands

# Settings backend (NVS on the storage partition)
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y

# Console for provisioning. The base64 cert/key are long (~1.6 KB cert, ~2.2 KB
# for an RSA key) SINGLE lines, so the shell command buffer must be enlarged far
# beyond its 256-byte default, and (optionally) the serial RX ring buffer too.
CONFIG_SHELL=y
CONFIG_SHELL_CMD_BUFF_SIZE=3072
CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE=4096

# So `kernel reboot cold` can reload identity after provisioning (or press RESET)
CONFIG_REBOOT=y
CONFIG_KERNEL_SHELL=y
```

> **Pasting caveat (verified on RT1170):** even with a large command buffer, a
> terminal that dumps the whole ~2 KB base64 line at once can overflow the shell
> serial RX before it drains. Either bump
> `CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE` (above) *and/or* paste in
> small chunks (a scripted 24-byte/12 ms feed works reliably).

## Shell command reference

| Command | What it does |
|---|---|
| `iotc setup <duid> [cpid] [env]` | **Guided onboarding.** Stores duid (+ optional cpid/env) to NVS and prints a personalized, step-by-step recipe. `cpid`/`env` default to the currently-stored or compiled-in values. **`<duid>` is required** (`iotc setup` alone → "wrong parameter count"). |
| `iotc cred cpid <cpid>` | Set the company id (CPID) in NVS. |
| `iotc cred env <env>` | Set the environment in NVS. |
| `iotc cred duid <duid>` | Set the device unique id in NVS. |
| `iotc cred cert <base64-PEM>` | Set the device certificate (single-line base64 of the PEM). |
| `iotc cred key <base64-PEM>` | Set the device private key (single-line base64 of the PEM). |
| `iotc cred show` | Print the stored identity (private key never echoed). |
| `iotc cred clear` | Erase the stored identity (app then falls back to compiled-in). |
| `iotcprov keygen <duid>` | *(needs `CONFIG_IOTCONNECT_ONDEVICE_KEYGEN`)* Generate an EC P-256 key **and** self-signed cert **on the device** and **print** them (does not store — for inspection/testing). |
| `iotcprov provision <duid> [cpid] [env]` | *(needs `CONFIG_IOTCONNECT_ONDEVICE_KEYGEN` + NVS)* Generate the key+cert **on the device**, **store the full identity in NVS**, and print the cert to register. The private key is created on-chip. This is the on-device onboarding path. |
| `kernel reboot cold` | Restart so the app reloads identity from NVS (or press RESET). |

## Onboarding sequences

### A. Guided — `iotc setup` (recommended)

On the board:

```
iotc setup rt1170zeph
```

The board stores the identity and prints the next steps. Do them on your PC:

```sh
openssl ecparam -name prime256v1 -genkey -noout -out rt1170zeph-key.pem
openssl req -x509 -new -key rt1170zeph-key.pem -out rt1170zeph-cert.pem \
    -days 3650 -subj "/CN=rt1170zeph"
# Create the device in the /IOTCONNECT portal (Self-Signed) and paste rt1170zeph-cert.pem
base64 -w0 rt1170zeph-cert.pem     # copy the output
base64 -w0 rt1170zeph-key.pem      # copy the output
```

Back on the board:

```
iotc cred cert <paste cert base64>
iotc cred key  <paste key base64>
iotc cred show
kernel reboot cold
```

### B. Manual — `iotc cred` only

If you already have the cert/key PEMs and `iotcDeviceConfig.json`:

```
iotc cred cpid <YOUR_CPID>
iotc cred env  poc
iotc cred duid rt1170zeph
iotc cred cert <base64 of device-cert.pem>
iotc cred key  <base64 of device-key.pem>
iotc cred show
kernel reboot cold
```

### C. On-device key generation — `iotcprov provision` (no PC keys)

The device generates its **own** key + cert — no `openssl`, no key on a PC:

```
iotcprov provision rt1170zeph
```

This creates an EC P-256 keypair on-chip, self-signs a cert (`CN=rt1170zeph`),
**stores the full identity in NVS**, and prints the certificate. Then:

1. Register the printed certificate in the /IOTCONNECT portal (Create Device
   `rt1170zeph`, Self-Signed, paste the cert).
2. `kernel reboot cold` → the board connects as `rt1170zeph` using the key it
   generated itself.

`iotcprov keygen <duid>` does the same generation but only **prints** (no NVS
store) — useful to inspect what the device produces.

> **Storage by build:** on **non-TF-M** boards the generated key is stored in
> NVS in the clear (device-generated, not hardware-sealed). On a **TF-M `/ns`**
> build (FRDM-MCXN947) the identity is instead sealed in **hardware-backed PSA
> Protected Storage** — HW-verified end-to-end (provision → seal → reboot →
> connect). A further hardening step would keep the key **non-exportable** and
> have TLS reference it by PSA key id so it never materializes in RAM (needs a
> Zephyr TLS opaque-key type; see below).

The application should try NVS first and fall back to compiled-in credentials
(the SDK `samples/telemetry` already does this):

```c
struct iotc_identity id;
if (iotc_identity_load(&id) == 0) {           /* provisioned in NVS */
    config.cpid = (char *)id.cpid;
    config.env  = (char *)id.env;
    config.duid = (char *)id.duid;
    config.auth_info.data.cert_info.device_cert     = id.device_cert;
    config.auth_info.data.cert_info.device_cert_len = id.device_cert_len;
    config.auth_info.data.cert_info.device_key      = id.device_key;
    config.auth_info.data.cert_info.device_key_len  = id.device_key_len;
} else {                                        /* fall back to header */
    /* ... device_credentials.h ... */
}
```

## Provision a device

1. **Get the identity strings.** Download `iotcDeviceConfig.json` from the
   device's Info panel in the IOTCONNECT portal; note its `cpid`, `env`, and
   `uid` (DUID).

2. **Base64-encode the cert and key** (single line, no wrapping) on your host:

   ```sh
   base64 -w0 device-cert.pem   # Linux
   base64 -w0 device-key.pem
   # macOS: base64 -i device-cert.pem ; Windows: certutil -encodehex ... or Git-Bash base64
   ```

3. **Paste over the serial console** (115200 8N1), one line each:

   ```
   iotc cred cpid <YOUR_CPID>
   iotc cred env  poc
   iotc cred duid mclMCXNzeph
   iotc cred cert MIIB...<base64 of device-cert.pem>...==
   iotc cred key  MIIE...<base64 of device-key.pem>...==
   iotc cred show
   ```

   `iotc cred show` prints the stored CPID/env/DUID and the cert/key sizes
   (the key is never echoed).

4. **Reset the board.** On boot the app logs
   `Using NVS-provisioned identity (duid=...)` and connects with the stored
   credentials.

`iotc cred clear` erases the stored identity (the app then falls back to the
compiled-in credentials, if any).

## Key protection & TF-M capability (per board)

The device generates its own key on-chip, but **where that key is stored** — and
whether it can be **sealed by hardware** — depends on the board. Hardware key
protection needs ARM **TrustZone-M** (Cortex-M23/M33/M55) and a Zephyr **TF-M**
(`/ns`) board target; the Cortex-M7 boards cannot provide it.

| Board | Core | TF-M (`/ns`) | Secure element | Key protection |
|---|---|:---:|:---:|---|
| **FRDM-MCXN947** | Cortex-M33 | ✅ `frdm_mcxn947/mcxn947/cpu0/ns` (**connect HW-verified end-to-end**) | EdgeLock (ELS) | **Hardware-backed** PSA Protected Storage (sealed at rest) |
| FRDM-MCXW72 | Cortex-M33 | ⚠️ capable, no Zephyr TF-M variant yet | EdgeLock | pending upstream TF-M port |
| FRDM-RW612 | Cortex-M33 | ⚠️ capable, no Zephyr TF-M variant yet | EdgeLock (ELS + PUF) | software only (NVS, **external** FlexSPI flash — note the key leaves the die) |
| FRDM-i.MX93 (M33) | Cortex-M33 | ⚠️ capable | ELE | pending |
| MIMXRT1170-EVKB | Cortex-M7 | ❌ no TrustZone-M | CAAM | software only (NVS) |
| FRDM-MCXE31B | Cortex-M7 | ❌ | — | software only (UART source anyway) |

- **TF-M boards** (build the `/ns` target): `CONFIG_BUILD_WITH_TFM=y` selects the
  PSA-Protected-Storage identity path in `iotc_identity.c`. The whole identity
  (cpid/env/duid + cert/key) is sealed **at rest** as a single PSA PS asset
  (the TF-M PS filesystem reserves a full slot per asset and allows only a few,
  so one packed blob is used). At connect time it is loaded into RAM and added
  as **VOLATILE** TLS credentials for the handshake — do **not** use
  `CONFIG_TLS_CREDENTIALS_BACKEND_PROTECTED_STORAGE`, which would push the CA +
  device creds into the small PS and overflow it. This path is HW-verified
  end-to-end on FRDM-MCXN947. Two Zephyr-module patches are required (see
  `patches/README.md`). A stricter "key never in RAM" model
  (`CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE`, referencing the key by PSA key
  id) additionally needs a Zephyr TLS opaque-key type (see below).
- **M7 boards** (RT1170, MCXE31B): no TrustZone-M, so no TF-M. The strongest
  posture is the on-device-generated key in NVS. Zephyr's *software* secure
  storage (`CONFIG_SECURE_STORAGE`, non-TF-M) can encrypt it at rest, but Zephyr
  documents that protection as "highly dependent… not a guarantee" (the
  key-encryption key is software).
- **All boards:** Zephyr's TLS credential subsystem has no opaque/PSA-key type,
  so a "key never materializes in RAM" model would require a Zephyr core change
  regardless of board.

**Recommendation:** for deployments that require hardware key protection, use a
**TF-M board (FRDM-MCXN947 today)**. The on-device keygen + NVS flow is the
portable baseline that works on every board.

> **Building the `/ns` (TF-M) target** pulls in a TF-M secure image + BL2
> bootloader, which need extra host Python packages beyond Zephyr's base
> requirements: `pip install cryptography cbor2 pyyaml jinja2 click imgtool`.
> A successful build produces `zephyr/tfm_merged.hex` (secure + non-secure,
> flash this) alongside the non-secure `zephyr.hex`. Verified on
> `frdm_mcxn947/mcxn947/cpu0/ns`.

## Notes & limits

- **PEM size vs. NVS sector.** A device cert/key PEM is ~1–2 KB. The settings
  NVS backend stores each value in one entry, so the storage partition's flash
  sector must be large enough (≥ ~2 KB). Internal flash on the MCXN947 (8 KB
  sectors) and the RT1170 external flash (4 KB) both satisfy this. The static
  buffers are 2048 bytes each (`iotc_identity.c`); raise them for larger keys.
- **Security.** Credentials are stored in NVS in the clear. For hardware-backed
  key protection, use `CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE` / a secure
  key store and provision the key under a PSA key id instead (out of scope of
  this simple NVS flow).
- Provisioning is transport-agnostic — any `iotc cred ...` entry path works
  (UART shell shown here; MCUmgr/settings upload also possible).
