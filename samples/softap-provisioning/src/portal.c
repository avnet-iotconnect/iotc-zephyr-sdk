/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Soft-AP provisioning portal (see portal.h).
 *
 * Flow, all driven from a phone browser at http://192.168.4.1 --
 *   1. POST /api/wifi      {"ssid":"...","psk":"..."}  -> wifi_credentials
 *   2. POST /api/provision {"duid":"..."}              -> on-device keygen,
 *      responds with the device certificate PEM to register in /IOTCONNECT
 *   3. POST /api/config    <iotcDeviceConfig.json>     -> cpid/env/duid/disc
 *   4. POST /api/finish                                -> reboot into STA
 * GET /api/status reports what is stored so the page can resume mid-flow.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_credentials.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/reboot.h>

#include "cJSON.h"
#include "iotconnect_identity.h"
#include "iotconnect_provision.h"

#include "portal.h"

LOG_MODULE_REGISTER(portal, LOG_LEVEL_INF);

#define PORTAL_AP_IP      "192.168.4.1"
#define PORTAL_AP_NETMASK "255.255.255.0"

/* Provisioning results, shared between handlers. */
static char prov_key[512];
static char prov_crt[1024];
static char prov_duid[64];
static bool wifi_stored;
static bool config_stored;

/* ---- the page ------------------------------------------------------------ */

static const char index_html[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>IOTCONNECT Device Setup</title><style>"
"body{font-family:system-ui,sans-serif;margin:0;background:#f4f6f8;color:#1a2733}"
"main{max-width:520px;margin:0 auto;padding:16px}"
"h1{font-size:1.3em}h2{font-size:1.05em;margin-top:1.6em}"
"section{background:#fff;border-radius:8px;padding:14px 16px;margin:12px 0;"
"box-shadow:0 1px 3px rgba(0,0,0,.12)}"
"input,textarea{width:100%;box-sizing:border-box;padding:8px;margin:6px 0;"
"border:1px solid #b9c4cc;border-radius:5px;font-size:1em}"
"textarea{font-family:monospace;font-size:.8em}"
"button{background:#0067b8;color:#fff;border:0;border-radius:5px;"
"padding:10px 18px;font-size:1em;margin-top:6px}"
"button:disabled{background:#9bb3c4}"
"pre{background:#eef2f5;padding:8px;overflow-x:auto;font-size:.72em;"
"white-space:pre-wrap;word-break:break-all}"
".ok{color:#107c10}.err{color:#c42b1c}.step{color:#5a6b78;font-size:.9em}"
"</style></head><body><main>"
"<h1>/IOTCONNECT Device Setup</h1>"
"<div id=st class=step>Loading device status&hellip;</div>"

"<section><h2>1&#41; Home Wi-Fi</h2>"
"<input id=ssid placeholder='Wi-Fi network name (SSID)'>"
"<input id=psk type=password placeholder='Passphrase (blank if open)'>"
"<button onclick=saveWifi()>Save Wi-Fi</button> <span id=wifiMsg></span></section>"

"<section><h2>2&#41; Device identity</h2>"
"<p class=step>Pick a Unique ID (up to 10 letters/digits, starts with a"
" letter). The key pair is generated on the device and never leaves it.</p>"
"<input id=duid placeholder='Unique ID (DUID)' maxlength=10>"
"<button onclick=prov()>Generate identity</button> <span id=provMsg></span>"
"<div id=certBox style=display:none><p class=step>Create the device in"
" /IOTCONNECT (Devices &rarr; Create Device, Auth: Self-Signed, Unique ID"
" as above) and paste this certificate:</p><pre id=cert></pre></div></section>"

"<section><h2>3&#41; Cloud account</h2>"
"<p class=step>Download <b>iotcDeviceConfig.json</b> from the device's Info"
" panel in /IOTCONNECT and paste its contents:</p>"
"<textarea id=cfg rows=7 placeholder='{ \"cpid\": ... }'></textarea>"
"<button onclick=saveCfg()>Save cloud config</button> <span id=cfgMsg></span></section>"

"<section><h2>4&#41; Connect</h2>"
"<p class=step>Reboots the device; it leaves this setup network and joins"
" your Wi-Fi as the device you created.</p>"
"<button id=fin onclick=finish()>Finish &amp; connect</button>"
" <span id=finMsg></span></section>"

"<script>"
"function J(u,o){return fetch(u,o).then(r=>{if(!r.ok)throw r.status;return r.text()})}"
"function refresh(){J('/api/status').then(t=>{let s=JSON.parse(t);"
"document.getElementById('st').textContent='Device '+(s.duid||'(no identity yet)')+"
"' | Wi-Fi: '+(s.wifi?'saved':'not set')+' | Cloud config: '+(s.config?'saved':'not set');"
"if(s.cert){document.getElementById('certBox').style.display='block';"
"document.getElementById('cert').textContent=s.cert;}}).catch(()=>{});}"
"function saveWifi(){let b=JSON.stringify({ssid:ssid.value,psk:psk.value});"
"J('/api/wifi',{method:'POST',body:b}).then(()=>{wifiMsg.textContent='saved';"
"wifiMsg.className='ok';refresh();}).catch(e=>{wifiMsg.textContent='failed ('+e+')';"
"wifiMsg.className='err';});}"
"function prov(){provMsg.textContent='generating (takes a few seconds)...';"
"provMsg.className='step';"
"J('/api/provision',{method:'POST',body:JSON.stringify({duid:duid.value})})"
".then(t=>{provMsg.textContent='done';provMsg.className='ok';"
"document.getElementById('certBox').style.display='block';"
"document.getElementById('cert').textContent=t;refresh();})"
".catch(e=>{provMsg.textContent='failed ('+e+')';provMsg.className='err';});}"
"function saveCfg(){J('/api/config',{method:'POST',body:cfg.value})"
".then(()=>{cfgMsg.textContent='saved';cfgMsg.className='ok';refresh();})"
".catch(e=>{cfgMsg.textContent='failed ('+e+')';cfgMsg.className='err';});}"
"function finish(){fin.disabled=true;finMsg.textContent="
"'Rebooting -- reconnect your phone to your normal Wi-Fi.';"
"J('/api/finish',{method:'POST'}).catch(()=>{});}"
"refresh();"
"</script></main></body></html>";

/* ---- request-body accumulation ------------------------------------------- */

/* The portal serves one provisioning client; a single body buffer per
 * resource is sufficient and keeps everything static. */
struct body_buf {
	char data[768];
	size_t len;
};

static int body_accumulate(struct body_buf *b, enum http_transaction_status status,
			   const struct http_request_ctx *req)
{
	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		b->len = 0;
		return -EAGAIN; /* nothing to respond to */
	}
	if (req->data != NULL && req->data_len > 0) {
		size_t room = sizeof(b->data) - 1 - b->len;
		size_t n = MIN(req->data_len, room);

		memcpy(b->data + b->len, req->data, n);
		b->len += n;
		b->data[b->len] = '\0';
	}
	return (status == HTTP_SERVER_REQUEST_DATA_FINAL) ? 0 : -EINPROGRESS;
}

static void respond(struct http_response_ctx *rsp, uint16_t code, const char *body)
{
	rsp->status = code;
	rsp->body = (const uint8_t *)body;
	rsp->body_len = strlen(body);
	rsp->final_chunk = true;
}

/* ---- /api/status ---------------------------------------------------------- */

static char status_json[1400];

static int status_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *req,
			  struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client); ARG_UNUSED(req); ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	/* Compact hand-built JSON; the certificate is the only tricky field
	 * and PEM is JSON-safe once newlines are escaped. */
	char cert_esc[1100];
	size_t o = 0;

	for (const char *p = prov_crt; *p != '\0' && o < sizeof(cert_esc) - 3; p++) {
		if (*p == '\n') {
			cert_esc[o++] = '\\';
			cert_esc[o++] = 'n';
		} else if (*p != '\r') {
			cert_esc[o++] = *p;
		}
	}
	cert_esc[o] = '\0';
	snprintk(status_json, sizeof(status_json),
		 "{\"duid\":\"%s\",\"wifi\":%s,\"config\":%s,\"cert\":\"%s\"}",
		 prov_duid, wifi_stored ? "true" : "false",
		 config_stored ? "true" : "false", cert_esc);
	respond(rsp, 200, status_json);
	return 0;
}

/* ---- /api/wifi ------------------------------------------------------------ */

static struct body_buf wifi_body;

static int wifi_handler(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *req,
			struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client); ARG_UNUSED(user_data);
	int ret = body_accumulate(&wifi_body, status, req);

	if (ret != 0) {
		return 0;
	}

	struct wifi_credentials_personal creds = { 0 };
	cJSON *root = cJSON_Parse(wifi_body.data);
	const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
	const cJSON *psk = cJSON_GetObjectItemCaseSensitive(root, "psk");

	wifi_body.len = 0;
	if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' ||
	    strlen(ssid->valuestring) >= sizeof(creds.header.ssid)) {
		cJSON_Delete(root);
		respond(rsp, 400, "bad ssid");
		return 0;
	}
	creds.header.ssid_len = strlen(ssid->valuestring);
	memcpy(creds.header.ssid, ssid->valuestring, creds.header.ssid_len);
	if (cJSON_IsString(psk) && psk->valuestring[0] != '\0') {
		creds.password_len = strlen(psk->valuestring);
		if (creds.password_len >= sizeof(creds.password)) {
			cJSON_Delete(root);
			respond(rsp, 400, "psk too long");
			return 0;
		}
		memcpy(creds.password, psk->valuestring, creds.password_len);
		creds.header.type = WIFI_SECURITY_TYPE_PSK;
	} else {
		creds.header.type = WIFI_SECURITY_TYPE_NONE;
	}
	cJSON_Delete(root);

	ret = wifi_credentials_set_personal_struct(&creds);
	if (ret != 0) {
		LOG_ERR("wifi_credentials store failed (%d)", ret);
		respond(rsp, 500, "store failed");
		return 0;
	}
	wifi_stored = true;
	LOG_INF("Portal: Wi-Fi credentials stored for \"%s\"", creds.header.ssid);
	respond(rsp, 200, "ok");
	return 0;
}

/* ---- /api/provision -------------------------------------------------------- */

static struct body_buf prov_body;

static int provision_handler(struct http_client_ctx *client, enum http_transaction_status status,
			     const struct http_request_ctx *req,
			     struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client); ARG_UNUSED(user_data);
	int ret = body_accumulate(&prov_body, status, req);

	if (ret != 0) {
		return 0;
	}

	cJSON *root = cJSON_Parse(prov_body.data);
	const cJSON *duid = cJSON_GetObjectItemCaseSensitive(root, "duid");

	prov_body.len = 0;
	if (!cJSON_IsString(duid) || duid->valuestring[0] == '\0' ||
	    strlen(duid->valuestring) >= sizeof(prov_duid)) {
		cJSON_Delete(root);
		respond(rsp, 400, "bad duid");
		return 0;
	}
	strcpy(prov_duid, duid->valuestring);
	cJSON_Delete(root);

	ret = iotc_provision_selfsigned(prov_duid, prov_key, sizeof(prov_key),
					prov_crt, sizeof(prov_crt));
	if (ret != 0) {
		LOG_ERR("on-device keygen failed (-0x%04x)", (unsigned int)-ret);
		respond(rsp, 500, "keygen failed");
		return 0;
	}
	/* Store like `iotcprov provision`: duid + cert + key now; cpid/env/disc
	 * arrive with the config JSON. Lengths include the NUL for PEM parsing. */
	(void)iotc_kv_save("duid", prov_duid, strlen(prov_duid));
	(void)iotc_kv_save("cert", prov_crt, strlen(prov_crt) + 1);
	(void)iotc_kv_save("key", prov_key, strlen(prov_key) + 1);
	LOG_INF("Portal: on-device identity generated for '%s'", prov_duid);
	respond(rsp, 200, prov_crt);
	return 0;
}

/* ---- /api/config ------------------------------------------------------------ */

static struct body_buf cfg_body;

static int config_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *req,
			  struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client); ARG_UNUSED(user_data);
	int ret = body_accumulate(&cfg_body, status, req);

	if (ret != 0) {
		return 0;
	}
	ret = iotc_identity_apply_config_json(cfg_body.data);
	cfg_body.len = 0;
	if (ret != 0) {
		respond(rsp, 400, "config not accepted");
		return 0;
	}
	config_stored = true;
	LOG_INF("Portal: cloud config stored");
	respond(rsp, 200, "ok");
	return 0;
}

/* ---- /api/finish ------------------------------------------------------------ */

static void reboot_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_WRN("Portal complete; rebooting into station mode");
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_fn);

static int finish_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *req,
			  struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client); ARG_UNUSED(req); ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	/* Give the response (and the AP) a moment to flush before rebooting. */
	k_work_schedule(&reboot_work, K_SECONDS(2));
	respond(rsp, 200, "rebooting");
	return 0;
}

/* ---- HTTP service wiring ------------------------------------------------- */

static uint16_t portal_port = 80;
HTTP_SERVICE_DEFINE(portal_svc, NULL, &portal_port, 1, 4, NULL, NULL, NULL);

static struct http_resource_detail_static index_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "text/html",
	},
	.static_data = index_html,
	.static_data_len = sizeof(index_html) - 1,
};
HTTP_RESOURCE_DEFINE(index_res, portal_svc, "/", &index_detail);

#define PORTAL_DYNAMIC(_name, _handler, _methods)                              \
	static struct http_resource_detail_dynamic _name##_detail = {          \
		.common = {                                                    \
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,                    \
			.bitmask_of_supported_http_methods = (_methods),       \
		},                                                             \
		.cb = _handler,                                                \
		.user_data = NULL,                                             \
	}

PORTAL_DYNAMIC(status_res, status_handler, BIT(HTTP_GET));
PORTAL_DYNAMIC(wifi_res, wifi_handler, BIT(HTTP_POST));
PORTAL_DYNAMIC(prov_res, provision_handler, BIT(HTTP_POST));
PORTAL_DYNAMIC(cfg_res, config_handler, BIT(HTTP_POST));
PORTAL_DYNAMIC(finish_res, finish_handler, BIT(HTTP_POST));

HTTP_RESOURCE_DEFINE(status_r, portal_svc, "/api/status", &status_res_detail);
HTTP_RESOURCE_DEFINE(wifi_r, portal_svc, "/api/wifi", &wifi_res_detail);
HTTP_RESOURCE_DEFINE(prov_r, portal_svc, "/api/provision", &prov_res_detail);
HTTP_RESOURCE_DEFINE(cfg_r, portal_svc, "/api/config", &cfg_res_detail);
HTTP_RESOURCE_DEFINE(finish_r, portal_svc, "/api/finish", &finish_res_detail);

/* ---- AP + DHCP + serve ----------------------------------------------------- */

static int ap_up(struct net_if *ap_iface)
{
	static struct wifi_connect_req_params ap_cfg;
	static char ap_ssid[32];
	struct net_in_addr addr, netmask;
	struct net_linkaddr *mac = net_if_get_link_addr(ap_iface);
	int ret;

	snprintk(ap_ssid, sizeof(ap_ssid), "IOTC-RW612-%02X%02X",
		 mac->addr[4], mac->addr[5]);

	if (net_addr_pton(NET_AF_INET, PORTAL_AP_IP, &addr) ||
	    net_addr_pton(NET_AF_INET, PORTAL_AP_NETMASK, &netmask)) {
		return -EINVAL;
	}
	net_if_ipv4_set_gw(ap_iface, &addr);
	if (net_if_ipv4_addr_add(ap_iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("AP address add failed");
		return -EIO;
	}
	(void)net_if_ipv4_set_netmask_by_addr(ap_iface, &addr, &netmask);

	addr.s4_addr[3] += 10; /* DHCP pool starts at .11 */
	ret = net_dhcpv4_server_start(ap_iface, &addr);
	if (ret != 0) {
		LOG_ERR("DHCPv4 server start failed (%d)", ret);
		return ret;
	}

	ap_cfg.ssid = (const uint8_t *)ap_ssid;
	ap_cfg.ssid_length = strlen(ap_ssid);
	ap_cfg.security = WIFI_SECURITY_TYPE_NONE;
	ap_cfg.channel = WIFI_CHANNEL_ANY;
	ap_cfg.band = WIFI_FREQ_BAND_2_4_GHZ;

	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &ap_cfg,
		       sizeof(ap_cfg));
	if (ret != 0) {
		LOG_ERR("AP enable failed (%d)", ret);
		return ret;
	}

	printk("\n========================================================\n");
	printk("  DEVICE SETUP -- connect a phone or laptop to Wi-Fi:\n");
	printk("      network:  %s   (open)\n", ap_ssid);
	printk("  then browse to:\n");
	printk("      http://%s/\n", PORTAL_AP_IP);
	printk("  Serial provisioning (iotcprov/iotc) still works too.\n");
	printk("========================================================\n\n");
	return 0;
}

int portal_run(void)
{
	struct net_if *ap_iface = net_if_get_wifi_sap();
	int ret;

	if (ap_iface == NULL) {
		LOG_ERR("no Wi-Fi AP interface (CONFIG_NXP_WIFI_SOFTAP_SUPPORT?)");
		return -ENODEV;
	}
	ret = ap_up(ap_iface);
	if (ret != 0) {
		return ret;
	}
	ret = http_server_start();
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("http_server_start failed (%d)", ret);
		return ret;
	}
	/* Parked: provisioning completes with a reboot from /api/finish. */
	for (;;) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
