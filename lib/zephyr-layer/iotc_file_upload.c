/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * IOTCONNECT Telemetry Files upload. Flow (hardware-verified against the
 * platform's AWS backend):
 *
 *   1. GET the AWS IoT credentials-provider URL from the identity (fs.url)
 *      with MUTUAL TLS (device cert) + x-amzn-iot-thingname -> temp STS
 *      credentials.
 *   2. When the upload bucket is customer-owned (identity fs.buckets[].ca
 *      with a role ARN in .rarn -- the common case, e.g. when Sagemaker
 *      support is on), those credentials cannot touch the bucket directly:
 *      STS AssumeRole(rarn) is called with them, yielding a SECOND set of
 *      temp credentials.
 *   3. The file is PUT to s3://<bucket>/device-uploads/<client_id>/<path>
 *      with an on-device AWS SigV4 signature (service "s3").
 *   4. A telemetry-shaped {"url": ...,"cf": ...} record on the identity's
 *      fu topic makes the file appear in the platform's Telemetry Files.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include <psa/crypto.h>

#include <cJSON.h>

#include "iotcl.h"
#include "iotcl_util.h"
#include "iotc_mqtt_client.h"
#include "iotc_file_upload.h"

LOG_MODULE_REGISTER(iotc_fu, CONFIG_IOTCONNECT_LOG_LEVEL);

#define FU_HTTPS_PORT_STR "443"
#define FU_RECV_BUF_SIZE 1024
#define FU_RESP_BUF_SIZE 4096
#define FU_TIMEOUT_MS 20000

#define SHA256_HEX_LEN 65 /* 64 hex chars + NUL */
#define SIGNED_HEADERS "host;x-amz-content-sha256;x-amz-date;x-amz-security-token"

/* Captured from the identity response by iotc_fu_identity_hook(). */
static char *fu_topic;
static char *fu_bucket;
static char *fu_role_arn; /* NULL unless the bucket is customer-owned */

/* --------------------------------------------------------------------------
 * Identity capture
 * -------------------------------------------------------------------------- */

static char *dup_or_null(const char *s)
{
	if (s == NULL) {
		return NULL;
	}
	size_t n = strlen(s) + 1;
	char *d = k_malloc(n);

	if (d != NULL) {
		memcpy(d, s, n);
	}
	return d;
}

void iotc_fu_identity_hook(const char *identity_json)
{
	cJSON *root = cJSON_Parse(identity_json);

	if (root == NULL) {
		return;
	}

	k_free(fu_topic);
	k_free(fu_bucket);
	k_free(fu_role_arn);
	fu_topic = NULL;
	fu_bucket = NULL;
	fu_role_arn = NULL;

	cJSON *d = cJSON_GetObjectItem(root, "d");
	cJSON *p = cJSON_GetObjectItem(d, "p");
	cJSON *topics = cJSON_GetObjectItem(p, "topics");
	cJSON *fu = cJSON_GetObjectItem(topics, "fu");

	if (cJSON_IsString(fu)) {
		fu_topic = dup_or_null(cJSON_GetStringValue(fu));
	}

	cJSON *fs = cJSON_GetObjectItem(p, "fs");
	cJSON *buckets = cJSON_GetObjectItem(fs, "buckets");

	/* Prefer a platform-owned bucket (no AssumeRole hop); fall back to
	 * the first customer-owned one and remember its role ARN. */
	if (cJSON_IsArray(buckets)) {
		cJSON *chosen = NULL;
		cJSON *it;

		cJSON_ArrayForEach(it, buckets) {
			if (!cJSON_IsString(cJSON_GetObjectItem(it, "bn"))) {
				continue;
			}
			if (chosen == NULL) {
				chosen = it;
			}
			if (!cJSON_IsTrue(cJSON_GetObjectItem(it, "ca"))) {
				chosen = it;
				break;
			}
		}
		if (chosen != NULL) {
			fu_bucket = dup_or_null(cJSON_GetStringValue(
				cJSON_GetObjectItem(chosen, "bn")));
			if (cJSON_IsTrue(cJSON_GetObjectItem(chosen, "ca"))) {
				fu_role_arn = dup_or_null(cJSON_GetStringValue(
					cJSON_GetObjectItem(chosen, "rarn")));
			}
		}
	}

	if (fu_topic != NULL && fu_bucket != NULL) {
		LOG_INF("file upload ready (bucket %s%s)", fu_bucket,
			fu_role_arn != NULL ? ", via AssumeRole" : "");
	}
	cJSON_Delete(root);
}

bool iotc_fu_available(void)
{
	IotclMqttConfig *mc = iotcl_mqtt_get_config();

	return mc != NULL && mc->aws.fs_creds_url != NULL &&
	       mc->client_id != NULL && fu_topic != NULL && fu_bucket != NULL;
}

/* --------------------------------------------------------------------------
 * Minimal HTTPS request (GET/PUT/POST) with extra headers and mTLS sec tags
 * -------------------------------------------------------------------------- */

struct fu_body {
	uint8_t *buf;
	size_t cap;
	size_t len;
	bool overflow;
	uint16_t http_status;
};

static int fu_response_cb(struct http_response *rsp,
			  enum http_final_call final_data, void *user_data)
{
	struct fu_body *body = (struct fu_body *)user_data;

	ARG_UNUSED(final_data);
	if (body == NULL) {
		return 0;
	}
	body->http_status = rsp->http_status_code;
	if (rsp->body_frag_start == NULL || rsp->body_frag_len == 0) {
		return 0;
	}
	if (body->buf == NULL ||
	    body->len + rsp->body_frag_len >= body->cap) {
		body->overflow = body->buf != NULL;
		return 0;
	}
	memcpy(body->buf + body->len, rsp->body_frag_start, rsp->body_frag_len);
	body->len += rsp->body_frag_len;
	body->buf[body->len] = '\0';
	return 0;
}

static int fu_https_request(enum http_method method, const char *hostname,
			    const char *resource, const char **extra_headers,
			    const char *content_type, const uint8_t *payload,
			    size_t payload_len, const sec_tag_t *tags,
			    size_t n_tags, uint8_t *body_buf,
			    size_t body_buf_size, size_t *out_len,
			    uint16_t *http_status)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;
	int peer_verify = CONFIG_IOTCONNECT_TLS_PEER_VERIFY;
	uint8_t recv_buf[FU_RECV_BUF_SIZE];
	struct http_request req;
	struct fu_body body = {
		.buf = body_buf,
		.cap = body_buf_size,
	};
	int sock = -1;
	int ret;

	if (out_len != NULL) {
		*out_len = 0;
	}

	ret = zsock_getaddrinfo(hostname, FU_HTTPS_PORT_STR, &hints, &res);
	if (ret != 0 || res == NULL) {
		LOG_ERR("DNS lookup failed for \"%s\": %d", hostname, ret);
		return -EHOSTUNREACH;
	}
	sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
	if (sock < 0) {
		ret = -errno;
		goto out_free_addr;
	}
	ret = zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, tags,
			       n_tags * sizeof(sec_tag_t));
	if (ret < 0) {
		ret = -errno;
		goto out_close;
	}
	ret = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, hostname,
			       strlen(hostname) + 1);
	if (ret < 0) {
		ret = -errno;
		goto out_close;
	}
	ret = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &peer_verify,
			       sizeof(peer_verify));
	if (ret < 0) {
		ret = -errno;
		goto out_close;
	}
	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("connect() to \"%s\" failed: %d", hostname, ret);
		goto out_close;
	}

	memset(&req, 0, sizeof(req));
	req.method = method;
	req.url = resource;
	req.host = hostname;
	/* req.port stays NULL: the Host header must not carry :443 (it is a
	 * SigV4-signed header). */
	req.protocol = "HTTP/1.1";
	req.header_fields = extra_headers;
	req.content_type_value = content_type;
	req.payload = (const char *)payload;
	req.payload_len = payload_len;
	req.response = fu_response_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = sizeof(recv_buf);

	ret = http_client_req(sock, &req, FU_TIMEOUT_MS, &body);
	if (ret < 0) {
		LOG_ERR("http_client_req failed: %d", ret);
		goto out_close;
	}
	if (body.overflow) {
		ret = -ENOMEM;
		goto out_close;
	}
	if (out_len != NULL) {
		*out_len = body.len;
	}
	if (http_status != NULL) {
		*http_status = body.http_status;
	}
	ret = 0;

out_close:
	zsock_close(sock);
out_free_addr:
	zsock_freeaddrinfo(res);
	return ret;
}

/* --------------------------------------------------------------------------
 * SigV4
 * -------------------------------------------------------------------------- */

struct aws_creds {
	char *akid;
	char *secret;
	char *token;
};

static void creds_free(struct aws_creds *c)
{
	k_free(c->akid);
	k_free(c->secret);
	k_free(c->token);
	memset(c, 0, sizeof(*c));
}

static void to_hex(const uint8_t *in, size_t in_len, char *out)
{
	static const char hexd[] = "0123456789abcdef";

	for (size_t i = 0; i < in_len; i++) {
		out[i * 2] = hexd[in[i] >> 4];
		out[i * 2 + 1] = hexd[in[i] & 0xF];
	}
	out[in_len * 2] = '\0';
}

static int sha256_hex(const uint8_t *data, size_t len, char *hex_out)
{
	uint8_t hash[32];
	size_t hash_len = 0;

	if (psa_crypto_init() != PSA_SUCCESS ||
	    psa_hash_compute(PSA_ALG_SHA_256, data, len, hash, sizeof(hash),
			     &hash_len) != PSA_SUCCESS) {
		return -EIO;
	}
	to_hex(hash, hash_len, hex_out);
	return 0;
}

static int hmac_sha256(const uint8_t *key, size_t key_len, const char *msg,
		       uint8_t out[32])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key_id = PSA_KEY_ID_NULL;
	size_t mac_len = 0;
	psa_status_t st;

	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));

	st = psa_import_key(&attr, key, key_len, &key_id);
	if (st != PSA_SUCCESS) {
		return -EIO;
	}
	st = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
			     (const uint8_t *)msg, strlen(msg), out, 32,
			     &mac_len);
	psa_destroy_key(key_id);
	return (st == PSA_SUCCESS && mac_len == 32) ? 0 : -EIO;
}

/*
 * Build the four SigV4 request headers for (method host path service region
 * payload_hash) signed with creds. auth_hdr/date_hdr/sha_hdr are caller
 * buffers; *token_hdr_out is k_malloc'd (caller frees). The canonical
 * request signs exactly: host, x-amz-content-sha256, x-amz-date,
 * x-amz-security-token.
 */
static int sigv4_build(const char *method, const char *host, const char *path,
		       const char *service, const char *region,
		       const char *payload_hash, const struct aws_creds *c,
		       char *auth_hdr, size_t auth_size, char *sha_hdr,
		       size_t sha_size, char *date_hdr, size_t date_size,
		       char **token_hdr_out)
{
	char date[9], amz_ts[17];
	char canon_hash[SHA256_HEX_LEN];
	time_t now = time(NULL);
	struct tm tm;
	int ret;

	gmtime_r(&now, &tm);
	snprintf(date, sizeof(date), "%04d%02d%02d", tm.tm_year + 1900,
		 tm.tm_mon + 1, tm.tm_mday);
	snprintf(amz_ts, sizeof(amz_ts), "%sT%02d%02d%02dZ", date, tm.tm_hour,
		 tm.tm_min, tm.tm_sec);

	size_t canon_size = 512 + strlen(c->token);
	char *canon = k_malloc(canon_size);

	if (canon == NULL) {
		return -ENOMEM;
	}
	snprintf(canon, canon_size,
		 "%s\n%s\n\n"
		 "host:%s\n"
		 "x-amz-content-sha256:%s\n"
		 "x-amz-date:%s\n"
		 "x-amz-security-token:%s\n"
		 "\n" SIGNED_HEADERS "\n%s",
		 method, path, host, payload_hash, amz_ts, c->token,
		 payload_hash);
	ret = sha256_hex((const uint8_t *)canon, strlen(canon), canon_hash);
	k_free(canon);
	if (ret != 0) {
		return ret;
	}

	char scope[80], sts_str[256];

	snprintf(scope, sizeof(scope), "%s/%s/%s/aws4_request", date, region,
		 service);
	snprintf(sts_str, sizeof(sts_str), "AWS4-HMAC-SHA256\n%s\n%s\n%s",
		 amz_ts, scope, canon_hash);

	uint8_t k1[32], k2[32], sig[32];
	char kseed[64], sig_hex[SHA256_HEX_LEN];

	snprintf(kseed, sizeof(kseed), "AWS4%s", c->secret);
	if (hmac_sha256((uint8_t *)kseed, strlen(kseed), date, k1) != 0 ||
	    hmac_sha256(k1, 32, region, k2) != 0 ||
	    hmac_sha256(k2, 32, service, k1) != 0 ||
	    hmac_sha256(k1, 32, "aws4_request", k2) != 0 ||
	    hmac_sha256(k2, 32, sts_str, sig) != 0) {
		return -EIO;
	}
	to_hex(sig, 32, sig_hex);

	snprintf(auth_hdr, auth_size,
		 "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s, "
		 "SignedHeaders=" SIGNED_HEADERS ", Signature=%s\r\n",
		 c->akid, scope, sig_hex);
	snprintf(sha_hdr, sha_size, "x-amz-content-sha256: %s\r\n",
		 payload_hash);
	snprintf(date_hdr, date_size, "x-amz-date: %s\r\n", amz_ts);

	size_t th_size = strlen(c->token) + 32;
	char *th = k_malloc(th_size);

	if (th == NULL) {
		return -ENOMEM;
	}
	snprintf(th, th_size, "x-amz-security-token: %s\r\n", c->token);
	*token_hdr_out = th;
	return 0;
}

/* --------------------------------------------------------------------------
 * Small parsing helpers
 * -------------------------------------------------------------------------- */

static bool key_char_ok(char ch)
{
	return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
	       (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
	       ch == '.' || ch == '~' || ch == '/';
}

static bool key_is_clean(const char *s)
{
	for (; *s != '\0'; s++) {
		if (!key_char_ok(*s)) {
			return false;
		}
	}
	return true;
}

/* Extract "<region>" from "<id>.credentials.iot.<region>.amazonaws.com". */
static int region_from_creds_host(const char *host, char *out, size_t out_size)
{
	const char *marker = ".iot.";
	const char *start = strstr(host, marker);

	if (start == NULL) {
		return -EINVAL;
	}
	start += strlen(marker);
	const char *end = strchr(start, '.');

	if (end == NULL || (size_t)(end - start) >= out_size) {
		return -EINVAL;
	}
	memcpy(out, start, end - start);
	out[end - start] = '\0';
	return 0;
}

static int split_url(const char *url, char *host, size_t host_size,
		     const char **path)
{
	const char *p = strstr(url, "://");

	p = (p != NULL) ? p + 3 : url;
	const char *slash = strchr(p, '/');
	size_t hlen = (slash != NULL) ? (size_t)(slash - p) : strlen(p);

	if (hlen == 0 || hlen >= host_size) {
		return -EINVAL;
	}
	memcpy(host, p, hlen);
	host[hlen] = '\0';
	*path = (slash != NULL) ? slash : "/";
	return 0;
}

/* Copy the text between <tag> and </tag> into a fresh k_malloc'd string. */
static char *xml_field_dup(const char *xml, const char *tag)
{
	char open[48];

	snprintf(open, sizeof(open), "<%s>", tag);
	const char *start = strstr(xml, open);

	if (start == NULL) {
		return NULL;
	}
	start += strlen(open);
	snprintf(open, sizeof(open), "</%s>", tag);
	const char *end = strstr(start, open);

	if (end == NULL) {
		return NULL;
	}

	size_t n = (size_t)(end - start);
	char *out = k_malloc(n + 1);

	if (out != NULL) {
		memcpy(out, start, n);
		out[n] = '\0';
	}
	return out;
}

/* --------------------------------------------------------------------------
 * Step 1: temporary credentials from the AWS IoT credentials provider
 * -------------------------------------------------------------------------- */

static int fetch_credentials(const char *creds_url, const char *client_id,
			     struct aws_creds *out)
{
	char host[128];
	const char *path;
	char thing_hdr[160];
	uint8_t *buf;
	size_t body_len = 0;
	uint16_t status = 0;
	int ret;

	ret = split_url(creds_url, host, sizeof(host), &path);
	if (ret != 0) {
		return ret;
	}
	snprintf(thing_hdr, sizeof(thing_hdr), "x-amzn-iot-thingname: %s\r\n",
		 client_id);
	const char *headers[] = { thing_hdr, NULL };
	/* Mutual TLS: broker CA validates the Amazon chain; the device tag
	 * carries the client certificate + key. */
	sec_tag_t tags[] = {
		(sec_tag_t)CONFIG_IOTCONNECT_SEC_TAG_BROKER_CA,
		(sec_tag_t)CONFIG_IOTCONNECT_SEC_TAG_DEVICE,
	};

	buf = k_malloc(FU_RESP_BUF_SIZE);
	if (buf == NULL) {
		return -ENOMEM;
	}
	ret = fu_https_request(HTTP_GET, host, path, headers, NULL, NULL, 0,
			       tags, ARRAY_SIZE(tags), buf, FU_RESP_BUF_SIZE,
			       &body_len, &status);
	if (ret != 0 || status != 200) {
		LOG_ERR("credentials GET failed (ret %d, HTTP %u)", ret, status);
		k_free(buf);
		return (ret != 0) ? ret : -EACCES;
	}

	cJSON *root = cJSON_Parse((const char *)buf);
	cJSON *creds = cJSON_GetObjectItem(root, "credentials");

	out->akid = dup_or_null(cJSON_GetStringValue(
		cJSON_GetObjectItem(creds, "accessKeyId")));
	out->secret = dup_or_null(cJSON_GetStringValue(
		cJSON_GetObjectItem(creds, "secretAccessKey")));
	out->token = dup_or_null(cJSON_GetStringValue(
		cJSON_GetObjectItem(creds, "sessionToken")));
	cJSON_Delete(root);
	k_free(buf);

	if (out->akid == NULL || out->secret == NULL || out->token == NULL) {
		LOG_ERR("credentials response missing fields");
		creds_free(out);
		return -EBADMSG;
	}
	return 0;
}

/* --------------------------------------------------------------------------
 * Step 2: STS AssumeRole (customer-owned buckets)
 * -------------------------------------------------------------------------- */

/* %-encode ':' and '/' (the only reserved chars in a role ARN). */
static void urlencode_arn(const char *arn, char *out, size_t out_size)
{
	size_t o = 0;

	for (; *arn != '\0' && o + 4 < out_size; arn++) {
		if (*arn == ':') {
			memcpy(&out[o], "%3A", 3);
			o += 3;
		} else if (*arn == '/') {
			memcpy(&out[o], "%2F", 3);
			o += 3;
		} else {
			out[o++] = *arn;
		}
	}
	out[o] = '\0';
}

static int sts_assume_role(const struct aws_creds *in, const char *region,
			   const char *role_arn, const char *session_name,
			   struct aws_creds *out)
{
	char sts_host[48];
	char arn_enc[256];
	char body[512];
	char payload_hash[SHA256_HEX_LEN];
	char auth_hdr[256], sha_hdr[96], date_hdr[48];
	char *token_hdr = NULL;
	uint8_t *resp;
	size_t resp_len = 0;
	uint16_t status = 0;
	int ret;

	snprintf(sts_host, sizeof(sts_host), "sts.%s.amazonaws.com", region);
	urlencode_arn(role_arn, arn_enc, sizeof(arn_enc));
	snprintf(body, sizeof(body),
		 "Action=AssumeRole&Version=2011-06-15&RoleArn=%s"
		 "&RoleSessionName=%s",
		 arn_enc, session_name);

	ret = sha256_hex((const uint8_t *)body, strlen(body), payload_hash);
	if (ret != 0) {
		return ret;
	}
	ret = sigv4_build("POST", sts_host, "/", "sts", region, payload_hash,
			  in, auth_hdr, sizeof(auth_hdr), sha_hdr,
			  sizeof(sha_hdr), date_hdr, sizeof(date_hdr),
			  &token_hdr);
	if (ret != 0) {
		return ret;
	}

	const char *headers[] = { auth_hdr, sha_hdr, date_hdr, token_hdr,
				  NULL };
	sec_tag_t tags[] = { (sec_tag_t)CONFIG_IOTCONNECT_SEC_TAG_BROKER_CA };

	resp = k_malloc(FU_RESP_BUF_SIZE);
	if (resp == NULL) {
		k_free(token_hdr);
		return -ENOMEM;
	}
	ret = fu_https_request(HTTP_POST, sts_host, "/", headers,
			       "application/x-www-form-urlencoded",
			       (const uint8_t *)body, strlen(body), tags, 1,
			       resp, FU_RESP_BUF_SIZE, &resp_len, &status);
	k_free(token_hdr);
	if (ret != 0 || status != 200) {
		LOG_ERR("STS AssumeRole failed (ret %d, HTTP %u)", ret, status);
		if (resp_len > 0) {
			LOG_ERR("STS says: %.200s", (char *)resp);
		}
		k_free(resp);
		return (ret != 0) ? ret : -EACCES;
	}

	out->akid = xml_field_dup((const char *)resp, "AccessKeyId");
	out->secret = xml_field_dup((const char *)resp, "SecretAccessKey");
	out->token = xml_field_dup((const char *)resp, "SessionToken");
	k_free(resp);

	if (out->akid == NULL || out->secret == NULL || out->token == NULL) {
		LOG_ERR("AssumeRole response missing fields");
		creds_free(out);
		return -EBADMSG;
	}
	LOG_INF("AssumeRole OK");
	return 0;
}

/* --------------------------------------------------------------------------
 * Steps 3+4: S3 PUT + fu announce
 * -------------------------------------------------------------------------- */

int iotc_fu_upload(const char *rel_path, const uint8_t *data, size_t len,
		   const char *content_type, const char *cf_json)
{
	IotclMqttConfig *mc = iotcl_mqtt_get_config();
	struct aws_creds provider_creds = { 0 };
	struct aws_creds role_creds = { 0 };
	const struct aws_creds *s3_creds;
	char creds_host[128], region[24];
	const char *creds_path;
	char payload_hash[SHA256_HEX_LEN];
	char s3_host[192], object_key[192];
	char auth_hdr[256], sha_hdr[96], date_hdr[48];
	char *token_hdr = NULL;
	int ret;

	if (!iotc_fu_available()) {
		LOG_ERR("file upload not available (enable File Support on "
			"the template and re-run discovery)");
		return -ENOTSUP;
	}
	if (rel_path == NULL || data == NULL || len == 0 ||
	    !key_is_clean(rel_path) || rel_path[0] == '/') {
		return -EINVAL;
	}
	if (content_type == NULL) {
		content_type = "application/octet-stream";
	}

	ret = split_url(mc->aws.fs_creds_url, creds_host, sizeof(creds_host),
			&creds_path);
	if (ret == 0) {
		ret = region_from_creds_host(creds_host, region,
					     sizeof(region));
	}
	if (ret != 0) {
		LOG_WRN("cannot parse region; assuming us-east-1");
		strcpy(region, "us-east-1");
	}

	ret = fetch_credentials(mc->aws.fs_creds_url, mc->client_id,
				&provider_creds);
	if (ret != 0) {
		return ret;
	}
	s3_creds = &provider_creds;

	if (fu_role_arn != NULL) {
		ret = sts_assume_role(&provider_creds, region, fu_role_arn,
				      mc->client_id, &role_creds);
		if (ret != 0) {
			goto out;
		}
		s3_creds = &role_creds;
	}

	snprintf(s3_host, sizeof(s3_host), "%s.s3.%s.amazonaws.com",
		 fu_bucket, region);
	ret = snprintf(object_key, sizeof(object_key),
		       "/device-uploads/%s/%s", mc->client_id, rel_path);
	if (ret < 0 || (size_t)ret >= sizeof(object_key) ||
	    !key_is_clean(object_key)) {
		ret = -EINVAL;
		goto out;
	}

	ret = sha256_hex(data, len, payload_hash);
	if (ret != 0) {
		goto out;
	}
	ret = sigv4_build("PUT", s3_host, object_key, "s3", region,
			  payload_hash, s3_creds, auth_hdr, sizeof(auth_hdr),
			  sha_hdr, sizeof(sha_hdr), date_hdr, sizeof(date_hdr),
			  &token_hdr);
	if (ret != 0) {
		goto out;
	}

	const char *headers[] = { auth_hdr, sha_hdr, date_hdr, token_hdr,
				  NULL };
	sec_tag_t tags[] = { (sec_tag_t)CONFIG_IOTCONNECT_SEC_TAG_BROKER_CA };
	char *err_body = k_malloc(1024);
	size_t err_len = 0;
	uint16_t http_status = 0;

	LOG_INF("PUT https://%s%s (%u B)", s3_host, object_key,
		(unsigned int)len);
	ret = fu_https_request(HTTP_PUT, s3_host, object_key, headers,
			       content_type, data, len, tags, 1,
			       (uint8_t *)err_body,
			       (err_body != NULL) ? 1024 : 0, &err_len,
			       &http_status);
	k_free(token_hdr);
	if (ret != 0 || http_status != 200) {
		LOG_ERR("S3 PUT failed (ret %d, HTTP %u)", ret, http_status);
		if (err_body != NULL && err_len > 0) {
			LOG_ERR("S3 says: %.240s", err_body);
		}
		k_free(err_body);
		if (ret == 0) {
			ret = -EACCES;
		}
		goto out;
	}
	k_free(err_body);

	/* ---- announce on the fu topic ------------------------------------- */
	{
		cJSON *root = cJSON_CreateObject();
		cJSON *arr = cJSON_AddArrayToObject(root, "d");
		cJSON *item = cJSON_CreateObject();
		cJSON *drec = cJSON_CreateObject();
		char ts_iso[32];

		if (iotcl_iso_timestamp_now(ts_iso, sizeof(ts_iso)) == 0) {
			cJSON_AddStringToObject(item, "dt", ts_iso);
		}
		if (cf_json != NULL) {
			cJSON *cf = cJSON_Parse(cf_json);

			if (cf != NULL) {
				cJSON_AddItemToObject(drec, "cf", cf);
			} else {
				cJSON_AddStringToObject(drec, "cf", cf_json);
			}
		}
		cJSON_AddStringToObject(drec, "url", rel_path);
		cJSON_AddItemToObject(item, "d", drec);
		cJSON_AddItemToArray(arr, item);

		char *msg = cJSON_PrintUnformatted(root);

		cJSON_Delete(root);
		if (msg == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		iotc_mqtt_client_publish(fu_topic, msg);
		cJSON_free(msg);
		LOG_INF("file \"%s\" uploaded + announced", rel_path);
		ret = 0;
	}

out:
	creds_free(&provider_creds);
	creds_free(&role_creds);
	return ret;
}
