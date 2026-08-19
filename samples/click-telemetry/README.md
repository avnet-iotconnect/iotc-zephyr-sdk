# click-telemetry sample

Auto-detects MikroE Click sensor boards on a mikroBUS Shuttle at power-up and
publishes per-channel telemetry to Avnet /IOTCONNECT. Clicks with in-tree
Zephyr sensor drivers report full readings; recognized Clicks without a driver
are detected via a raw I2C probe and reported present (a documented extension
point).

The board's `mikrobus_i2c` node supplies the bus. Note that some boards route
mikroBUS I2C without pull-ups — fit external ~4.7k resistors or include a Click
that provides them.

Credentials: non-TF-M builds use the telemetry sample's generated
`device_credentials.h`; TF-M (`/ns`) builds use the quickstart flow
(`quickstart_credentials.h`, public CA roots only, key provisioned on-chip).

Build (reference target):

```sh
west build -b frdm_mcxn947/mcxn947/cpu0 iotc-zephyr-sdk/samples/click-telemetry
```

Per-board demo packaging (board matrix, dashboard templates, Click catalog)
lives in the
[iotc-zephyr-demos](https://github.com/avnet-iotconnect/iotc-zephyr-demos)
repository.
