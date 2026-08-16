/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Device REST API (DRA) HTTPS client. Implements iotc_dra_run(): the two-step
 * Discovery -> Identity GET sequence over a Zephyr TLS socket using the HTTP
 * client (zephyr/net/http/client.h). The iotc-c-lib DRA module builds the URLs
 * and parses the response bodies; this file owns the socket/TLS/HTTP plumbing
 * only. After a successful run the library's global IotclMqttConfig holds the
 * resolved broker host/client_id/username/topics (read via
 * iotcl_mqtt_get_config()).
 *
 * Compiled only when CONFIG_IOTCONNECT_DRA=y (see CMakeLists.txt).
 *
 * Preconditions (guaranteed by the orchestrator, lib/iotconnect.c):
 *   - iotc-c-lib is initialized in IOTCL_DCT_CUSTOM mode.
 *   - The DRA CA (e.g. GoDaddy Secure Server Certificate G2) has already been
 *     registered under cfg->dra_ca_sec_tag via iotc_tls_credentials_register()
 *     in iotconnect_sdk_init() BEFORE this runs; otherwise the TLS handshake
 *     would fail peer verification. (The app supplies it as auth_info.dra_ca,
 *     or provisions it out-of-band under that sec tag.)
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/http/client.h>

#include "iotc_dra_client.h"
#if defined(CONFIG_IOTCONNECT_FILE_UPLOAD)
#include "iotc_file_upload.h"
#endif

/* iotc-c-lib core + DRA module. */
#include "iotcl.h"
#include "iotcl_cfg.h"
#include "iotcl_dra_url.h"
#include "iotcl_dra_discovery.h"
#include "iotcl_dra_identity.h"

LOG_MODULE_REGISTER(iotc_dra, CONFIG_IOTCONNECT_LOG_LEVEL);

#define DRA_HTTPS_PORT_STR "443"
#define DRA_RECV_BUF_SIZE  CONFIG_IOTCONNECT_DRA_HTTP_RECV_BUF_SIZE

/* identity suffix = "/uid/" + duid; discovery_parse slack avoids a realloc when
 * iotcl_dra_identity_build_url() later appends that suffix to the base URL. */
#define DRA_IDENTITY_SLACK(duid) ((int)(strlen(IOTCL_DRA_IDENTITY_PREFIX) + strlen(duid)))

/*
 * Body accumulator shared with the http_client response callback. The HTTP
 * client delivers the body in one or more fragments; we concatenate them into a
 * caller-supplied, NUL-terminated buffer that the iotc-c-lib parsers consume.
 *
 * iotc_dra_run() is invoked once at bootstrap from a single context, so a
 * file-scope accumulator is acceptable here; it is reset before every request.
 */
struct dra_body {
	uint8_t *buf;     /* destination buffer (owned by caller) */
	size_t   cap;     /* capacity in bytes (room for trailing NUL) */
	size_t   len;     /* bytes written so far */
	bool     overflow;
};

static int dra_response_cb(struct http_response *rsp,
			   enum http_final_call final_data,
			   void *user_data)
{
	struct dra_body *body = (struct dra_body *)user_data;

	ARG_UNUSED(final_data);

	if (body == NULL || rsp->body_frag_start == NULL || rsp->body_frag_len == 0) {
		return 0;
	}

	/* Keep one byte for the NUL terminator. */
	if (body->len + rsp->body_frag_len >= body->cap) {
		body->overflow = true;
		LOG_ERR("DRA HTTP response body exceeds recv buffer (%zu bytes)",
			body->cap);
		return 0;
	}

	memcpy(body->buf + body->len, rsp->body_frag_start, rsp->body_frag_len);
	body->len += rsp->body_frag_len;
	body->buf[body->len] = '\0';

	return 0;
}

/*
 * Perform a single HTTPS GET against (hostname, resource) verifying the peer
 * with the supplied sec tag. On success the response body is in body_buf
 * (NUL-terminated) and *out_len holds its length. Returns 0 on success or a
 * negative -errno.
 */
static int dra_https_get(const char *hostname,
			 const char *resource,
			 int ca_sec_tag,
			 int timeout_ms,
			 uint8_t *body_buf,
			 size_t body_buf_size,
			 size_t *out_len)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;
	sec_tag_t sec_tag_list[] = { (sec_tag_t)ca_sec_tag };
	int peer_verify = CONFIG_IOTCONNECT_TLS_PEER_VERIFY; /* TLS_PEER_VERIFY_REQUIRED == 2 */
	uint8_t recv_buf[DRA_RECV_BUF_SIZE];
	struct http_request req;
	struct dra_body body = {
		.buf = body_buf,
		.cap = body_buf_size,
		.len = 0,
		.overflow = false,
	};
	int sock = -1;
	int ret;

	*out_len = 0;

	ret = zsock_getaddrinfo(hostname, DRA_HTTPS_PORT_STR, &hints, &res);
	if (ret != 0 || res == NULL) {
		LOG_ERR("DRA DNS lookup failed for \"%s\": %d", hostname, ret);
		return -EHOSTUNREACH;
	}

	sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
	if (sock < 0) {
		ret = -errno;
		LOG_ERR("DRA socket() failed: %d", ret);
		goto out_free_addr;
	}

	ret = zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
			 sec_tag_list, sizeof(sec_tag_list));
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("DRA TLS_SEC_TAG_LIST failed: %d", ret);
		goto out_close;
	}

	ret = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
			 hostname, strlen(hostname) + 1);
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("DRA TLS_HOSTNAME failed: %d", ret);
		goto out_close;
	}

	ret = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
			 &peer_verify, sizeof(peer_verify));
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("DRA TLS_PEER_VERIFY failed: %d", ret);
		goto out_close;
	}

	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("DRA connect() to \"%s\" failed: %d", hostname, ret);
		goto out_close;
	}

	memset(&req, 0, sizeof(req));
	req.method = HTTP_GET;
	req.url = resource;
	req.host = hostname;
	/* req.port stays NULL: when set, Zephyr's http_client sends
	 * "Host: <host>:443", but AWS SigV4 presigned URLs sign the Host
	 * header WITHOUT the default port -- S3 answers
	 * SignatureDoesNotMatch (hardware-observed on AI Model downloads).
	 * The connection port comes from getaddrinfo above, and RFC 9110
	 * omits default ports in Host. */
	req.protocol = "HTTP/1.1";
	req.content_type_value = "application/json";
	req.response = dra_response_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = sizeof(recv_buf);

	LOG_INF("DRA GET https://%s%s", hostname, resource);

	ret = http_client_req(sock, &req, timeout_ms, &body);
	if (ret < 0) {
		LOG_ERR("DRA http_client_req failed: %d", ret);
		goto out_close;
	}

	if (body.overflow) {
		ret = -ENOMEM;
		goto out_close;
	}

	if (body.len == 0) {
		LOG_ERR("DRA HTTP response had an empty body");
		ret = -ENODATA;
		goto out_close;
	}

	*out_len = body.len;
	ret = 0;

out_close:
	zsock_close(sock);
out_free_addr:
	zsock_freeaddrinfo(res);
	return ret;
}

int iotc_dra_run(const iotc_dra_config_t *cfg)
{
	/* Single URL context: discovery_parse rewrites it to the identity base
	 * URL, then identity_build_url appends the /uid/<duid> suffix in place. */
	IotclDraUrlContext discovery_url = { 0 };
	uint8_t *body = NULL;
	size_t body_len = 0;
	const char *hostname;
	const char *resource;
	int status;
	int ret;

	if (cfg == NULL || cfg->cpid == NULL || cfg->env == NULL || cfg->duid == NULL) {
		return IOTCL_ERR_MISSING_VALUE;
	}

	/* Same k_heap (CONFIG_HEAP_MEM_POOL_SIZE) that iotc-c-lib + cJSON use via
	 * iotcl_configure_dynamic_memory(k_malloc, ...) -- keep all IOTCONNECT
	 * allocations in one pool so it can be sized as a unit. */
	body = k_malloc(DRA_RECV_BUF_SIZE);
	if (body == NULL) {
		LOG_ERR("DRA: out of memory for response buffer");
		return IOTCL_ERR_OUT_OF_MEMORY;
	}

	/* ------------------------------------------------------------------
	 * Step 1: Discovery
	 *
	 * If a discovery host is configured (CONFIG_IOTCONNECT_DRA_DISCOVERY_HOST),
	 * use it verbatim -- this matches the device's provisioned endpoint (e.g.
	 * the AWS region-specific awsdiscovery.iotconnect.io). Otherwise fall back
	 * to the c-lib platform default (global discovery.iotconnect.io?pf=...).
	 * ------------------------------------------------------------------ */
	if (cfg->discovery_host != NULL && cfg->discovery_host[0] != '\0') {
		status = iotcl_dra_discovery_init_url_with_host(
			&discovery_url, (char *)cfg->discovery_host,
			cfg->cpid, cfg->env);
	} else if (cfg->platform == IOTC_DRA_PF_AZURE) {
		status = iotcl_dra_discovery_init_url_azure(&discovery_url,
							    cfg->cpid, cfg->env);
	} else {
		status = iotcl_dra_discovery_init_url_aws(&discovery_url,
							 cfg->cpid, cfg->env);
	}
	if (status != IOTCL_SUCCESS) {
		LOG_ERR("DRA: failed to build discovery URL: %d", status);
		goto out_free_body;
	}

	hostname = iotcl_dra_url_get_hostname(&discovery_url);
	resource = iotcl_dra_url_get_resource(&discovery_url);
	if (hostname == NULL || resource == NULL) {
		status = IOTCL_ERR_CONFIG_ERROR;
		goto out_deinit_url;
	}

	ret = dra_https_get(hostname, resource, cfg->dra_ca_sec_tag,
			    cfg->timeout_ms, body, DRA_RECV_BUF_SIZE, &body_len);
	if (ret < 0) {
		LOG_ERR("DRA: discovery HTTP GET failed: %d", ret);
		status = IOTCL_ERR_FAILED;
		goto out_deinit_url;
	}

	/* Rewrite discovery_url in place to the identity base URL from d.bu. The
	 * slack reserves room for the upcoming /uid/<duid> append. */
	status = iotcl_dra_discovery_parse(&discovery_url,
					   DRA_IDENTITY_SLACK(cfg->duid),
					   (const char *)body);
	if (status != IOTCL_SUCCESS) {
		LOG_ERR("DRA: discovery response parse failed: %d", status);
		goto out_deinit_url;
	}

	/* ------------------------------------------------------------------
	 * Step 2: Identity
	 * ------------------------------------------------------------------ */
	status = iotcl_dra_identity_build_url(&discovery_url, cfg->duid);
	if (status != IOTCL_SUCCESS) {
		LOG_ERR("DRA: failed to build identity URL: %d", status);
		goto out_deinit_url;
	}

	/* The suffix append may have reallocated the buffer; re-fetch both. */
	hostname = iotcl_dra_url_get_hostname(&discovery_url);
	resource = iotcl_dra_url_get_resource(&discovery_url);
	if (hostname == NULL || resource == NULL) {
		status = IOTCL_ERR_CONFIG_ERROR;
		goto out_deinit_url;
	}

	body_len = 0;
	ret = dra_https_get(hostname, resource, cfg->dra_ca_sec_tag,
			    cfg->timeout_ms, body, DRA_RECV_BUF_SIZE, &body_len);
	if (ret < 0) {
		LOG_ERR("DRA: identity HTTP GET failed: %d", ret);
		status = IOTCL_ERR_FAILED;
		goto out_deinit_url;
	}

	/* Populates the library-owned IotclMqttConfig (host/client_id/username/
	 * topics/cd). Requires IOTCL_DCT_CUSTOM mode (orchestrator guarantee). */
	status = iotcl_dra_identity_configure_library_mqtt((const char *)body);
	if (status != IOTCL_SUCCESS) {
		LOG_ERR("DRA: identity response parse failed: %d", status);
		goto out_deinit_url;
	}

#if defined(CONFIG_IOTCONNECT_FILE_UPLOAD)
	/* Capture the Telemetry Files extras (upload bucket + fu topic) that
	 * iotc-c-lib does not store. */
	iotc_fu_identity_hook((const char *)body);
#endif

	LOG_INF("DRA: discovery/identity complete; MQTT config populated");
	status = IOTCL_SUCCESS;

out_deinit_url:
	iotcl_dra_url_deinit(&discovery_url);
out_free_body:
	/* body came from k_malloc() (the IOTCONNECT k_heap), so it MUST be released
	 * with k_free(). Using libc free() here mismatches the allocator: it writes
	 * libc free-list bookkeeping into a live k_heap chunk, clobbering adjacent
	 * chunk headers. The damage is latent until the next k_free() coalesces
	 * across the poisoned header -- which, once the secure/non-secure RAM
	 * boundary was moved, dereferenced a wild pointer in the secure region and
	 * hard-faulted (TF-M SecureFault) right after the first telemetry publish. */
	k_free(body);
	return status;
}

int iotc_https_download(const char *hostname,
			const char *resource,
			int ca_sec_tag,
			int timeout_ms,
			uint8_t *buf,
			size_t buf_size,
			size_t *out_len)
{
	if (hostname == NULL || resource == NULL || buf == NULL ||
	    out_len == NULL || buf_size == 0) {
		return -EINVAL;
	}
	return dra_https_get(hostname, resource, ca_sec_tag, timeout_ms,
			     buf, buf_size, out_len);
}
