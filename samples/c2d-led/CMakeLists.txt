# Cloud-to-device LED control sample: C2D commands drive the board LED
# (alias led0) and the LED state reports back as telemetry. Reuses the
# telemetry sample's generated device_credentials.h, so one provisioning
# step covers both samples.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(iotc_c2d_led_sample)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../telemetry/src) # device_credentials.h
