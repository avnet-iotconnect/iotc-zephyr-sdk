/*
 * Copyright (c) 2026 Avnet, Inc.
 * SPDX-License-Identifier: MIT
 *
 * click-telemetry demo -- auto-detect MikroE Click sensors on the mikroBUS /
 * Shuttle I2C bus and publish their data to Avnet /IOTCONNECT.
 *
 * The MikroE Shuttle expands one mikroBUS socket to 4 Click positions that
 * SHARE the mikroBUS I2C bus. At power-up the app walks a registry of the
 * supported Clicks, probes each I2C address, and marks the ones that ACK as
 * RECOGNIZED. Each publish cycle it reads every recognized Click and adds its
 * channels to one IOTCONNECT telemetry record.
 *
 * All readers are raw-I2C (no Zephyr sensor driver needed) and use the exact
 * register sequences + conversions from the proven Microchip WFI32-IoT sample
 * (github avnet-iotconnect/iotc-azurertos-sdk .../clicks). Verified live on
 * FRDM-MCXN947: Temp&Hum 14, Altitude 4, Air quality 7, Ultra-Low Press.
 *
 * I2C address map (distinct addresses required on the shared bus):
 *   0x15 T6713 CO2    0x27 Altitude 4   0x28 T9602        0x40 Temp&Hum 14
 *   0x5C VAV Press    0x6C Ultra-Low P  0x70 Air quality  0x76 Altitude 2
 * PHT (MS8607) uses 0x40 + 0x76 -> collides with Temp&Hum 14 AND Altitude 2,
 * so it is disabled in auto-detect by default; enable it only if a PHT Click is
 * the sole occupant of those addresses.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/conn_mgr_monitor.h>

#include "iotconnect.h"
#include "iotcl.h"
#include "iotcl_telemetry.h"
#include "iotcl_c2d.h"
#include "iotc_time.h"
#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
#include "iotconnect_vitals.h"
#endif
#if defined(CONFIG_BUILD_WITH_TFM)
/* TF-M /ns build: the device key is sealed in TF-M Protected Storage (loaded at
 * runtime via iotc_identity_load), so the binary carries only the PUBLIC CA
 * roots -- never a private key. */
#include "iotconnect_identity.h"
#include "quickstart_credentials.h"
#else
/* Non-TF-M build: identity is baked into the (gitignored) creds header. */
#include "device_credentials.h"
#endif

LOG_MODULE_REGISTER(click_telemetry, LOG_LEVEL_INF);

#define PUBLISH_SEC 10

/* Telemetry cadence -- adjustable at runtime via the set-reporting-interval C2D
 * command. */
static int publish_interval_sec = PUBLISH_SEC;

/* Board LED (alias led0) driven by the led-on/off/toggle C2D commands. */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static int led_state;

/* mikroBUS I2C bus shared by all 4 Shuttle positions. */
static const struct device *const i2c_bus = DEVICE_DT_GET(DT_NODELABEL(mikrobus_i2c));

/* Add "<ns>.<key> = value" to the telemetry record. The dot makes iotc-c-lib
 * nest one level (parent object "<ns>" with leaf "<key>"), matching the
 * IOTCONNECT OBJECT attributes (type OBJECT + "childs") in the device template. */
static void add_num(IotclMessageHandle msg, const char *ns, const char *key, double v)
{
	char path[48];

	snprintf(path, sizeof(path), "%s.%s", ns, key);
	iotcl_telemetry_set_number(msg, path, v);
}

/* Reader signature: read the Click at addr, add its channels, return true if
 * any value was published. */
typedef bool (*click_read_fn)(const struct device *bus, uint16_t addr,
			      IotclMessageHandle msg, const char *ns);

/* ---------------- Temp&Hum 14 @0x40 (TE HTU31D) --------------------------- *
 * reset 0x1E -> conversion 0x40 -> read cmd 0x00 (6B).
 * T = -40 + 165*raw/65535 ; RH = 100*raw/65535.                              */
static bool th14_init;
static bool read_temphum14(const struct device *bus, uint16_t addr,
			   IotclMessageHandle msg, const char *ns)
{
	uint8_t cmd, r[6] = {0};

	if (!th14_init) {
		cmd = 0x1E; /* reset */
		if (i2c_write(bus, &cmd, 1, addr) != 0) {
			return false;
		}
		k_msleep(20);
		th14_init = true;
	}
	cmd = 0x40; /* trigger conversion (lowest OSR) */
	if (i2c_write(bus, &cmd, 1, addr) != 0) {
		return false;
	}
	k_msleep(10);
	cmd = 0x00; /* READ_T_AND_RH */
	if (i2c_write_read(bus, addr, &cmd, 1, r, sizeof(r)) != 0) {
		return false;
	}
	double rt = (r[0] << 8) | r[1];
	double rh = (r[3] << 8) | r[4];

	add_num(msg, ns, "temperature_c", rt / 65535.0 * 165.0 - 40.0);
	add_num(msg, ns, "humidity_pct", rh / 65535.0 * 100.0);
	return true;
}

/* ---------------- Altitude 2 @0x76 (TE MS5607) ---------------------------- *
 * PROM cal C1..C6 (0xA2..0xAC) once; then D1 (0x46) + D2 (0x56), read 0x00 3B
 * each; 1st-order pressure/temperature compensation.                         */
static bool alt2_init;
static uint32_t alt2_c[7];
static uint32_t ms5607_adc(const struct device *bus, uint16_t addr, uint8_t conv)
{
	uint8_t z = 0x00, b[3] = {0};

	(void)i2c_write(bus, &conv, 1, addr);
	k_msleep(10);
	if (i2c_write_read(bus, addr, &z, 1, b, sizeof(b)) != 0) {
		return 0;
	}
	k_msleep(10);
	return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}
static bool read_altitude2(const struct device *bus, uint16_t addr,
			   IotclMessageHandle msg, const char *ns)
{
	if (!alt2_init) {
		uint8_t rst = 0x1E;

		if (i2c_write(bus, &rst, 1, addr) != 0) {
			return false;
		}
		k_msleep(10);
		for (int i = 1; i <= 6; i++) {
			uint8_t prom = 0xA0 + i * 2, b[2] = {0};

			if (i2c_write_read(bus, addr, &prom, 1, b, 2) != 0) {
				return false;
			}
			alt2_c[i] = (b[0] << 8) | b[1];
			k_msleep(5);
		}
		alt2_init = true;
	}

	uint32_t d1 = ms5607_adc(bus, addr, 0x46); /* pressure */
	uint32_t d2 = ms5607_adc(bus, addr, 0x56); /* temperature */

	if (d1 == 0 || d2 == 0) {
		return false;
	}
	double dT = (double)d2 - (double)alt2_c[5] * 256.0;
	double temp = (2000.0 + dT * (double)alt2_c[6] / 8388608.0) / 100.0;
	double off = (double)alt2_c[2] * 131072.0 + ((double)alt2_c[4] * dT) / 64.0;
	double sens = (double)alt2_c[1] * 65536.0 + ((double)alt2_c[3] * dT) / 128.0;
	double press = ((((double)d1 * sens / 2097152.0) - off) / 32768.0) / 100.0; /* mbar */
	double alt = ((pow(1013.25 / press, 0.19022256) - 1.0) *
		      (temp + 273.15)) / 0.0065;

	add_num(msg, ns, "pressure_mbar", press);
	add_num(msg, ns, "temperature_c", temp);
	add_num(msg, ns, "altitude_m", alt);
	return true;
}

/* ---------------- Altitude 4 @0x27 --------------------------------------- *
 * measure 0xAC -> read 5B [st,P_hi,P_lo,T_hi,T_lo].
 * P = raw/65.535 + 260 hPa ; T = raw/524.28 - 40.                            */
static bool read_altitude4(const struct device *bus, uint16_t addr,
			   IotclMessageHandle msg, const char *ns)
{
	uint8_t cmd = 0xAC, r[5] = {0};

	if (i2c_write(bus, &cmd, 1, addr) != 0) {
		return false;
	}
	k_msleep(20);
	if (i2c_read(bus, r, sizeof(r), addr) != 0) {
		return false;
	}
	double rp = (r[1] << 8) | r[2];
	double rt = (r[3] << 8) | r[4];
	double press = rp / 65.535 + 260.0; /* hPa */
	double temp = rt / 524.28 - 40.0;
	double alt = ((pow(1013.25 / press, 0.19022256) - 1.0) *
		      (temp + 273.15)) / 0.0065;

	add_num(msg, ns, "pressure_hpa", press);
	add_num(msg, ns, "temperature_c", temp);
	add_num(msg, ns, "altitude_m", alt);
	return true;
}

/* ---------------- Ultra-Low Press @0x6C ---------------------------------- *
 * clear STATUS 0x36, wait ready, read temp 0x2E / press 0x30 (2B words).
 * T = (raw - B0)/B1 ; P = -20 + (raw+26215)/52429*520 Pa.                    */
static bool read_ultralowpress(const struct device *bus, uint16_t addr,
			       IotclMessageHandle msg, const char *ns)
{
	uint8_t clr[3] = {0x36, 0xFF, 0xFF};
	uint8_t reg, w[2] = {0};

	if (i2c_write(bus, clr, sizeof(clr), addr) != 0) {
		return false;
	}
	k_msleep(10);
	for (int i = 0; i < 20; i++) {
		uint8_t sreg = 0x36, st[2] = {0};

		if (i2c_write_read(bus, addr, &sreg, 1, st, 2) == 0) {
			uint16_t status = (st[0] << 8) | st[1];

			if ((status & 0x0010) && (status & 0x0008)) {
				break;
			}
		}
		k_msleep(5);
	}
	reg = 0x2E;
	if (i2c_write_read(bus, addr, &reg, 1, w, 2) != 0) {
		return false;
	}
	int16_t traw = (int16_t)((w[0] << 8) | w[1]);

	reg = 0x30;
	(void)i2c_write_read(bus, addr, &reg, 1, w, 2);
	int16_t praw = (int16_t)((w[0] << 8) | w[1]);

	add_num(msg, ns, "pressure_pa",
		-20.0 + (((double)praw + 26215.0) / 52429.0) * 520.0);
	add_num(msg, ns, "temperature_c", ((double)traw - (-16881.0)) / 397.2);
	return true;
}

/* ---------------- VAV Press @0x5C ---------------------------------------- *
 * reset 0x11 -> start conversion 0x20 once; then read 4B.
 * P = 2sComp(word)/1200 ; T = (2sComp(word)-105)/72 + 23.1.                  */
static bool vav_init;
static bool read_vavpress(const struct device *bus, uint16_t addr,
			  IotclMessageHandle msg, const char *ns)
{
	uint8_t r[4] = {0};

	if (!vav_init) {
		uint8_t cmd = 0x11; /* reset firmware */

		if (i2c_write(bus, &cmd, 1, addr) != 0) {
			return false;
		}
		k_msleep(10);
		cmd = 0x20; /* start pressure conversion */
		(void)i2c_write(bus, &cmd, 1, addr);
		k_msleep(10);
		vav_init = true;
	}
	if (i2c_read(bus, r, sizeof(r), addr) != 0) {
		return false;
	}
	int16_t praw = (int16_t)((r[1] << 8) | r[0]);
	int16_t traw = (int16_t)((r[3] << 8) | r[2]);

	add_num(msg, ns, "pressure_pa", (double)praw / 1200.0);
	add_num(msg, ns, "temperature_c", ((double)traw - 105.0) / 72.0 + 23.1);
	return true;
}

/* ---------------- Air quality 7 @0x70 (Amphenol MiCS-VZ-89TE) ------------- *
 * GetStatus {0x0C,0,0,0,0,0xF3} -> read 7B -> CO2eq / tVOC.                  */
static bool read_airquality7(const struct device *bus, uint16_t addr,
			     IotclMessageHandle msg, const char *ns)
{
	uint8_t cmd[6] = {0x0C, 0x00, 0x00, 0x00, 0x00, 0xF3};
	uint8_t r[7] = {0};

	if (i2c_write(bus, cmd, sizeof(cmd), addr) != 0) {
		return false;
	}
	k_msleep(100);
	if (i2c_read(bus, r, sizeof(r), addr) != 0) {
		return false;
	}
	add_num(msg, ns, "co2eq_ppm", (r[0] - 13) * (1600.0 / 229.0) + 400.0);
	add_num(msg, ns, "tvoc_ppb", (r[1] - 13) * (1000.0 / 229.0));
	return true;
}

/* ---------------- T6713 CO2 @0x15 ---------------------------------------- *
 * cmd {0x04,0x13,0x8B,0x00,0x01} -> read 4B -> CO2 = b[2]*256 + b[3].        */
static bool read_t6713(const struct device *bus, uint16_t addr,
		       IotclMessageHandle msg, const char *ns)
{
	uint8_t cmd[5] = {0x04, 0x13, 0x8B, 0x00, 0x01};
	uint8_t r[4] = {0};

	if (i2c_write(bus, cmd, sizeof(cmd), addr) != 0) {
		return false;
	}
	k_msleep(100);
	if (i2c_read(bus, r, sizeof(r), addr) != 0) {
		return false;
	}
	add_num(msg, ns, "co2_ppm", (double)((r[2] << 8) | r[3]));
	return true;
}

/* ---------------- T9602 Temp/Hum @0x28 ----------------------------------- *
 * read 4B (no cmd). RH = ((b0&63)*256+b1)/16384*100 ;
 * T = (b2*64 + (b3>>2))/16384*165 - 40.                                      */
static bool read_t9602(const struct device *bus, uint16_t addr,
		       IotclMessageHandle msg, const char *ns)
{
	uint8_t r[4] = {0};

	if (i2c_read(bus, r, sizeof(r), addr) != 0) {
		return false;
	}
	k_msleep(100);
	double rh = ((r[0] & 63) * 256 + r[1]) / 16384.0 * 100.0;
	double t = (r[2] * 64 + (r[3] >> 2)) / 16384.0 * 165.0 - 40.0;

	add_num(msg, ns, "humidity_pct", rh);
	add_num(msg, ns, "temperature_c", t);
	return true;
}

/* ---------------- PHT @0x40(RH) + 0x76(P&T) (TE MS8607) ------------------- *
 * P&T identical to MS5607 at 0x76; RH read at 0x40.
 * Disabled by default (address collision) -- see file header.               */
static bool pht_init;
static uint32_t pht_c[7];
static bool read_pht(const struct device *bus, uint16_t addr,
		     IotclMessageHandle msg, const char *ns)
{
	ARG_UNUSED(addr);
	const uint16_t pt = 0x76, rha = 0x40;

	if (!pht_init) {
		uint8_t rst = 0x1E;

		if (i2c_write(bus, &rst, 1, pt) != 0) {
			return false;
		}
		k_msleep(10);
		for (int i = 1; i <= 6; i++) {
			uint8_t prom = 0xA0 + i * 2, b[2] = {0};

			if (i2c_write_read(bus, pt, &prom, 1, b, 2) != 0) {
				return false;
			}
			pht_c[i] = (b[0] << 8) | b[1];
			k_msleep(5);
		}
		pht_init = true;
	}

	uint32_t d1 = ms5607_adc(bus, pt, 0x46);
	uint32_t d2 = ms5607_adc(bus, pt, 0x56);

	if (d1 && d2) {
		double dT = (double)d2 - (double)pht_c[5] * 256.0;
		double temp = (2000.0 + dT * (double)pht_c[6] / 8388608.0) / 100.0;
		double off = (double)pht_c[2] * 131072.0 + ((double)pht_c[4] * dT) / 64.0;
		double sens = (double)pht_c[1] * 65536.0 + ((double)pht_c[3] * dT) / 128.0;

		add_num(msg, ns, "pressure_mbar",
			((((double)d1 * sens / 2097152.0) - off) / 32768.0) / 100.0);
		add_num(msg, ns, "temperature_c", temp);
	}

	uint8_t rcmd = 0xE5, rb[2] = {0}; /* measure RH, hold */

	if (i2c_write_read(bus, rha, &rcmd, 1, rb, 2) == 0) {
		double raw = (rb[0] << 8) | rb[1];

		add_num(msg, ns, "humidity_pct", (raw * 12500.0 / 65536.0 - 600.0) / 100.0);
	}
	return true;
}

/* ---------------- IO1 Xplained Pro @0x4F (AT30TSE758) ---------------------- *
 * Not a Click: Microchip's Xplained Pro sensor wing, same probe-and-read
 * pattern on the shared bus. Config reg (ptr 0x01) bits 14:13=11 -> 12-bit;
 * temp reg (ptr 0x00) is left-justified two's complement, 0.0625 C/LSB.
 * (Its EEPROM half sits separately at 0x57 and is not probed.)              */
static bool read_io1_temp(const struct device *bus, uint16_t addr,
			  IotclMessageHandle msg, const char *ns)
{
	uint8_t cfg[3] = { 0x01, 0x60, 0x00 };
	uint8_t reg = 0x00, b[2] = {0};

	(void)i2c_write(bus, cfg, sizeof(cfg), addr);
	if (i2c_write_read(bus, addr, &reg, 1, b, sizeof(b)) != 0) {
		return false;
	}
	add_num(msg, ns, "temperature_c",
		(double)((int16_t)((b[0] << 8) | b[1]) >> 4) * 0.0625);
	return true;
}

/* --- Registry ------------------------------------------------------------- */
struct click {
	const char *name;
	const char *part;
	const char *ns;   /* telemetry key namespace */
	uint16_t addr;    /* primary I2C probe address */
	click_read_fn read;
	bool enabled;     /* included in auto-detect */
	bool present;
};

static struct click clicks[] = {
	{ "Temp&Hum 14",     "TE HTU31D",         "temp_hum_14",     0x40, read_temphum14,     true,  false },
	{ "Altitude 2",      "TE MS5607",         "altitude_2",      0x76, read_altitude2,     true,  false },
	{ "Altitude 4",      "baro (0xAC)",       "altitude_4",      0x27, read_altitude4,     true,  false },
	{ "Ultra-Low Press", "diff-press (0x6C)", "ultra_low_press", 0x6C, read_ultralowpress, true,  false },
	{ "VAV Press",       "diff-press (0x5C)", "vav_press",       0x5C, read_vavpress,      true,  false },
	{ "Air quality 7",   "MiCS-VZ-89TE",      "air_quality_7",   0x70, read_airquality7,   true,  false },
	{ "T6713 CO2",       "Amphenol T6713",    "t6713",           0x15, read_t6713,         true,  false },
	{ "T9602",           "Amphenol T9602",    "t9602",           0x28, read_t9602,         true,  false },
	{ "PHT",             "TE MS8607",         "pht",             0x40, read_pht,           false, false },
	{ "IO1 Xplained Pro", "AT30TSE758",       "io1",             0x4F, read_io1_temp,      true,  false },
};

/* Best-effort I2C presence probe (ACK on a 1-byte read). */
static bool i2c_present(uint16_t addr)
{
	uint8_t b;

	return i2c_read(i2c_bus, &b, 1, addr) == 0;
}

static void detect_clicks(void)
{
	LOG_INF("Scanning mikroBUS/Shuttle Click positions...");
	for (size_t i = 0; i < ARRAY_SIZE(clicks); i++) {
		struct click *c = &clicks[i];

		c->present = c->enabled && i2c_present(c->addr);
		LOG_INF("  [%s] %-16s 0x%02X  %s",
			c->present ? "RECOGNIZED" : "  absent  ",
			c->name, c->addr, c->part);
	}
}

static void publish_clicks(void)
{
	IotclMessageHandle msg = iotcl_telemetry_create();
	char summary[128];
	size_t off = 0, n = 0;

	if (msg == NULL) {
		return;
	}
	summary[0] = '\0';
	for (size_t i = 0; i < ARRAY_SIZE(clicks); i++) {
		struct click *c = &clicks[i];

		if (c->present && c->read(i2c_bus, c->addr, msg, c->ns)) {
			n++;
			off += snprintf(summary + off, sizeof(summary) - off,
					"%s%s", n > 1 ? " " : "", c->ns);
		}
	}
	if (n > 0) {
#if defined(CONFIG_IOTCONNECT_DEVICE_VITALS)
		iotc_vitals_append(msg);
#endif
		(void)iotcl_mqtt_send_telemetry(msg, false);
		LOG_INF("Published telemetry from %u Click(s): %s", (unsigned)n, summary);
	}
	iotcl_telemetry_destroy(msg);
}

/* --- C2D command handling ------------------------------------------------- */

static void set_led(int on)
{
	led_state = on ? 1 : 0;
	if (device_is_ready(led.port)) {
		gpio_pin_set_dt(&led, led_state);
	}
	LOG_INF("LED -> %s", led_state ? "ON" : "OFF");
}

/* Case-insensitive substring match (picolibc may lack strcasestr). */
static bool ci_contains(const char *haystack, const char *needle)
{
	for (const char *h = haystack; *h != '\0'; h++) {
		size_t i = 0;

		while (needle[i] != '\0' &&
		       tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (needle[i] == '\0') {
			return true;
		}
	}
	return needle[0] == '\0';
}

/* Parse the first integer found in a command line (e.g. "set-... 30" -> 30). */
static int first_int(const char *s)
{
	while (*s != '\0' && (*s < '0' || *s > '9')) {
		s++;
	}
	return atoi(s);
}

static void on_command(IotclC2dEventData data)
{
	const char *cmd = iotcl_c2d_get_command(data);
	const char *ack = iotcl_c2d_get_ack_id(data);
	int status = IOTCL_C2D_EVT_CMD_FAILED;

	LOG_INF("C2D command: %s", cmd ? cmd : "(null)");
	if (cmd != NULL) {
		if (ci_contains(cmd, "toggle")) {
			set_led(!led_state);
			status = IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK;
		} else if (ci_contains(cmd, "off")) {
			set_led(0);
			status = IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK;
		} else if (ci_contains(cmd, "on")) {
			set_led(1);
			status = IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK;
		} else if (ci_contains(cmd, "interval")) {
			int v = first_int(cmd);

			if (v >= 1 && v <= 3600) {
				publish_interval_sec = v;
				LOG_INF("Reporting interval -> %d s", v);
				status = IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK;
			}
		} else if (ci_contains(cmd, "reboot")) {
			LOG_WRN("Reboot requested via C2D");
			if (ack != NULL) {
				(void)iotcl_mqtt_send_cmd_ack(
					ack, IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK, NULL);
			}
			k_sleep(K_MSEC(500));
			sys_reboot(SYS_REBOOT_COLD);
		} else {
			LOG_WRN("Unrecognized command");
		}
	}
	if (ack != NULL) {
		(void)iotcl_mqtt_send_cmd_ack(ack, status,
			status == IOTCL_C2D_EVT_CMD_FAILED ? "unknown command" : NULL);
	}
}

/* --- Network bring-up (same pattern as the other demos) ------------------- */

static K_SEM_DEFINE(l4_connected_sem, 0, 1);
static struct net_mgmt_event_callback l4_cb;
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		k_sem_give(&l4_connected_sem);
	}
}

static int network_up(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		return -ENODEV;
	}
	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);
	if (!net_if_is_up(iface)) {
		int ret = net_if_up(iface);

		if (ret && ret != -EALREADY) {
			return ret;
		}
	}
	conn_mgr_mon_resend_status();
	LOG_INF("Waiting for network connectivity...");
	k_sem_take(&l4_connected_sem, K_FOREVER);
	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("click-telemetry demo starting");

	if (!device_is_ready(i2c_bus)) {
		LOG_ERR("mikroBUS I2C not ready");
		return 0;
	}
	if (device_is_ready(led.port)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	detect_clicks();

	if (network_up() != 0) {
		return 0;
	}
	ret = iotc_time_sync(CONFIG_IOTCONNECT_SNTP_SERVER,
			     CONFIG_IOTCONNECT_SNTP_TIMEOUT_MS);
	if (ret) {
		LOG_ERR("SNTP sync failed (%d)", ret);
		return 0;
	}

	IotConnectClientConfig config;

	iotconnect_sdk_init_config(&config);
#if defined(CONFIG_IOTCONNECT_CT_AWS)
	config.connection_type = IOTC_CT_AWS;
#elif defined(CONFIG_IOTCONNECT_CT_AZURE)
	config.connection_type = IOTC_CT_AZURE;
#endif
#if defined(CONFIG_BUILD_WITH_TFM)
	/* Identity (cpid/env/duid + device cert/key) comes from the TF-M-sealed
	 * blob in Protected Storage; provision it once with the quickstart flow
	 * (iotcprov provision <duid> + iotc config). Only the public CA roots are
	 * compiled in. */
	struct iotc_identity id;

	if (iotc_identity_load(&id) != 0) {
		printk("\nDevice not provisioned. Seal an identity first via the\n"
		       "quickstart flow:  iotcprov provision <duid>  ->  register the\n"
		       "cert in IOTCONNECT  ->  iotc config  ->  kernel reboot cold.\n\n");
		return 0; /* idle at the shell for provisioning */
	}
	config.cpid = (char *)id.cpid;
	config.env = (char *)id.env;
	config.duid = (char *)id.duid;
	config.auth_info.type = IOTC_AT_X509;
	config.auth_info.ca_cert = broker_ca_pem;
	config.auth_info.ca_cert_len = sizeof(broker_ca_pem);
	config.auth_info.dra_ca = dra_ca_pem;
	config.auth_info.dra_ca_len = sizeof(dra_ca_pem);
	config.auth_info.data.cert_info.device_cert = id.device_cert;
	config.auth_info.data.cert_info.device_cert_len = id.device_cert_len;
	config.auth_info.data.cert_info.device_key = id.device_key;
	config.auth_info.data.cert_info.device_key_len = id.device_key_len;
#else
	config.cpid = NULL;
	config.env = NULL;
	config.duid = NULL;
	config.auth_info.type = IOTC_AT_X509;
	config.auth_info.ca_cert = broker_ca_pem;
	config.auth_info.ca_cert_len = sizeof(broker_ca_pem);
	config.auth_info.dra_ca = dra_ca_pem;
	config.auth_info.dra_ca_len = sizeof(dra_ca_pem);
	config.auth_info.data.cert_info.device_cert = device_cert_pem;
	config.auth_info.data.cert_info.device_cert_len = sizeof(device_cert_pem);
	config.auth_info.data.cert_info.device_key = device_key_pem;
	config.auth_info.data.cert_info.device_key_len = sizeof(device_key_pem);
#endif
	config.cmd_cb = on_command;
	config.verbose = true;

	ret = iotconnect_sdk_init(&config);
	if (ret) {
		LOG_ERR("iotconnect_sdk_init failed (%d)", ret);
		return 0;
	}

	while (true) {
		ret = iotconnect_sdk_connect();
		if (ret) {
			LOG_ERR("connect failed (%d); retrying", ret);
			k_sleep(K_SECONDS(5));
			continue;
		}
		while (iotconnect_sdk_is_connected()) {
			publish_clicks();
			k_sleep(K_SECONDS(publish_interval_sec));
		}
		iotconnect_sdk_disconnect();
		LOG_WRN("Disconnected; reconnecting...");
	}
	return 0;
}
