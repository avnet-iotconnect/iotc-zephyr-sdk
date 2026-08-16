/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * IOTCONNECT "Telemetry Files" device-to-cloud file upload
 * (CONFIG_IOTCONNECT_FILE_UPLOAD).
 *
 * Wire protocol (mirrors the IOTCONNECT Python Lite SDK, AWS backends):
 *   1. The DRA identity response carries the S3 filesystem block when
 *      "File Support" is enabled on the device template:
 *        p.fs.url        AWS IoT credentials-provider endpoint
 *        p.fs.buckets[]  upload bucket name(s)
 *        p.topics.fu     MQTT "file upload" announce topic
 *      iotc-c-lib stores fs.url (IotclMqttConfig.aws.fs_creds_url); the
 *      bucket + fu topic are captured by iotc_fu_identity_hook(), which the
 *      DRA client calls with the raw identity JSON.
 *   2. HTTPS GET fs.url with MUTUAL TLS (device cert/key) and header
 *      "x-amzn-iot-thingname: <client_id>" returns temporary STS
 *      credentials (accessKeyId/secretAccessKey/sessionToken).
 *   3. The file bytes are PUT to
 *        https://<bucket>.s3.<region>.amazonaws.com/device-uploads/<client_id>/<rel_path>
 *      signed with AWS Signature V4 (headers: x-amz-date,
 *      x-amz-content-sha256, x-amz-security-token).
 *   4. A telemetry-shaped record {"url": "<rel_path>", ...custom} is
 *      published on the fu topic; the platform's Telemetry Files panel
 *      lists (and for images, renders) the file. A "cf" value in the
 *      custom JSON appears as the file's Classification.
 */
#ifndef IOTC_FILE_UPLOAD_H
#define IOTC_FILE_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the DRA client with the raw identity-response JSON to capture
 * the upload bucket and fu topic (no-op if the fs block is absent). */
void iotc_fu_identity_hook(const char *identity_json);

/* True when the identity carried everything file upload needs (i.e. the
 * template has File Support enabled and DRA ran). */
bool iotc_fu_available(void);

/*
 * Upload a file and announce it. Blocking (DNS + 2x TLS handshakes + body
 * transfer); call from a worker thread, not from the MQTT callback thread.
 *
 * rel_path     file name/path under the device's upload prefix, e.g.
 *              "snap-1234.png" (no leading slash, no "device-uploads/").
 * data/len     file body.
 * content_type e.g. "image/png" (NULL -> "application/octet-stream").
 * cf_json      optional Classification: either a JSON object string
 *              (e.g. "{\"state\":\"occupied\",\"score\":91}") or a plain
 *              string; shown on the file card in Telemetry Files. NULL to
 *              omit.
 *
 * Returns 0 on success or a negative errno.
 */
int iotc_fu_upload(const char *rel_path, const uint8_t *data, size_t len,
		   const char *content_type, const char *cf_json);

#ifdef __cplusplus
}
#endif

#endif /* IOTC_FILE_UPLOAD_H */
