/*
 * Copyright (c) 2026 Avnet, Inc.  SPDX-License-Identifier: MIT
 *
 * IOTCONNECT OTA -> MCUboot (CONFIG_IOTCONNECT_OTA_MCUBOOT).
 *
 * Flow (matches the iotcl_c2d.h "best practices" contract):
 *   1. The platform pushes an OTA message; the SDK routes it to
 *      iotc_ota_handle() when the application sets
 *      config.ota_cb = iotc_ota_handle.
 *   2. The image is downloaded over HTTPS (TLS against the same CA set the
 *      broker/DRA use) and streamed into the MCUboot secondary slot.
 *   3. The pending OTA ack id is persisted to settings, the image is marked
 *      for a TEST swap, and the device reboots.
 *   4. On the next boot, once the NEW firmware is connected, the application
 *      calls iotc_ota_confirm_if_pending(): the image is confirmed
 *      (permanent) and the persisted ack is completed with DOWNLOAD_DONE.
 *      If the new image fails to boot, MCUboot reverts and the same call
 *      reports DOWNLOAD_FAILED from the old firmware.
 *
 * The downloaded file must be an MCUboot-signed image (imgtool / sysbuild
 * zephyr.signed.bin) built for this board's slot layout.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/tls_credentials.h>

#include "iotcl.h"
#include "iotcl_c2d.h"

LOG_MODULE_REGISTER(iotc_ota, CONFIG_IOTCONNECT_LOG_LEVEL);

#define OTA_HTTPS_PORT_STR "443"
#define OTA_RECV_BUF_SIZE  CONFIG_IOTCONNECT_OTA_HTTP_RECV_BUF_SIZE
#define OTA_HTTP_TIMEOUT   K_MSEC(CONFIG_IOTCONNECT_OTA_HTTP_TIMEOUT_MS)

/* Persisted across the update reboot. */
static char pending_ack[128];
static bool have_pending_ack;

static struct flash_img_context flash_ctx;
static bool download_failed;
static size_t downloaded;

/* ---- settings persistence for the ack id -------------------------------- */

static int ota_settings_set(const char *name, size_t len,
			    settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "ack") == 0 && len < sizeof(pending_ack)) {
		if (read_cb(cb_arg, pending_ack, len) >= 0) {
			pending_ack[len] = '\0';
			have_pending_ack = pending_ack[0] != '\0';
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(iotc_ota, "iotc_ota", NULL,
			       ota_settings_set, NULL, NULL);

static void ota_store_ack(const char *ack)
{
	(void)settings_save_one("iotc_ota/ack", ack ? ack : "",
				ack ? strlen(ack) : 0);
}

/* ---- HTTPS download into the secondary slot ------------------------------ */

static int ota_body_cb(struct http_response *rsp,
		       enum http_final_call final, void *user_data)
{
	ARG_UNUSED(user_data);

	if (rsp->http_status_code != 0 && rsp->http_status_code != 200) {
		LOG_ERR("OTA download HTTP status %d", rsp->http_status_code);
		download_failed = true;
		return 0;
	}
	if (rsp->body_frag_len > 0 && !download_failed) {
		bool flush = (final == HTTP_DATA_FINAL);

		if (flash_img_buffered_write(&flash_ctx, rsp->body_frag_start,
					     rsp->body_frag_len, flush) != 0) {
			LOG_ERR("Flash write failed at %u bytes", (unsigned)downloaded);
			download_failed = true;
			return 0;
		}
		downloaded += rsp->body_frag_len;
	}
	return 0;
}

static int ota_download(const char *url, const char *hostname)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;
	sec_tag_t sec_tags[] = {
		(sec_tag_t)CONFIG_IOTCONNECT_SEC_TAG_BROKER_CA,
		(sec_tag_t)CONFIG_IOTCONNECT_SEC_TAG_DRA_CA,
	};
	static uint8_t recv_buf[OTA_RECV_BUF_SIZE];
	struct http_request req;
	int sock = -1;
	int ret;

	/* The URL is "https://<host>/<path...>"; http_client wants the path. */
	const char *path = strstr(url, hostname);

	if (path == NULL) {
		return -EINVAL;
	}
	path += strlen(hostname);
	if (*path == '\0') {
		path = "/";
	}

	ret = zsock_getaddrinfo(hostname, OTA_HTTPS_PORT_STR, &hints, &res);
	if (ret != 0) {
		LOG_ERR("OTA host resolve failed (%d)", ret);
		return -EIO;
	}
	sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
	if (sock < 0) {
		ret = -errno;
		goto out;
	}
	if (zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
			     sec_tags, sizeof(sec_tags)) < 0 ||
	    zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
			     hostname, strlen(hostname) + 1) < 0) {
		ret = -errno;
		goto out;
	}
	ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	if (ret < 0) {
		ret = -errno;
		LOG_ERR("OTA TLS connect failed (%d)", ret);
		goto out;
	}

	download_failed = false;
	downloaded = 0;
	ret = flash_img_init(&flash_ctx);
	if (ret != 0) {
		LOG_ERR("flash_img_init failed (%d)", ret);
		goto out;
	}

	memset(&req, 0, sizeof(req));
	req.method = HTTP_GET;
	req.url = path;
	req.host = hostname;
	req.protocol = "HTTP/1.1";
	req.response = ota_body_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = sizeof(recv_buf);

	ret = http_client_req(sock, &req, CONFIG_IOTCONNECT_OTA_HTTP_TIMEOUT_MS, NULL);
	if (ret < 0 || download_failed || downloaded == 0) {
		LOG_ERR("OTA download failed (ret=%d, %u bytes)", ret, (unsigned)downloaded);
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	LOG_INF("OTA image downloaded: %u bytes", (unsigned)downloaded);
	ret = 0;
out:
	if (sock >= 0) {
		zsock_close(sock);
	}
	if (res != NULL) {
		zsock_freeaddrinfo(res);
	}
	return ret;
}

/* ---- public API ---------------------------------------------------------- */

void iotc_ota_handle(IotclC2dEventData data)
{
	const char *url = iotcl_c2d_get_ota_url(data, 0);
	const char *host = iotcl_c2d_get_ota_url_hostname(data, 0);
	const char *ack = iotcl_c2d_get_ack_id(data);
	int ret;

	if (url == NULL || host == NULL) {
		LOG_ERR("OTA event carries no URL");
		return;
	}
	LOG_INF("OTA requested: %s", url);
	if (ack != NULL) {
		(void)iotcl_mqtt_send_ota_ack(ack, IOTCL_C2D_EVT_OTA_DOWNLOADING,
					      "downloading");
	}

	ret = ota_download(url, host);
	if (ret != 0) {
		if (ack != NULL) {
			(void)iotcl_mqtt_send_ota_ack(
				ack, IOTCL_C2D_EVT_OTA_DOWNLOAD_FAILED,
				"download failed");
		}
		return;
	}

	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret != 0) {
		LOG_ERR("boot_request_upgrade failed (%d)", ret);
		if (ack != NULL) {
			(void)iotcl_mqtt_send_ota_ack(
				ack, IOTCL_C2D_EVT_OTA_DOWNLOAD_FAILED,
				"image mark failed");
		}
		return;
	}

	/* Persist the ack so the NEW firmware can complete it after boot. */
	ota_store_ack(ack);
	LOG_WRN("OTA image staged; rebooting into MCUboot test swap");
	k_sleep(K_SECONDS(2)); /* let the ack publish drain */
	sys_reboot(SYS_REBOOT_COLD);
}

void iotc_ota_confirm_if_pending(void)
{
	if (!have_pending_ack) {
		/* Still confirm a self-tested image (e.g. after a manual swap). */
		if (!boot_is_img_confirmed()) {
			(void)boot_write_img_confirmed();
		}
		return;
	}

	if (boot_is_img_confirmed()) {
		/* MCUboot reverted: we are back on the OLD image. */
		LOG_ERR("OTA image was reverted by MCUboot");
		(void)iotcl_mqtt_send_ota_ack(pending_ack,
					      IOTCL_C2D_EVT_OTA_DOWNLOAD_FAILED,
					      "new image reverted");
	} else {
		int ret = boot_write_img_confirmed();

		if (ret == 0) {
			LOG_INF("OTA image confirmed; reporting success");
			(void)iotcl_mqtt_send_ota_ack(pending_ack,
						      IOTCL_C2D_EVT_OTA_DOWNLOAD_DONE,
						      NULL);
		} else {
			LOG_ERR("Image confirm failed (%d)", ret);
			(void)iotcl_mqtt_send_ota_ack(pending_ack,
						      IOTCL_C2D_EVT_OTA_DOWNLOAD_FAILED,
						      "confirm failed");
		}
	}
	pending_ack[0] = '\0';
	have_pending_ack = false;
	ota_store_ack(NULL);
}
