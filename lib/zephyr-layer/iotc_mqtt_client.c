/*
 * Copyright (c) 2020-2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Zephyr MQTT-over-TLS transport seam for the IOTCONNECT SDK.
 *
 * Owns the single struct mqtt_client used by the SDK, connects it over a TLS
 * socket (sec tags supplied by the orchestrator), subscribes to the C2D topic,
 * runs the poll/keepalive message pump on its own thread, and forwards inbound
 * PUBLISH payloads into iotc-c-lib's single inbound entry point
 * (iotcl_mqtt_receive_with_length) from a dedicated workqueue -- never from the
 * MQTT receive context.
 *
 * Connectivity boundary (TODO(board)): getaddrinfo()/mqtt_connect() assume an
 * already-up network interface with L4 connectivity and a set realtime clock.
 * Bearer bring-up (Ethernet/Wi-Fi/cellular-PPP) and SNTP are the board's /
 * application's responsibility; see boards/<board>.conf and the porting guide.
 */

#include "iotc_mqtt_client.h"

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/random/random.h>

/* iotc-c-lib: single inbound entry point + error codes. */
#include "iotcl.h"
#include "iotcl_cfg.h"

LOG_MODULE_REGISTER(iotc_mqtt, CONFIG_IOTCONNECT_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Module state (single global MQTT client owned here)
 * ------------------------------------------------------------------------- */

static struct mqtt_client client;

static uint8_t rx_buf[CONFIG_IOTCONNECT_MQTT_RX_BUF_SIZE];
static uint8_t tx_buf[CONFIG_IOTCONNECT_MQTT_TX_BUF_SIZE];

static struct sockaddr_storage broker;

static volatile bool connected;

/* Copy of the connect config (strings are caller-owned and must outlive us). */
static iotc_mqtt_config_t cfg;

/* Signalled by the event handler on CONNACK so the connect path can unblock. */
static K_SEM_DEFINE(connack_sem, 0, 1);

/* Connect-time CONNACK poll timeout. */
#define IOTC_MQTT_CONNACK_TIMEOUT_MS 10000

/* -------------------------------------------------------------------------
 * Message-pump thread + C2D workqueue
 * ------------------------------------------------------------------------- */

static K_THREAD_STACK_DEFINE(pump_stack, CONFIG_IOTCONNECT_THREAD_STACK_SIZE);
static struct k_thread pump_thread;
static k_tid_t pump_tid;
static volatile bool pump_run;

static K_THREAD_STACK_DEFINE(c2d_workq_stack, CONFIG_IOTCONNECT_C2D_WORKQ_STACK_SIZE);
static struct k_work_q c2d_workq;
static bool c2d_workq_started;

/*
 * One C2D work item per inbound PUBLISH. The topic and payload are copied onto
 * the heap in the MQTT rx context and ownership is handed to the worker, which
 * calls into iotc-c-lib (cmd_cb/ota_cb run there) and then frees everything.
 */
struct c2d_work {
	struct k_work work;
	char *topic;
	uint8_t *data;
	size_t len;
};

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static void notify_status(iotc_mqtt_evt_t evt)
{
	if (cfg.status_cb) {
		cfg.status_cb(evt);
	}
}

/*
 * Resolve cfg->host into the broker sockaddr. IPv4-only for the consolidation
 * baseline.
 *
 * TODO(board): the socket family / AF is fixed to AF_INET here. If/when an
 * IPv6-capable board is added, select AF_INET6 (or AF_UNSPEC + iterate the
 * getaddrinfo result list) and fill sockaddr_in6 below accordingly. This is
 * the single point where the address family is chosen.
 */
static int resolve_broker(const char *host, uint16_t port)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,        /* TODO(board): AF_INET6 for IPv6 boards */
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *result = NULL;
	int rc;

	rc = zsock_getaddrinfo(host, NULL, &hints, &result);
	if (rc != 0 || result == NULL) {
		LOG_ERR("getaddrinfo(%s) failed: %d", host, rc);
		return -EHOSTUNREACH;
	}

	/* TODO(board): only the first AF_INET result is used. */
	struct sockaddr_in *b = (struct sockaddr_in *)&broker;
	struct sockaddr_in *r = (struct sockaddr_in *)result->ai_addr;

	memset(&broker, 0, sizeof(broker));
	b->sin_family = AF_INET;
	b->sin_port = htons(port);
	b->sin_addr = r->sin_addr;

	zsock_freeaddrinfo(result);

	char addr_str[NET_IPV4_ADDR_LEN];

	zsock_inet_ntop(AF_INET, &b->sin_addr, addr_str, sizeof(addr_str));
	LOG_INF("Broker %s resolved to %s:%u", host, addr_str, port);

	return 0;
}

/* -------------------------------------------------------------------------
 * C2D worker (off the MQTT rx context) -> iotc-c-lib
 * ------------------------------------------------------------------------- */

static void c2d_work_handler(struct k_work *item)
{
	struct c2d_work *w = CONTAINER_OF(item, struct c2d_work, work);

	/*
	 * Single iotc-c-lib inbound entry point: validates the topic against
	 * sub_c2d and fires cmd_cb/ota_cb. IOTCL_ERR_IGNORED (e.g. a topic we
	 * do not handle) is benign.
	 */
	int ret = iotcl_mqtt_receive_with_length(w->topic, w->data, w->len);

	if (ret != IOTCL_SUCCESS && ret != IOTCL_ERR_IGNORED) {
		LOG_WRN("iotcl_mqtt_receive_with_length failed: %d", ret);
	}

	k_free(w->topic);
	k_free(w->data);
	k_free(w);
}

/* Drain a PUBLISH payload into a heap buffer and dispatch it to the workqueue. */
static void handle_publish(const struct mqtt_publish_param *pub)
{
	const struct mqtt_topic *topic = &pub->message.topic;
	uint32_t payload_len = pub->message.payload.len;
	struct c2d_work *w = NULL;
	uint8_t *data = NULL;
	char *topic_str = NULL;
	int ret;

	/* Heap copies handed off to the worker (freed there). */
	data = k_malloc(payload_len + 1U);
	topic_str = k_malloc(topic->topic.size + 1U);
	w = k_malloc(sizeof(*w));
	if (!data || !topic_str || !w) {
		LOG_ERR("C2D alloc failed (payload=%u)", payload_len);
		goto drain_and_fail;
	}

	memcpy(topic_str, topic->topic.utf8, topic->topic.size);
	topic_str[topic->topic.size] = '\0';

	/*
	 * mqtt_read_publish_payload() may return short; loop until we have the
	 * whole payload (mqtt_readall_publish_payload pumps mqtt_input as needed).
	 */
	ret = mqtt_readall_publish_payload(&client, data, payload_len);
	if (ret < 0) {
		LOG_ERR("mqtt_readall_publish_payload failed: %d", ret);
		goto fail;
	}
	data[payload_len] = '\0';

	k_work_init(&w->work, c2d_work_handler);
	w->topic = topic_str;
	w->data = data;
	w->len = payload_len;

	/* QoS1 PUBACK / QoS2 PUBREC are emitted by the caller after we return. */
	k_work_submit_to_queue(&c2d_workq, &w->work);
	return;

drain_and_fail:
	/*
	 * Even on alloc failure we must consume the payload so the MQTT input
	 * buffer stays consistent; drain into a scratch buffer.
	 */
	{
		uint8_t scratch[64];

		while (mqtt_read_publish_payload(&client, scratch, sizeof(scratch)) > 0) {
			/* discard */
		}
	}
fail:
	k_free(data);
	k_free(topic_str);
	k_free(w);
}

/* -------------------------------------------------------------------------
 * MQTT event handler (runs in the pump thread's mqtt_input() context)
 * ------------------------------------------------------------------------- */

static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("CONNACK error: %d", evt->result);
			break;
		}
		LOG_INF("MQTT connected");
		connected = true;
		k_sem_give(&connack_sem);
		notify_status(IOTC_MQTT_EVT_CONNECTED);
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT disconnected: %d", evt->result);
		connected = false;
		notify_status(IOTC_MQTT_EVT_DISCONNECTED);
		break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_WRN("PUBACK error: %d", evt->result);
			notify_status(IOTC_MQTT_EVT_SEND_FAILED);
			break;
		}
		LOG_DBG("PUBACK id %u", evt->param.puback.message_id);
		notify_status(IOTC_MQTT_EVT_DELIVERED);
		break;

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *pub = &evt->param.publish;

		LOG_DBG("PUBLISH len %u qos %u", pub->message.payload.len,
			pub->message.topic.qos);

		/* Copy payload + hand off to the workqueue; never call iotcl here. */
		handle_publish(pub);

		/* Acknowledge per QoS. */
		if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {
				.message_id = pub->message_id,
			};

			mqtt_publish_qos1_ack(&client, &ack);
		} else if (pub->message.topic.qos == MQTT_QOS_2_EXACTLY_ONCE) {
			struct mqtt_pubrec_param rec = {
				.message_id = pub->message_id,
			};

			mqtt_publish_qos2_receive(&client, &rec);
		}
		break;
	}

	case MQTT_EVT_SUBACK:
		LOG_INF("SUBACK id %u result %d", evt->param.suback.message_id,
			evt->result);
		break;

	default:
		LOG_DBG("Unhandled MQTT evt %d", evt->type);
		break;
	}
}

/* -------------------------------------------------------------------------
 * Message pump thread: poll + mqtt_input + mqtt_live (keepalive)
 * ------------------------------------------------------------------------- */

static void pump_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct zsock_pollfd fds[1];

	fds[0].fd = client.transport.tls.sock;
	fds[0].events = ZSOCK_POLLIN;

	while (pump_run) {
		int timeout = mqtt_keepalive_time_left(&client);
		int rc = zsock_poll(fds, 1, timeout);

		if (rc < 0) {
			LOG_ERR("poll error: %d", errno);
			break;
		}

		if (rc > 0 && (fds[0].revents & ZSOCK_POLLIN)) {
			rc = mqtt_input(&client);
			if (rc != 0) {
				LOG_ERR("mqtt_input failed: %d", rc);
				break;
			}
		}

		if ((fds[0].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) != 0) {
			LOG_ERR("socket error revents 0x%x", fds[0].revents);
			break;
		}

		/* Always service keepalive (PINGREQ) and outbound retries. */
		rc = mqtt_live(&client);
		if (rc != 0 && rc != -EAGAIN) {
			LOG_ERR("mqtt_live failed: %d", rc);
			break;
		}
	}

	LOG_INF("MQTT pump thread exiting");
	connected = false;
}

/* -------------------------------------------------------------------------
 * Subscribe to the C2D topic
 * ------------------------------------------------------------------------- */

static int subscribe_c2d(void)
{
	struct mqtt_topic sub_topic = {
		.topic = {
			.utf8 = (const uint8_t *)cfg.sub_c2d,
			.size = strlen(cfg.sub_c2d),
		},
		.qos = (uint8_t)cfg.qos,
	};
	struct mqtt_subscription_list sub_list = {
		.list = &sub_topic,
		.list_count = 1U,
		.message_id = 1U,
	};

	LOG_INF("Subscribing to C2D: %s", cfg.sub_c2d);
	return mqtt_subscribe(&client, &sub_list);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int iotc_mqtt_client_connect(const iotc_mqtt_config_t *user_cfg)
{
	int rc;

	if (!user_cfg || !user_cfg->host || !user_cfg->client_id ||
	    !user_cfg->sub_c2d || !user_cfg->sec_tags || user_cfg->sec_tag_count == 0U) {
		return -EINVAL;
	}

	if (connected) {
		LOG_WRN("Already connected");
		return -EALREADY;
	}

	/* Snapshot the caller config (strings stay caller-owned). */
	cfg = *user_cfg;
	k_sem_reset(&connack_sem);

	/* TODO(board): assumes an up interface with L4 connectivity + valid clock. */
	rc = resolve_broker(cfg.host, cfg.port);
	if (rc != 0) {
		return rc;
	}

	mqtt_client_init(&client);

	/* --- Broker address + identity --------------------------------------- */
	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;

	client.client_id.utf8 = (const uint8_t *)cfg.client_id;
	client.client_id.size = strlen(cfg.client_id);

	/* Azure supplies a username; AWS does not (NULL => omit). */
	if (cfg.username != NULL) {
		static struct mqtt_utf8 user_name;

		user_name.utf8 = (const uint8_t *)cfg.username;
		user_name.size = strlen(cfg.username);
		client.user_name = &user_name;
	} else {
		client.user_name = NULL;
	}
	client.password = NULL;

	client.protocol_version = MQTT_VERSION_3_1_1;
	client.keepalive = CONFIG_IOTCONNECT_MQTT_KEEPALIVE_SEC;

	/* --- Buffers --------------------------------------------------------- */
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);

	/* --- TLS transport (mutual TLS via sec tags) ------------------------- */
	client.transport.type = MQTT_TRANSPORT_SECURE;

	struct mqtt_sec_config *tls = &client.transport.tls.config;

	tls->peer_verify = cfg.peer_verify;
	tls->cipher_list = NULL;
	tls->sec_tag_list = cfg.sec_tags;
	tls->sec_tag_count = cfg.sec_tag_count;
	tls->hostname = cfg.host;            /* SNI */
	tls->cert_nocopy = TLS_CERT_NOCOPY_NONE;

	/* --- Connect + wait for CONNACK -------------------------------------- */
	LOG_INF("Connecting to MQTT broker %s:%u", cfg.host, cfg.port);
	rc = mqtt_connect(&client);
	if (rc != 0) {
		LOG_ERR("mqtt_connect failed: %d", rc);
		return rc;
	}

	/* Poll the TLS socket until the event handler signals CONNACK. */
	struct zsock_pollfd fds[1] = {
		{
			.fd = client.transport.tls.sock,
			.events = ZSOCK_POLLIN,
		},
	};

	rc = zsock_poll(fds, 1, IOTC_MQTT_CONNACK_TIMEOUT_MS);
	if (rc < 0) {
		LOG_ERR("CONNACK poll failed: %d", errno);
		mqtt_abort(&client);
		return -errno;
	}
	if (rc == 0) {
		LOG_ERR("CONNACK timeout");
		mqtt_abort(&client);
		return -ETIMEDOUT;
	}

	rc = mqtt_input(&client);
	if (rc != 0) {
		LOG_ERR("mqtt_input (CONNACK) failed: %d", rc);
		mqtt_abort(&client);
		return rc;
	}

	if (k_sem_take(&connack_sem, K_MSEC(IOTC_MQTT_CONNACK_TIMEOUT_MS)) != 0 ||
	    !connected) {
		LOG_ERR("Did not receive CONNACK");
		mqtt_abort(&client);
		return -ECONNREFUSED;
	}

	/* --- Subscribe to C2D ------------------------------------------------ */
	rc = subscribe_c2d();
	if (rc != 0) {
		LOG_ERR("mqtt_subscribe failed: %d", rc);
		mqtt_disconnect(&client, NULL);
		connected = false;
		return rc;
	}

	/* --- Start the C2D workqueue ----------------------------------------- */
	if (!c2d_workq_started) {
		k_work_queue_init(&c2d_workq);
		k_work_queue_start(&c2d_workq, c2d_workq_stack,
				   K_THREAD_STACK_SIZEOF(c2d_workq_stack),
				   CONFIG_IOTCONNECT_THREAD_PRIORITY, NULL);
		k_thread_name_set(&c2d_workq.thread, "iotc_c2d");
		c2d_workq_started = true;
	}

	/* --- Start the message pump ------------------------------------------ */
	pump_run = true;
	pump_tid = k_thread_create(&pump_thread, pump_stack,
				   K_THREAD_STACK_SIZEOF(pump_stack),
				   pump_thread_fn, NULL, NULL, NULL,
				   CONFIG_IOTCONNECT_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(pump_tid, "iotc_mqtt_pump");

	LOG_INF("MQTT client connected and pumping");
	return 0;
}

void iotc_mqtt_client_disconnect(void)
{
	if (pump_run) {
		pump_run = false;
		if (pump_tid != NULL) {
			/* Unblock the pump if it is sitting in poll(). */
			(void)k_thread_join(&pump_thread, K_SECONDS(5));
			pump_tid = NULL;
		}
	}

	if (connected) {
		int rc = mqtt_disconnect(&client, NULL);

		if (rc != 0) {
			LOG_WRN("mqtt_disconnect failed: %d", rc);
		}
		connected = false;
	}

	LOG_INF("MQTT client disconnected");
}

bool iotc_mqtt_client_is_connected(void)
{
	return connected;
}

void iotc_mqtt_client_publish(const char *topic, const char *json_str)
{
	if (!topic || !json_str) {
		LOG_ERR("publish: NULL arg");
		notify_status(IOTC_MQTT_EVT_SEND_FAILED);
		return;
	}

	if (!connected) {
		LOG_WRN("publish: not connected, dropping");
		notify_status(IOTC_MQTT_EVT_SEND_FAILED);
		return;
	}

	struct mqtt_publish_param param = {
		.message = {
			.topic = {
				.topic = {
					.utf8 = (const uint8_t *)topic,
					.size = strlen(topic),
				},
				.qos = (uint8_t)cfg.qos,
			},
			.payload = {
				.data = (uint8_t *)json_str,
				.len = strlen(json_str),
			},
		},
		.message_id = sys_rand16_get() | 0x1U, /* non-zero id */
		.dup_flag = 0,
		.retain_flag = 0,
	};

	int rc = mqtt_publish(&client, &param);

	if (rc != 0) {
		LOG_ERR("mqtt_publish failed: %d", rc);
		notify_status(IOTC_MQTT_EVT_SEND_FAILED);
		return;
	}

	LOG_DBG("Published %zu bytes to %s", param.message.payload.len, topic);
}
