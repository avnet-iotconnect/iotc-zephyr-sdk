/*
 * Copyright (c) 2026 Avnet, Inc.  SPDX-License-Identifier: MIT
 *
 * PUBLIC CA roots only -- safe to commit and distribute. The per-device
 * certificate and private key are generated ON the device (iotcprov provision)
 * and stored in NVS; they are NEVER compiled into the quickstart binary.
 * The roots themselves now live in the SDK's shared header.
 */
#ifndef QUICKSTART_CREDENTIALS_H
#define QUICKSTART_CREDENTIALS_H

#include "iotconnect_ca_roots.h"

#endif /* QUICKSTART_CREDENTIALS_H */
