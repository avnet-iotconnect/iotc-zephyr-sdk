# IOTCONNECT Zephyr quickstart -- flash-and-provision binary. No credentials
# baked in: the device generates its own key/cert (iotcprov provision) and
# keeps its identity in NVS. Only public CA roots are compiled in
# (src/quickstart_credentials.h).
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(iotc_quickstart_sample)

target_sources(app PRIVATE src/main.c)
