# iotc-zephyr-sdk

Vendor-neutral [Avnet /IOTCONNECT](https://www.iotconnect.io/) SDK for the
[Zephyr RTOS](https://www.zephyrproject.org/), packaged as a west-importable
Zephyr **module** layered on top of the portable
[`iotc-c-lib`](https://github.com/avnet-iotconnect/iotc-c-lib) protocol core.

This SDK is the **maintenance-consolidation** layer for the IOTCONNECT embedded
family: a single Zephyr code base that targets NXP MCX, Nordic, Renesas RA and ST
silicon. Bringing up a new board is a `boards/<board>.conf` + devicetree overlay
exercise, **not** a new SDK fork. See [docs/porting-new-board.md](docs/porting-new-board.md).

## Architecture

```
  +-------------------------------------------------------------+
  |  application (samples/telemetry/src/main.c)                 |
  +-------------------------------------------------------------+
  |  PUBLIC API   include/iotconnect.h                          |  family contract
  |               include/iotconnect_telemetry.h                |
  +-------------------------------------------------------------+
  |  ORCHESTRATOR lib/iotconnect.c                              |  platform-independent glue
  |   DRA discovery/identity -> creds -> MQTT connect -> pump   |
  +-------------------------------------------------------------+
  |  ZEPHYR TRANSPORT  lib/zephyr-layer/                        |  the only board-aware seam
  |   iotc_mqtt_client.c  (zephyr/net/mqtt.h + TLS sec tags)    |
  |   iotc_dra_client.c   (zephyr/net/http/client.h)           |
  |   iotc_tls_credentials.c (tls_credentials, PSA-friendly)   |
  |   iotc_time.c         (zephyr/net/sntp.h)                   |
  +-------------------------------------------------------------+
  |  PROTOCOL CORE   iotc-c-lib (core + modules/device-rest-api)|  portable, unchanged
  |   iotcl_*  (telemetry JSON, C2D parse, DRA URL build/parse) |
  +-------------------------------------------------------------+
```

The public API and the orchestrator are vendor-neutral. Only the four files in
`lib/zephyr-layer/` touch hardware-adjacent Zephyr subsystems, and even those are
driven entirely by Kconfig + devicetree, so a board change never edits `.c`.

## Layout

```
  west.yml                         standalone manifest (zephyr 4.4 + iotc-c-lib + cJSON)
  zephyr/module.yml                makes this a Zephyr module
  CMakeLists.txt                   zephyr_library, guarded by CONFIG_IOTCONNECT
  Kconfig                          menuconfig IOTCONNECT + sub-options
  include/iotconnect.h             public family API
  include/iotconnect_telemetry.h   thin telemetry convenience over iotc-c-lib
  lib/iotconnect.c                 lifecycle orchestrator
  lib/zephyr-layer/*.{c,h}         Zephyr transport seam
  samples/telemetry/               reference app (FRDM-MCXN947 first target)
  samples/quickstart/              flash-and-provision app (on-device keygen, NVS identity)
  samples/c2d-led/                 cloud-to-device LED control
  samples/click-telemetry/         MikroE Click sensor auto-detect telemetry
  docs/porting-new-board.md        consolidation / new-board guide
```

## Getting started

```sh
# Create a workspace from this manifest repo
west init -m https://github.com/avnet-iotconnect/iotc-zephyr-sdk --mr main my-workspace
cd my-workspace
west update

# Build the telemetry sample for the first reference target
# (this manifest repo is checked out at <workspace>/iotc-zephyr-sdk per west.yml `self`)
west build -b frdm_mcxn947/mcxn947/cpu0 iotc-zephyr-sdk/samples/telemetry
```

Fill in your device identity (CPID / env / DUID) and credentials via Kconfig
(`CONFIG_IOTCONNECT_CPID`, `CONFIG_IOTCONNECT_ENV`, `CONFIG_IOTCONNECT_DUID`) or
at runtime through the public `IotConnectClientConfig`. See the sample README.

## Zephyr version

Validated against Zephyr **v4.4.1** with **Zephyr SDK 1.0.1**. The pin moved up
from the original 3.7 LTS scaffold because the consolidation target set includes
2025 silicon (e.g. NXP MCX E) that only exists in Zephyr ≥ 4.2. The transport
seam was migrated to the 4.x APIs (PSA-Crypto mbedTLS Kconfig, `net_mgmt`
`uint64_t` events, `mqtt_disconnect(c, NULL)`, `zsock_*` sockets, `int`-returning
HTTP response callback).

## Status

**Builds, flashes, and connects on the FRDM-MCXN947 over Ethernet** (verified
2026-06-24). The board's ENET-QoS MAC + LAN8741 PHY are enabled by
`samples/telemetry/boards/frdm_mcxn947_mcxn947_cpu0.{conf,overlay}`; the sample
takes DHCP, runs DRA discovery/identity, then MQTT-over-TLS to IOTCONNECT (AWS).

Two ways to build:

```sh
# A) In an existing Zephyr 4.4 workspace (what the reference build uses):
west build -p always -b frdm_mcxn947/mcxn947/cpu0 -d build/iotc_n947 \
  <path>/iotc-zephyr-sdk/samples/telemetry \
  -- -DZEPHYR_EXTRA_MODULES=<path>/iotc-zephyr-sdk \
     -DZEPHYR_IOTC_C_LIB_MODULE_DIR=<path>/iotc-c-lib

# B) Standalone, from this repo's manifest (see "Getting started" above).
```

Credentials for `samples/telemetry` are generated into a git-ignored
`src/device_credentials.h` (see the sample README); the repo never holds keys.

### Still open

- **`west.yml` pins** for `iotc-c-lib` (`v3.0.0`) and Zephyr should be set to the
  exact reviewed tags for the standalone-manifest path. cJSON is its git
  submodule (`submodules: true`).
- **OTA** is toggled by `CONFIG_IOTCONNECT_OTA` but the MCUboot download path is
  stubbed in the sample callback.
- **Azure symmetric-key (SAS)** auth is in the API surface but only X.509 is
  fully wired in the transport seam.
- **SNTP / time:** TLS needs a valid wall clock; `iotc_time.c` flags an
  RTC-fallback `TODO(board)` for bearers where SNTP is unreliable.
- **IPv4-only** in the MQTT/DRA sockets (`TODO(board)`); IPv6/AF_UNSPEC boards
  need the AF selection generalized.
- **iotc-c-lib logging** is routed to `printk` (the deferred `LOG_*` macros can't
  expand in c-lib TUs that don't register a log module); a filtered-logging shim
  is a future improvement.

`grep -rn "TODO" .` enumerates the per-file follow-ups. nRF91 **cellular** is
deliberately out of scope of this manifest (its modem-offloaded sockets/TLS are
NCS-coupled) — see [docs/porting-new-board.md](docs/porting-new-board.md).

## License

MIT. See [LICENSE](LICENSE). `iotc-c-lib` and cJSON carry their own MIT licenses.
