# iotc-zephyr-sdk top-level module CMakeLists.txt
#
# Compiles to nothing unless CONFIG_IOTCONNECT=y. Builds three things into one
# Zephyr library:
#   1. the platform-independent orchestrator (lib/iotconnect.c)
#   2. the Zephyr transport seam (lib/zephyr-layer/*.c)
#   3. the portable iotc-c-lib core + DRA module + bundled cJSON
#
# iotc-c-lib has its own standalone CMake project, but for a Zephyr build we
# compile its sources directly so they land in the same library/partition and
# so its log macros can be overridden to Zephyr LOG_* via IOTCL_USER_CONFIG_FILE.

if(CONFIG_IOTCONNECT)

  # Locate the iotc-c-lib west project. Accept it from a CMake cache var
  # (-DZEPHYR_IOTC_C_LIB_MODULE_DIR=...), then the environment (same name), then
  # fall back to the conventional west workspace path.
  set(IOTC_C_LIB_DIR ${ZEPHYR_IOTC_C_LIB_MODULE_DIR})
  if(NOT IOTC_C_LIB_DIR)
    set(IOTC_C_LIB_DIR $ENV{ZEPHYR_IOTC_C_LIB_MODULE_DIR})
  endif()
  if(NOT IOTC_C_LIB_DIR)
    # Fallback to the conventional workspace path if the module var is unset.
    set(IOTC_C_LIB_DIR ${ZEPHYR_BASE}/../modules/lib/iotc-c-lib)
  endif()

  zephyr_library()

  # --- Public + internal include paths --------------------------------------
  zephyr_include_directories(include)
  zephyr_include_directories(lib/zephyr-layer)
  zephyr_include_directories(${IOTC_C_LIB_DIR}/core/include)
  zephyr_include_directories(${IOTC_C_LIB_DIR}/modules/device-rest-api)
  zephyr_include_directories(${IOTC_C_LIB_DIR}/lib/cJSON)

  # --- This SDK: orchestrator + Zephyr transport seam -----------------------
  zephyr_library_sources(
    lib/iotconnect.c
    lib/zephyr-layer/iotc_mqtt_client.c
    lib/zephyr-layer/iotc_tls_credentials.c
    lib/zephyr-layer/iotc_time.c
  )
  zephyr_library_sources_ifdef(CONFIG_IOTCONNECT_DRA
    lib/zephyr-layer/iotc_dra_client.c
  )
  zephyr_library_sources_ifdef(CONFIG_IOTCONNECT_IDENTITY_NVS
    lib/zephyr-layer/iotc_identity.c
  )
  zephyr_library_sources_ifdef(CONFIG_IOTCONNECT_ONDEVICE_KEYGEN
    lib/zephyr-layer/iotc_provision.c
  )
  zephyr_library_sources_ifdef(CONFIG_IOTCONNECT_OTA_MCUBOOT
    lib/zephyr-layer/iotc_ota.c
  )
  zephyr_library_sources_ifdef(CONFIG_IOTCONNECT_WIFI_AUTOCONNECT
    lib/zephyr-layer/iotc_wifi_autoconnect.c
  )
  zephyr_library_sources_ifdef(CONFIG_IOTCONNECT_DEVICE_VITALS
    lib/zephyr-layer/iotc_device_vitals.c
  )
  zephyr_library_sources_ifdef(CONFIG_IOTCONNECT_FILE_UPLOAD
    lib/zephyr-layer/iotc_file_upload.c
  )

  # --- iotc-c-lib protocol core (always required) ---------------------------
  zephyr_library_sources(
    ${IOTC_C_LIB_DIR}/core/src/iotcl.c
    ${IOTC_C_LIB_DIR}/core/src/iotcl_c2d.c
    ${IOTC_C_LIB_DIR}/core/src/iotcl_internal.c
    ${IOTC_C_LIB_DIR}/core/src/iotcl_telemetry.c
    ${IOTC_C_LIB_DIR}/core/src/iotcl_util.c
  )

  # --- iotc-c-lib Device REST API module (discovery/identity) ---------------
  zephyr_library_sources_ifdef(CONFIG_IOTCONNECT_DRA
    ${IOTC_C_LIB_DIR}/modules/device-rest-api/iotcl_dra_discovery.c
    ${IOTC_C_LIB_DIR}/modules/device-rest-api/iotcl_dra_identity.c
    ${IOTC_C_LIB_DIR}/modules/device-rest-api/iotcl_dra_url.c
    ${IOTC_C_LIB_DIR}/modules/device-rest-api/iotcl_dra_json_config.c
    ${IOTC_C_LIB_DIR}/modules/device-rest-api/iotcl_dra_credentials.c
  )

  # --- cJSON (git submodule of iotc-c-lib) ----------------------------------
  # west.yml sets `submodules: true` on the iotc-c-lib project so `west update`
  # checks this out. Fail early with an actionable message if it is missing.
  if(NOT EXISTS ${IOTC_C_LIB_DIR}/lib/cJSON/cJSON.c)
    message(FATAL_ERROR
      "cJSON not found at ${IOTC_C_LIB_DIR}/lib/cJSON/cJSON.c.\n"
      "iotc-c-lib bundles cJSON as a git submodule. Run `west update` "
      "(the iotc-c-lib project sets submodules:true), or "
      "`git -C ${IOTC_C_LIB_DIR} submodule update --init lib/cJSON`.")
  endif()
  zephyr_library_sources(
    ${IOTC_C_LIB_DIR}/lib/cJSON/cJSON.c
  )

  # --- Route iotc-c-lib logging through Zephyr LOG_* ------------------------
  # iotcl_log.h honors IOTCL_USER_CONFIG_FILE (MbedTLS-style). We point it at a
  # header in this module that redefines IOTCL_ERROR/WARN/INFO to LOG_*.
  zephyr_library_compile_definitions(
    IOTCL_USER_CONFIG_FILE="iotc_iotcl_user_config.h"
  )

endif() # CONFIG_IOTCONNECT
