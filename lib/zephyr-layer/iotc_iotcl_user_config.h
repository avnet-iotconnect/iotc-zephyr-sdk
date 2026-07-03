/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * iotc-c-lib user config (MbedTLS-style override header).
 *
 * Wired up via the CMake compile definition:
 *     IOTCL_USER_CONFIG_FILE="iotc_iotcl_user_config.h"
 *
 * iotcl_log.h includes this file before defining its own IOTCL_ERROR/WARN/INFO
 * and IOTCL_ENDLN macros (each is wrapped in #ifndef), so redefining them here
 * routes all iotc-c-lib logging through the Zephyr logging subsystem instead of
 * bare printf(). IOTCL_ENDLN is blanked because Zephyr's LOG_* already append a
 * newline.
 *
 * The iotc-c-lib macros are printf-style:
 *     IOTCL_ERROR(err_code, fmt, ...)
 *     IOTCL_WARN(err_code,  fmt, ...)
 *     IOTCL_INFO(fmt, ...)
 * Zephyr's LOG_ERR/LOG_WRN/LOG_INF are also printf-style, so we forward the
 * format + varargs directly. The error code is prefixed so it stays visible.
 */
#ifndef IOTC_IOTCL_USER_CONFIG_H
#define IOTC_IOTCL_USER_CONFIG_H

#include <zephyr/sys/printk.h>

/*
 * Route iotc-c-lib logging to printk(), NOT the Zephyr LOG_* macros.
 *
 * iotc-c-lib's logging macros are expanded inside the library's own .c files
 * (iotcl.c, iotcl_c2d.c, ...), which are vendor-neutral and do NOT call
 * LOG_MODULE_REGISTER/LOG_MODULE_DECLARE. The deferred LOG_* macros require a
 * per-translation-unit log module symbol, so they fail to compile there.
 * printk() has no such requirement and always reaches the console.
 *
 * TODO: for deferred/filtered logging, replace these with a thin shim function
 * (declared here, defined once in a .c that registers a log module).
 */
#define IOTCL_ENDLN "\n"

#define IOTCL_ERROR(err_code, ...)                                             \
	do {                                                                   \
		printk("[iotcl][E][%d] ", (int)(err_code));                    \
		printk(__VA_ARGS__);                                           \
		printk("\n");                                                  \
	} while (0)

#define IOTCL_WARN(err_code, ...)                                              \
	do {                                                                   \
		printk("[iotcl][W][%d] ", (int)(err_code));                    \
		printk(__VA_ARGS__);                                           \
		printk("\n");                                                  \
	} while (0)

#define IOTCL_INFO(...)                                                        \
	do {                                                                   \
		printk("[iotcl][I] ");                                         \
		printk(__VA_ARGS__);                                           \
		printk("\n");                                                  \
	} while (0)

#endif /* IOTC_IOTCL_USER_CONFIG_H */
