# c2d-led sample

Cloud-to-device LED control. The app connects to Avnet /IOTCONNECT, drives the
board LED (devicetree alias `led0`) from C2D commands, ACKs every command, and
publishes the LED state as periodic telemetry:

| Command sent from IOTCONNECT | Effect |
|---|---|
| `led-on` | LED on |
| `led-off` | LED off |
| `led-toggle` | invert the LED |

Credentials come from the telemetry sample's generated
`../telemetry/src/device_credentials.h` (git-ignored), so one provisioning step
covers both samples — see the SDK's provisioning docs.

Build (reference target):

```sh
west build -b frdm_mcxn947/mcxn947/cpu0 iotc-zephyr-sdk/samples/c2d-led
```

Per-board demo packaging (board matrix, dashboard templates) lives in the
[iotc-zephyr-demos](https://github.com/avnet-iotconnect/iotc-zephyr-demos)
repository.
