# quickstart sample

A ready-to-flash provisioning binary: no build-time credentials. Flash it, open
the serial console, and onboard the device to your own IOTCONNECT account
entirely from the prompt:

1. `iotcprov provision <duid>` — the device generates its own key and
   self-signed certificate on-chip and prints the certificate PEM.
2. In IOTCONNECT: Create Device (Self-Signed) and paste the certificate.
3. `iotc config` — paste the downloaded `iotcDeviceConfig.json` block to set
   cpid/env/duid.
4. `kernel reboot cold` — the device connects with its NVS-persisted identity.

Only public CA roots are compiled in (`src/quickstart_credentials.h`); the
per-device key never leaves the chip.

Build (reference target):

```sh
west build -b frdm_mcxn947/mcxn947/cpu0 iotc-zephyr-sdk/samples/quickstart
```

Per-board demo packaging (board matrix, prebuilt images) lives in the
[iotc-zephyr-demos](https://github.com/avnet-iotconnect/iotc-zephyr-demos)
repository.
