# Click telemetry sample: auto-detects MikroE Click sensor boards on a
# mikroBUS Shuttle and publishes per-channel telemetry.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(iotc_click_telemetry_sample)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../telemetry/src     # device_credentials.h (non-TF-M)
  ${CMAKE_CURRENT_SOURCE_DIR}/../quickstart/src)   # quickstart_credentials.h (TF-M /ns: public CAs only)
