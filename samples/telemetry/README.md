# /IOTCONNECT Telemetry Sample

Connects to Avnet /IOTCONNECT over MQTT/TLS, bootstraps the broker connection
with the Device REST API (discovery -> identity), publishes periodic telemetry,
and acknowledges cloud-to-device commands. This is the canonical reference app
and mirrors the IOTCONNECT generic-SDK `main.c` skeleton.

## Reference target: FRDM-MCXN947

```sh
west build -b frdm_mcxn947/mcxn947/cpu0 \
  modules/lib/iotc-zephyr-sdk/samples/telemetry
west flash
```

The board file `boards/frdm_mcxn947_mcxn947_cpu0.conf` carries the
bearer-specific configuration (cellular modem over PPP, mirroring the real
`iotc-mcx-zephyr-demos` networking story). The board overlay (modem node,
Ethernet deletion, APN) lives alongside it in your board overlay; see that file
and `docs/porting-new-board.md`.

## Configuration

Device identity and connection type are set in `prj.conf`:

```
CONFIG_IOTCONNECT_CPID="..."
CONFIG_IOTCONNECT_ENV="..."
CONFIG_IOTCONNECT_DUID="..."
CONFIG_IOTCONNECT_CT_AWS=y   # or CONFIG_IOTCONNECT_CT_AZURE=y
```

TLS credentials (broker CA, device cert + key, DRA CA) are passed in
`src/main.c` via the `IotConnectClientConfig.auth_info` PEM fields. Replace the
placeholder PEM arrays with your provisioned device certificate and key. For
secure-element / PSA storage, set
`CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE=y` and provision out-of-band.

## Flow

1. Bring up the network interface and wait for L4 connectivity.
2. `iotc_time_sync()` (SNTP) sets the realtime clock so TLS cert checks pass.
3. `iotconnect_sdk_init_config()` / fill config / `iotconnect_sdk_init()`
   (runs DRA discovery + identity).
4. `iotconnect_sdk_connect()` (registers creds, MQTT/TLS connect, subscribe).
5. Loop: build telemetry with the iotc-c-lib builder and send; the command
   callback acks inbound commands.
6. `iotconnect_sdk_disconnect()` / `iotconnect_sdk_deinit()`.

## Other boards

To target Nordic / Renesas RA / ST, add a `boards/<board>.conf` (+ overlay) and
build for that board. You do not modify the SDK. See
`docs/porting-new-board.md`.
