# Porting to a new board (the consolidation guide)

The entire point of `iotc-zephyr-sdk` is that **adding a board is a
configuration exercise, not a code fork**. The public API
(`include/iotconnect.h`), the orchestrator (`lib/iotconnect.c`) and the Zephyr
transport seam (`lib/zephyr-layer/*`) are vendor-neutral. They talk only to
generic Zephyr subsystems (net sockets, MQTT, TLS credentials, HTTP client,
SNTP, settings). A new board supplies:

1. a Zephyr board definition (already upstream for supported boards), and
2. `samples/<sample>/boards/<board>.conf` + an optional devicetree overlay.

That is the whole consolidation lever: NXP MCX, Nordic, Renesas RA and ST all
share one SDK; only the bearer/credential plumbing differs, and that lives in
Kconfig + devicetree.

## The board file (`boards/<board>.conf`)

This file selects the **network bearer** and any board-specific TLS/flash
tuning. The SDK does not care how packets reach the broker -- it only needs an
up interface with L4 connectivity and a set realtime clock.

Common bearer choices:

| Bearer            | Key Kconfig                                              |
|-------------------|---------------------------------------------------------|
| Ethernet          | `CONFIG_NET_L2_ETHERNET=y`, board PHY driver            |
| Wi-Fi             | `CONFIG_WIFI=y`, `CONFIG_NET_L2_WIFI_MGMT=y`            |
| Cellular (PPP)    | `CONFIG_MODEM_CELLULAR=y`, `CONFIG_NET_L2_PPP=y`        |

The FRDM-MCXN947 reference uses the cellular/PPP path (see
`samples/telemetry/boards/frdm_mcxn947_mcxn947_cpu0.conf`), mirroring the real
`iotc-mcx-zephyr-demos` head-start.

## The devicetree overlay

Use the overlay to wire board-specific hardware the bearer needs -- e.g. a modem
node and its `modem` alias, `mdm-power-gpios`, and deleting unused nodes:

```dts
/ {
    aliases { modem = &modem; };
};

/delete-node/ &enet;            /* disable on-board Ethernet */

&flexcomm1_lpuart1 {
    status = "okay";
    modem: modem {
        compatible = "quectel,bg95"; /* swap per modem; generic driver picks impl */
        mdm-power-gpios = <&gpio1 3 GPIO_ACTIVE_LOW>;
    };
};
```

To retarget a different modem you change the `compatible` string and the APN --
not any SDK source.

## Per-vendor notes

### NXP MCX (reference)
- HAL: add `hal_nxp` to the west allowlist.
- Bearer in the head-start demo is cellular over PPP; Ethernet variants just
  flip the bearer Kconfig.

### ST / Renesas RA
- Add `hal_st` / `hal_renesas` to the west allowlist.
- Pick the bearer (Ethernet/Wi-Fi/cellular) in `boards/<board>.conf`.
- No SDK code changes.

### Nordic
- Mainline Nordic boards with Ethernet/Wi-Fi work as-is on Zephyr LTS.
- **nRF91 cellular stays NCS-coupled**: the nRF91 modem uses offloaded sockets
  + modem TLS + modem key management (sec_tags in the modem) and the nRF Modem
  Library. That bearer is NOT vendor-neutral Zephyr and is built in the Nordic
  Connect SDK (NCS) tree, not against this manifest's mainline LTS. Treat nRF91
  as a separate downstream; the IOTCONNECT public API still applies, but the
  transport seam's TLS-credential and socket assumptions differ. Keep nRF91 in
  the existing `iotc-nrf-sdk` until/unless a mainline offload abstraction lands.

## Credentials and secure storage (PSA)

By default `tls_credential_add()` uses the volatile RAM backend -- credentials
are re-registered each boot from the PEM blobs passed in
`IotConnectClientConfig.auth_info`. For keys that must persist and resist
physical attack:

- set `CONFIG_IOTCONNECT_USE_PSA_PROTECTED_STORAGE=y`,
- select `CONFIG_TLS_CREDENTIALS_BACKEND_PROTECTED_STORAGE=y` (requires
  `CONFIG_BUILD_WITH_TFM=y`, a TF-M-capable board),
- provision the device cert/key once into PSA Protected Storage and leave the
  `auth_info` cert/key pointers NULL so the SDK references the stored tag
  instead of re-adding RAM copies.

This is the same sec-tag contract on every board; only the backend differs, so
secure-element / TF-M boards consolidate behind one toggle.

## Checklist for a new board

- [ ] Board supported upstream in Zephyr (or add a board definition).
- [ ] Vendor HAL added to `west.yml` allowlist.
- [ ] `samples/telemetry/boards/<board>.conf` selects the bearer.
- [ ] Devicetree overlay wires bearer hardware (if any).
- [ ] Realtime clock source available (SNTP works once the bearer is up).
- [ ] Flash partition `storage_partition` exists (for settings/NVS) or
      `CONFIG_SETTINGS_NVS` disabled.
- [ ] Build: `west build -b <board> samples/telemetry`. No SDK edits.
