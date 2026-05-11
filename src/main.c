/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/location.h>
#include <modem/nrf_modem_lib.h>
#include <date_time.h>

#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
#include <golioth/client.h>
#include <golioth/stream.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/socket.h>
#include <zcbor_encode.h>

#define GOLIOTH_PSK_ID CONFIG_GOLIOTH_SAMPLE_PSK_ID
#define GOLIOTH_PSK    CONFIG_GOLIOTH_SAMPLE_PSK

/* If PSK in Kconfig is 64/128 hex chars (Golioth UI), decode to raw bytes then re-encode as
 * lowercase hex for AT%%CMNG. Otherwise use master behavior: hex-encode each ASCII byte of
 * CONFIG_GOLIOTH_SAMPLE_PSK for the modem.
 */
static bool golioth_psk_string_is_hex(const char *s, size_t len)
{
	if (len < 2U || (len % 2U) != 0U) {
		return false;
	}

	for (size_t i = 0; i < len; i++) {
		if (!isxdigit((unsigned char)s[i])) {
			return false;
		}
	}

	return true;
}

static int golioth_psk_hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_cap,
				  size_t *out_len)
{
	if ((hex_len % 2U) != 0U || hex_len / 2U > out_cap) {
		return -EINVAL;
	}

	for (size_t i = 0; i < hex_len / 2U; i++) {
		char h = hex[i * 2];
		char l = hex[i * 2 + 1];
		uint8_t vh;
		uint8_t vl;

		if (h >= '0' && h <= '9') {
			vh = (uint8_t)(h - '0');
		} else if (h >= 'a' && h <= 'f') {
			vh = (uint8_t)(10 + (h - 'a'));
		} else if (h >= 'A' && h <= 'F') {
			vh = (uint8_t)(10 + (h - 'A'));
		} else {
			return -EINVAL;
		}

		if (l >= '0' && l <= '9') {
			vl = (uint8_t)(l - '0');
		} else if (l >= 'a' && l <= 'f') {
			vl = (uint8_t)(10 + (l - 'a'));
		} else if (l >= 'A' && l <= 'F') {
			vl = (uint8_t)(10 + (l - 'A'));
		} else {
			return -EINVAL;
		}

		out[i] = (uint8_t)((vh << 4) | vl);
	}

	*out_len = hex_len / 2U;
	return 0;
}

static struct golioth_client *golioth_client;
static K_SEM_DEFINE(golioth_connected_sem, 0, 1);

static const struct golioth_client_config golioth_cfg = {
	.credentials = {
		.auth_type = GOLIOTH_TLS_AUTH_TYPE_TAG,
		.tag = CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG,
	},
};

static int golioth_provision_psk_credentials(void)
{
	/* 64-byte key as hex = 128 chars + NUL fits; master used 2 * CONFIG string len */
	char psk_hex[256];
	const char hex_digits[] = "0123456789abcdef";
	const char *psk_id = GOLIOTH_PSK_ID;
	const char *psk = GOLIOTH_PSK;
	const size_t psk_id_len = strlen(psk_id);
	const size_t psk_ascii_len = strlen(psk);
	const int tag = CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG;
	uint8_t psk_bin[64];
	int err;

	if (psk_id_len == 0 || psk_ascii_len == 0) {
		printk("[golioth] missing credentials: set CONFIG_GOLIOTH_SAMPLE_PSK_ID and "
		       "CONFIG_GOLIOTH_SAMPLE_PSK (e.g. west build ... "
		       "-DEXTRA_CONF_FILE=credentials.conf)\n");
		return -EINVAL;
	}

	if ((psk_ascii_len == 64U || psk_ascii_len == 128U) &&
	    golioth_psk_string_is_hex(psk, psk_ascii_len)) {
		size_t raw_len;

		err = golioth_psk_hex_decode(psk, psk_ascii_len, psk_bin, sizeof(psk_bin),
					   &raw_len);
		if (err) {
			printk("[golioth] PSK hex decode failed\n");
			return err;
		}

		if (2U * raw_len + 1U > sizeof(psk_hex)) {
			return -EINVAL;
		}

		for (size_t i = 0; i < raw_len; i++) {
			psk_hex[2 * i] = hex_digits[psk_bin[i] >> 4];
			psk_hex[2 * i + 1] = hex_digits[psk_bin[i] & 0x0f];
		}
		psk_hex[2 * raw_len] = '\0';
		printk("[golioth] PSK: Golioth hex UI %zu chars -> %zu key bytes -> modem\n",
		       psk_ascii_len, raw_len);
	} else {
		if (psk_ascii_len * 2U + 1U > sizeof(psk_hex)) {
			printk("[golioth] PSK too long for modem buffer\n");
			return -EINVAL;
		}

		for (size_t i = 0; i < psk_ascii_len; i++) {
			unsigned char value = (unsigned char)psk[i];

			psk_hex[i * 2] = hex_digits[value >> 4];
			psk_hex[i * 2 + 1] = hex_digits[value & 0x0f];
		}
		psk_hex[psk_ascii_len * 2] = '\0';
	}

	/* Ignore delete failures when the credentials do not exist yet. */
	(void)nrf_modem_at_printf("AT%%CMNG=3,%d,4", tag);
	(void)nrf_modem_at_printf("AT%%CMNG=3,%d,3", tag);

	err = nrf_modem_at_printf("AT%%CMNG=0,%d,4,\"%s\"", tag, psk_id);
	if (err) {
		printk("[golioth] failed to provision PSK ID to sec tag %d: %d\n", tag, err);
		return err;
	}

	err = nrf_modem_at_printf("AT%%CMNG=0,%d,3,\"%s\"", tag, psk_hex);
	if (err) {
		printk("[golioth] failed to provision PSK to sec tag %d: %d\n", tag, err);
		return err;
	}

	printk("[golioth] provisioned PSK credentials to modem sec tag %d\n", tag);

	return 0;
}

static void golioth_on_client_event(struct golioth_client *client,
				     enum golioth_client_event event,
				     void *arg)
{
	ARG_UNUSED(client);
	ARG_UNUSED(arg);

	printk("[golioth] client event: %d\n", event);

	if (event == GOLIOTH_CLIENT_EVENT_CONNECTED) {
		printk("Connected to Golioth\n");
		k_sem_give(&golioth_connected_sem);
	}
}

static void publish_location_to_golioth(const struct location_event_data *event_data)
{
	if (!golioth_client) {
		printk("[golioth] publish skipped: client not initialized\n");
		return;
	}

	char payload[196];
	int len = snprintk(payload, sizeof(payload),
		"{\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.1f,\"method\":\"%s\"}",
		event_data->location.latitude,
		event_data->location.longitude,
		(double)event_data->location.accuracy,
		location_method_str(event_data->method));

	if (len <= 0 || len >= (int)sizeof(payload)) {
		printk("[golioth] failed to encode location JSON\n");
		return;
	}

	printk("[golioth] publishing location JSON to path 'loc/fix': %s\n", payload);

	enum golioth_status status = golioth_stream_set(golioth_client,
							"loc/fix",
							GOLIOTH_CONTENT_TYPE_JSON,
							payload, len,
							NULL, NULL);
	if (status != GOLIOTH_OK) {
		printk("Failed to publish location to Golioth: %d\n", status);
	} else {
		printk("Location published to Golioth path 'loc/fix'\n");
	}
}

static void publish_status_to_golioth(const char *state, int32_t code)
{
	if (!golioth_client) {
		printk("[golioth] status publish skipped: client not initialized\n");
		return;
	}

	char payload[128];
	int len = snprintk(payload, sizeof(payload),
		"{\"state\":\"%s\",\"code\":%d}", state, (int)code);

	if (len <= 0 || len >= (int)sizeof(payload)) {
		printk("[golioth] failed to encode status JSON\n");
		return;
	}

	enum golioth_status status = golioth_stream_set(golioth_client,
							"loc/stat",
							GOLIOTH_CONTENT_TYPE_JSON,
							payload, len,
							NULL, NULL);
	if (status != GOLIOTH_OK) {
		printk("[golioth] failed to publish status '%s': %d\n", state, status);
	} else {
		printk("[golioth] status published to path 'loc/stat': %s (%d)\n", state, code);
	}
}

/* Latest fix for bundling with periodic sensor telemetry on LightDB Stream. */
static K_MUTEX_DEFINE(golioth_last_loc_mtx);
static struct {
	double lat;
	double lon;
	double acc_m;
	char method[24];
	bool valid;
} golioth_last_loc;

static void golioth_store_last_location(const struct location_event_data *ev)
{
	k_mutex_lock(&golioth_last_loc_mtx, K_FOREVER);
	golioth_last_loc.lat = ev->location.latitude;
	golioth_last_loc.lon = ev->location.longitude;
	golioth_last_loc.acc_m = (double)ev->location.accuracy;
	{
		const char *m = location_method_str(ev->method);

		strncpy(golioth_last_loc.method, m, sizeof(golioth_last_loc.method) - 1);
		golioth_last_loc.method[sizeof(golioth_last_loc.method) - 1] = '\0';
	}
	golioth_last_loc.valid = true;
	k_mutex_unlock(&golioth_last_loc_mtx);
}

/*
 * Periodic JSON to path telemetry/snapshot: env + accelerometer + last known location.
 * Stream paths are hierarchical; Golioth LightDB Stream shows telemetry/snapshot.
 */
static void golioth_publish_telemetry_snapshot(double temp_c, double rh_pct, bool have_env,
					       double ax, double ay, double az, bool have_motion)
{
	char payload[512];
	int len;
	bool have_loc;
	double lat, lon, acc_m;
	char method[24];

	if (!golioth_client || !golioth_client_is_connected(golioth_client)) {
		return;
	}

	k_mutex_lock(&golioth_last_loc_mtx, K_FOREVER);
	have_loc = golioth_last_loc.valid;
	lat = golioth_last_loc.lat;
	lon = golioth_last_loc.lon;
	acc_m = golioth_last_loc.acc_m;
	strncpy(method, golioth_last_loc.method, sizeof(method) - 1);
	method[sizeof(method) - 1] = '\0';
	k_mutex_unlock(&golioth_last_loc_mtx);

	len = snprintk(payload, sizeof(payload),
		       "{\"uptime_ms\":%lld,"
		       "\"env_ok\":%d,\"temp_c\":%.2f,\"rh_pct\":%.1f,"
		       "\"motion_ok\":%d,\"ax_ms2\":%.5f,\"ay_ms2\":%.5f,\"az_ms2\":%.5f,"
		       "\"loc_ok\":%d,\"lat\":%.7f,\"lon\":%.7f,\"loc_acc_m\":%.1f,"
		       "\"loc_method\":\"%s\"}",
		       k_uptime_get(),
		       have_env ? 1 : 0, temp_c, rh_pct,
		       have_motion ? 1 : 0, ax, ay, az,
		       have_loc ? 1 : 0, lat, lon, acc_m, method);

	if (len <= 0 || len >= (int)sizeof(payload)) {
		printk("[golioth] telemetry JSON encode failed or truncated (len=%d)\n", len);
		return;
	}

	enum golioth_status status = golioth_stream_set(golioth_client,
							"telemetry/snapshot",
							GOLIOTH_CONTENT_TYPE_JSON,
							payload, (size_t)len,
							NULL, NULL);

	if (status != GOLIOTH_OK) {
		printk("[golioth] telemetry/snapshot publish failed: %d\n", status);
	}
}
#endif /* CONFIG_GOLIOTH_FIRMWARE_SDK */

static K_SEM_DEFINE(location_event, 0, 1);

static K_SEM_DEFINE(lte_connected, 0, 1);

static K_SEM_DEFINE(time_update_finished, 0, 1);

static struct k_work_delayable gnss_progress_work;
static bool gnss_debug_active;
static int64_t gnss_debug_start_ms;
static struct k_work_delayable boot_heartbeat_work;
static bool modem_ready_for_debug;
static const char *boot_stage = "reset";
static struct k_work_delayable boot_led_work;

#if IS_ENABLED(CONFIG_SHT4X) && DT_NODE_HAS_STATUS(DT_ALIAS(sht40), okay)
static const struct device *const sht40_dev = DEVICE_DT_GET(DT_ALIAS(sht40));
#endif
#if IS_ENABLED(CONFIG_LIS2DH) && DT_NODE_HAS_STATUS(DT_ALIAS(accel0), okay)
static const struct device *const accel_dev = DEVICE_DT_GET(DT_ALIAS(accel0));
#endif

#if (IS_ENABLED(CONFIG_SHT4X) && DT_NODE_HAS_STATUS(DT_ALIAS(sht40), okay)) || \
	(IS_ENABLED(CONFIG_LIS2DH) && DT_NODE_HAS_STATUS(DT_ALIAS(accel0), okay))
static struct k_work_delayable sensor_poll_work;

static void sensor_poll_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	bool have_env = false;
	double temp_c = 0.0;
	double rh_pct = 0.0;
	bool have_motion = false;
	double ax_ms2 = 0.0;
	double ay_ms2 = 0.0;
	double az_ms2 = 0.0;

#if IS_ENABLED(CONFIG_SHT4X) && DT_NODE_HAS_STATUS(DT_ALIAS(sht40), okay)
	if (device_is_ready(sht40_dev)) {
		struct sensor_value t, rh;
		int err = sensor_sample_fetch(sht40_dev);

		if (err) {
			printk("[sensor] SHT40 fetch failed: %d\n", err);
		} else {
			err = sensor_channel_get(sht40_dev, SENSOR_CHAN_AMBIENT_TEMP, &t);
			if (!err) {
				err = sensor_channel_get(sht40_dev, SENSOR_CHAN_HUMIDITY, &rh);
			}
			if (err) {
				printk("[sensor] SHT40 channel read failed: %d\n", err);
			} else {
				temp_c = sensor_value_to_double(&t);
				rh_pct = sensor_value_to_double(&rh);
				have_env = true;
				printk("[sensor] SHT40: %.2f degC, %.1f %%RH\n", temp_c, rh_pct);
			}
		}
	} else {
		printk("[sensor] SHT40 not ready\n");
	}
#endif

#if IS_ENABLED(CONFIG_LIS2DH) && DT_NODE_HAS_STATUS(DT_ALIAS(accel0), okay)
	if (device_is_ready(accel_dev)) {
		struct sensor_value acc[3];
		int err = sensor_sample_fetch(accel_dev);

		if (err) {
			printk("[sensor] LIS2DH fetch failed: %d\n", err);
		} else {
			err = sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_XYZ, acc);
			if (err) {
				printk("[sensor] LIS2DH channel read failed: %d\n", err);
			} else {
				ax_ms2 = sensor_value_to_double(&acc[0]);
				ay_ms2 = sensor_value_to_double(&acc[1]);
				az_ms2 = sensor_value_to_double(&acc[2]);
				have_motion = true;
				printk("[sensor] LIS2DH (m/s^2): X=%.3f Y=%.3f Z=%.3f\n",
				       ax_ms2, ay_ms2, az_ms2);
			}
		}
	} else {
		printk("[sensor] LIS2DH not ready\n");
	}
#endif

#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
	golioth_publish_telemetry_snapshot(temp_c, rh_pct, have_env, ax_ms2, ay_ms2, az_ms2,
					   have_motion);
#endif

	k_work_reschedule(&sensor_poll_work, K_SECONDS(10));
}
#endif

static uint8_t boot_stage_lookup_led_blinks(const char *stage)
{
	static const struct {
		const char *name;
		uint8_t blinks;
	} map[] = {
		{ "main_entry", 1 },
		{ "date_time_handler_register", 2 },
		{ "nrf_modem_lib_init_begin", 3 },
		{ "nrf_modem_lib_init_done", 4 },
		{ "lte_handler_register", 5 },
		{ "golioth_psk_provision", 6 },
		{ "lte_connect_request", 7 },
		{ "lte_wait_registered", 8 },
		{ "lte_registered", 9 },
		{ "gnss_antenna_enable", 10 },
		{ "network_wait_ready", 11 },
		{ "golioth_client_create", 12 },
		{ "golioth_wait_connected", 13 },
		{ "date_time_wait", 14 },
		{ "location_init", 15 },
		{ "location_high_accuracy_start", 16 },
		{ "location_periodic_running", 17 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
		if (!strcmp(stage, map[i].name)) {
			return map[i].blinks;
		}
	}

	return 1;
}

#define BOOT_LED_NODE DT_ALIAS(led0)
#if DT_NODE_HAS_STATUS(BOOT_LED_NODE, okay)
static const struct gpio_dt_spec boot_led = GPIO_DT_SPEC_GET(BOOT_LED_NODE, gpios);
static bool boot_led_ready;

#define BOOT_LED_ON_MS           160
#define BOOT_LED_BETWEEN_MS      220
#define BOOT_LED_GROUP_GAP_MS    1500
#define BOOT_LED_FIRST_GAP_MS    450
#define BOOT_LED_STAGECHG_GAP_MS 280

enum boot_led_phase {
	BOOT_LED_PHASE_GAP = 0,
	BOOT_LED_PHASE_ON,
	BOOT_LED_PHASE_OFF,
};

static uint8_t boot_led_pattern_blinks = 1;
static enum boot_led_phase boot_led_phase = BOOT_LED_PHASE_GAP;
static uint8_t boot_led_flash_idx;

static void boot_led_enter_gap(uint16_t gap_ms);

static void boot_led_notify_stage_changed(void)
{
	if (!boot_led_ready) {
		return;
	}

	boot_led_enter_gap(BOOT_LED_STAGECHG_GAP_MS);
}

static void boot_led_enter_gap(uint16_t gap_ms)
{
	boot_led_phase = BOOT_LED_PHASE_GAP;
	boot_led_flash_idx = 0;

	if (!boot_led_ready) {
		return;
	}

	(void)gpio_pin_set_dt(&boot_led, 0);
	k_work_reschedule(&boot_led_work, K_MSEC(gap_ms));
}

static void boot_led_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	uint8_t n = boot_led_pattern_blinks;

	if (n > 24U) {
		n = 24U;
	}

	if (!boot_led_ready) {
		return;
	}

	switch (boot_led_phase) {
	case BOOT_LED_PHASE_GAP:
		boot_led_flash_idx = 0;
		boot_led_phase = BOOT_LED_PHASE_ON;
		k_work_reschedule(&boot_led_work, K_NO_WAIT);
		break;

	case BOOT_LED_PHASE_ON:
		(void)gpio_pin_set_dt(&boot_led, 1);
		boot_led_phase = BOOT_LED_PHASE_OFF;
		k_work_reschedule(&boot_led_work, K_MSEC(BOOT_LED_ON_MS));
		break;

	case BOOT_LED_PHASE_OFF:
		(void)gpio_pin_set_dt(&boot_led, 0);
		boot_led_flash_idx++;

		if (boot_led_flash_idx < n) {
			boot_led_phase = BOOT_LED_PHASE_ON;
			k_work_reschedule(&boot_led_work, K_MSEC(BOOT_LED_BETWEEN_MS));
		} else {
			boot_led_enter_gap(BOOT_LED_GROUP_GAP_MS);
		}
		break;

	default:
		break;
	}
}
#else
static void boot_led_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
}
#endif

static void boot_mark(const char *stage)
{
	unsigned int led_n = boot_stage_lookup_led_blinks(stage);

	boot_stage = stage;

	printk("[boot] stage=%s led_pattern=%u blink%s uptime=%lld ms\n", boot_stage, led_n,
	       led_n == 1U ? "" : "s", k_uptime_get());

#if DT_NODE_HAS_STATUS(BOOT_LED_NODE, okay)
	boot_led_pattern_blinks = led_n > 24U ? 24U : (uint8_t)led_n;
	boot_led_notify_stage_changed();
#endif
}

static void log_modem_snapshot(const char *cmd, const char *tag)
{
	char rsp[192];
	int err = nrf_modem_at_cmd(rsp, sizeof(rsp), "%s", cmd);

	if (err) {
		printk("[gnss] %s query failed (%d)\n", tag, err);
		return;
	}

	printk("[gnss] %s: %s\n", tag, rsp);
}

static void log_modem_firmware_version(void)
{
	char rsp[192];
	char *ver;
	int err = nrf_modem_at_cmd(rsp, sizeof(rsp), "%s", "AT%SHORTSWVER");

	if (err) {
		printk("[dbg] modem firmware version (AT%%SHORTSWVER) failed: %d\n", err);
		return;
	}

	/* rsp is typically "%SHORTSWVER: nrf9160_x.y.z\r\nOK\r\n" — emit one short line. */
	ver = strstr(rsp, ": ");
	if (ver) {
		ver += 2;
		for (char *q = ver; *q; q++) {
			if (*q == '\r' || *q == '\n') {
				*q = '\0';
				break;
			}
		}
		printk("[dbg] modem firmware version: %s\n", ver);
	} else {
		printk("[dbg] modem firmware version (raw): %s\n", rsp);
	}
}

static void log_modem_hardware_version(void)
{
	char rsp[192];
	char *ver;
	int err = nrf_modem_at_cmd(rsp, sizeof(rsp), "%s", "AT%HWVERSION");

	if (err) {
		printk("[dbg] modem hardware version (AT%%HWVERSION) failed: %d\n", err);
		return;
	}

	ver = strstr(rsp, ": ");
	if (ver) {
		ver += 2;
		for (char *q = ver; *q; q++) {
			if (*q == '\r' || *q == '\n') {
				*q = '\0';
				break;
			}
		}
		printk("[dbg] modem hardware version: %s\n", ver);
	} else {
		printk("[dbg] modem hardware version (raw): %s\n", rsp);
	}
}

static void gnss_progress_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!gnss_debug_active) {
		return;
	}

	int64_t elapsed_s = (k_uptime_get() - gnss_debug_start_ms) / MSEC_PER_SEC;

	printk("[gnss] waiting for fix... elapsed=%lld s\n", elapsed_s);
	log_modem_snapshot("AT+CSCON?", "CSCON");
	log_modem_snapshot("AT%XMONITOR", "XMONITOR");

	k_work_reschedule(&gnss_progress_work, K_SECONDS(30));
}

static void boot_heartbeat_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	printk("[boot] heartbeat stage=%s uptime=%lld ms\n",
	       boot_stage, k_uptime_get());

	if (modem_ready_for_debug) {
		log_modem_snapshot("AT+CFUN?", "CFUN");
		log_modem_snapshot("AT+CEREG?", "CEREG");
		log_modem_snapshot("AT%XSYSTEMMODE?", "XSYSTEMMODE");
	}

	k_work_reschedule(&boot_heartbeat_work, K_SECONDS(15));
}

static void gnss_progress_start(const char *context)
{
	gnss_debug_active = true;
	gnss_debug_start_ms = k_uptime_get();
	printk("[gnss] progress debug started (%s)\n", context);
	k_work_reschedule(&gnss_progress_work, K_SECONDS(30));
}

static void gnss_progress_stop(const char *reason)
{
	if (!gnss_debug_active) {
		return;
	}

	gnss_debug_active = false;
	k_work_cancel_delayable(&gnss_progress_work);
	printk("[gnss] progress debug stopped (%s)\n", reason);
}

#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
static int wait_for_golioth_dns(void)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_DGRAM,
		.ai_protocol = IPPROTO_UDP,
	};
	struct zsock_addrinfo *res;
	int err;

	printk("[net] resolving coap.golioth.io:5684\n");
	printk("[dbg] wait_for_golioth_dns: start uptime=%lld ms\n", k_uptime_get());

	for (int i = 0; i < 24; i++) {
		printk("[dbg] DNS try %d/24 uptime=%lld ms\n", i + 1, k_uptime_get());
		err = zsock_getaddrinfo("coap.golioth.io", "5684", &hints, &res);
		if (!err) {
			int sock = zsock_socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);

			printk("[net] raw UDP socket probe: family=%d addrlen=%u sock=%d\n",
			       res->ai_family, (unsigned int)res->ai_addrlen, sock);
			if (sock >= 0) {
				err = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
				printk("[net] raw UDP connect probe returned %d errno=%d\n",
				       err, errno);
				(void)zsock_close(sock);
			}

			printk("[net] DNS resolution succeeded on attempt %d\n", i + 1);
			zsock_freeaddrinfo(res);
			return 0;
		}

		printk("[net] DNS attempt %d failed: %d\n", i + 1, err);
		k_sleep(K_SECONDS(5));
	}

	printk("[net] DNS resolution timed out, last error: %d\n", err);

	return err;
}

static int wait_for_network_ready(void)
{
	struct net_if *iface = net_if_get_default();
	uint64_t raised_event;
	int err;

	printk("[dbg] wait_for_network_ready: enter uptime=%lld ms\n", k_uptime_get());

	if (!iface) {
		printk("[net] no default network interface\n");
		return -ENODEV;
	}

	printk("[net] default iface=%p dormant=%d\n", iface, net_if_is_dormant(iface));

	err = net_if_up(iface);
	printk("[dbg] net_if_up(iface) -> %d uptime=%lld ms\n", err, k_uptime_get());
	if (err && err != -EALREADY) {
		printk("[net] net_if_up failed: %d\n", err);
		return err;
	}

	printk("[net] net_if_up returned: %d\n", err);

	if (net_if_is_dormant(iface)) {
		printk("Waiting for LTE network interface...\n");
		printk("[dbg] waiting L4 connected (up to 120s) uptime=%lld ms\n",
		       k_uptime_get());
		err = net_mgmt_event_wait_on_iface(iface, NET_EVENT_L4_CONNECTED,
						   &raised_event, NULL, NULL,
						   K_SECONDS(120));
		printk("[dbg] L4 wait done err=%d uptime=%lld ms\n", err, k_uptime_get());
		if (err) {
			printk("[net] wait for L4 connected failed: %d\n", err);
			return err;
		}

		printk("[net] L4 connected event: 0x%llx\n", raised_event);
	}

	printk("LTE network interface ready\n");

	/*
	 * Wait until the modem has assigned an IPv4 address before DNS and Golioth DTLS.
	 * Matches Golioth net_connect() pattern; avoids zsock_connect() failing during handshake.
	 */
	if (net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) == NULL) {
		printk("[net] waiting for IPv4 address (up to 120 s)...\n");
		err = net_mgmt_event_wait_on_iface(iface, NET_EVENT_IPV4_ADDR_ADD,
						   &raised_event, NULL, NULL,
						   K_SECONDS(120));
		printk("[dbg] IPv4 wait done err=%d uptime=%lld ms\n", err,
		       k_uptime_get());
		if (err) {
			printk("[net] wait for IPv4 address failed: %d\n", err);
			return err;
		}
		printk("[net] IPv4 assigned\n");
	} else {
		printk("[net] IPv4 already assigned\n");
	}

	printk("[dbg] wait_for_network_ready: calling DNS wait uptime=%lld ms\n",
	       k_uptime_get());

	return wait_for_golioth_dns();
}
#endif /* CONFIG_GOLIOTH_FIRMWARE_SDK */

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	printk("[time] date_time event: %d\n", evt->type);
	k_sem_give(&time_update_finished);
}

static void lte_event_handler(const struct lte_lc_evt *const evt)
{
	printk("[dbg] lte_event_handler: type=%d uptime=%lld ms\n", evt->type,
	       k_uptime_get());

	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		printk("[lte] network registration status: %d\n", evt->nw_reg_status);
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		     (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			printk("[dbg] LTE registered home/roam -> give lte_connected uptime=%lld ms\n",
			       k_uptime_get());
			printk("Connected to LTE\n");
			k_sem_give(&lte_connected);
		}
		break;
	default:
		printk("[lte] event type: %d\n", evt->type);
		break;
	}
}

static bool lte_is_registered(void)
{
	enum lte_lc_nw_reg_status status;
	int err;

	err = lte_lc_nw_reg_status_get(&status);
	if (err) {
		printk("[lte] status query failed: %d\n", err);
		return false;
	}

	printk("[lte] current registration status: %d\n", status);

	return (status == LTE_LC_NW_REG_REGISTERED_HOME) ||
	       (status == LTE_LC_NW_REG_REGISTERED_ROAMING);
}

static int enable_onboard_gnss_antenna_if_needed(void)
{
#if defined(CONFIG_GPS_SAMPLE_ANTENNA_ONBOARD)
	static const char coex0_cmd[] = "AT%XCOEX0=1,1,1565,1586";
	int err = nrf_modem_at_printf("%s", coex0_cmd);

	if (err) {
		printk("[gnss] failed to enable onboard antenna with '%s': %d\n",
		       coex0_cmd, err);
		return err;
	}

	printk("[gnss] onboard antenna enabled using '%s'\n",
	       coex0_cmd);
#endif

	return 0;
}

static void location_event_handler(const struct location_event_data *event_data)
{
	printk("[location] event id=%d method=%s\n",
	       event_data->id, location_method_str(event_data->method));

	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
		gnss_progress_stop("fix");
		printk("Got location:\n");
		printk("  method: %s\n", location_method_str(event_data->method));
		printk("  latitude: %.06f\n", event_data->location.latitude);
		printk("  longitude: %.06f\n", event_data->location.longitude);
		printk("  accuracy: %.01f m\n", (double)event_data->location.accuracy);
		if (event_data->location.datetime.valid) {
			printk("  date: %04d-%02d-%02d\n",
				event_data->location.datetime.year,
				event_data->location.datetime.month,
				event_data->location.datetime.day);
			printk("  time: %02d:%02d:%02d.%03d UTC\n",
				event_data->location.datetime.hour,
				event_data->location.datetime.minute,
				event_data->location.datetime.second,
				event_data->location.datetime.ms);
		}
		printk("  Google maps URL: https://maps.google.com/?q=%.06f,%.06f\n\n",
			event_data->location.latitude, event_data->location.longitude);
#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
		golioth_store_last_location(event_data);
		publish_status_to_golioth("location_fix", 0);
		publish_location_to_golioth(event_data);
#endif
		break;

	case LOCATION_EVT_TIMEOUT:
		gnss_progress_stop("timeout");
		printk("Getting location timed out\n\n");
#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
		publish_status_to_golioth("location_timeout", -ETIMEDOUT);
#endif
		break;

	case LOCATION_EVT_ERROR:
		gnss_progress_stop("error");
		printk("Getting location failed\n\n");
#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
		publish_status_to_golioth("location_error", -EIO);
#endif
		break;

	case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
		printk("Getting location assistance requested (A-GNSS). Not doing anything.\n\n");
#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
		publish_status_to_golioth("agnss_request", 0);
#endif
		break;

	case LOCATION_EVT_GNSS_PREDICTION_REQUEST:
		printk("Getting location assistance requested (P-GPS). Not doing anything.\n\n");
#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
		publish_status_to_golioth("pgps_request", 0);
#endif
		break;

	default:
		printk("Getting location: Unknown event\n\n");
		break;
	}

	k_sem_give(&location_event);
}

static void location_event_wait(void)
{
	k_sem_take(&location_event, K_FOREVER);
}

/**
 * @brief Retrieve location so that fallback is applied.
 *
 * @details This is achieved by setting GNSS as first priority method and giving it too short
 * timeout. Then a fallback to next method, which is cellular in this example, occurs.
 */
static void location_with_fallback_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {LOCATION_METHOD_GNSS, LOCATION_METHOD_CELLULAR};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);
	/* GNSS timeout is set to 1 second to force a failure. */
	config.methods[0].gnss.timeout = 1 * MSEC_PER_SEC;
	/* Default cellular configuration may be overridden here. */
	config.methods[1].cellular.timeout = 40 * MSEC_PER_SEC;

	printk("Requesting location with short GNSS timeout to trigger fallback to cellular...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	location_event_wait();
}

/**
 * @brief Retrieve location with default configuration.
 *
 * @details This is achieved by not passing configuration at all to location_request().
 */
static void location_default_get(void)
{
	int err;

	printk("Requesting location with the default configuration...\n");

	err = location_request(NULL);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	location_event_wait();
}

/**
 * @brief Retrieve location with GNSS low accuracy.
 */
static void location_gnss_low_accuracy_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {LOCATION_METHOD_GNSS};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);
	config.methods[0].gnss.accuracy = LOCATION_ACCURACY_LOW;

	printk("Requesting low accuracy GNSS location...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	location_event_wait();
}

/**
 * @brief Retrieve location with GNSS high accuracy.
 */
static void location_gnss_high_accuracy_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {LOCATION_METHOD_GNSS};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);
	config.methods[0].gnss.accuracy = LOCATION_ACCURACY_HIGH;
	config.methods[0].gnss.timeout = 8 * 60 * MSEC_PER_SEC;

	printk("Requesting high accuracy GNSS location...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	gnss_progress_start("high_accuracy");

	location_event_wait();
}

#if defined(CONFIG_LOCATION_METHOD_WIFI)
/**
 * @brief Retrieve location with Wi-Fi positioning as first priority, GNSS as second
 * and cellular as third.
 */
static void location_wifi_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {
		LOCATION_METHOD_WIFI,
		LOCATION_METHOD_GNSS,
		LOCATION_METHOD_CELLULAR};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);

	printk("Requesting Wi-Fi location with GNSS and cellular fallback...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	location_event_wait();
}
#endif

/**
 * @brief Retrieve location periodically using GNSS only.
 */
static void location_gnss_periodic_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {LOCATION_METHOD_GNSS};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);
	config.interval = 30;
	config.methods[0].gnss.timeout = 4 * 60 * MSEC_PER_SEC;

	printk("Requesting 30s periodic GNSS location...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	gnss_progress_start("periodic");
}

int main(void)
{
	int err;

	k_work_init_delayable(&gnss_progress_work, gnss_progress_work_handler);
	k_work_init_delayable(&boot_heartbeat_work, boot_heartbeat_work_handler);
	k_work_init_delayable(&boot_led_work, boot_led_work_handler);

#if (IS_ENABLED(CONFIG_SHT4X) && DT_NODE_HAS_STATUS(DT_ALIAS(sht40), okay)) || \
	(IS_ENABLED(CONFIG_LIS2DH) && DT_NODE_HAS_STATUS(DT_ALIAS(accel0), okay))
	k_work_init_delayable(&sensor_poll_work, sensor_poll_work_handler);
	k_work_reschedule(&sensor_poll_work, K_SECONDS(10));
	printk("[sensor] printing SHT40 + LIS2DH every 10 s\n");
#endif

#if DT_NODE_HAS_STATUS(BOOT_LED_NODE, okay)
	if (gpio_is_ready_dt(&boot_led)) {
		if (!gpio_pin_configure_dt(&boot_led, GPIO_OUTPUT_INACTIVE)) {
			boot_led_ready = true;
			boot_led_enter_gap(BOOT_LED_FIRST_GAP_MS);
			printk("[boot] LED stage blink enabled on led0 (first gap then pattern)\n");
		} else {
			printk("[boot] failed to configure led0 for debug blink\n");
		}
	} else {
		printk("[boot] led0 GPIO not ready for debug blink\n");
	}
#else
	printk("[boot] no led0 alias available for debug blink\n");
#endif

	k_work_reschedule(&boot_heartbeat_work, K_SECONDS(5));
	boot_mark("main_entry");

	printk("Location sample started\n\n");

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		boot_mark("date_time_handler_register");
		/* Registering early for date_time event handler to avoid missing
		 * the first event after LTE is connected.
		 */
		printk("[time] registering date_time handler\n");
		date_time_register_handler(date_time_evt_handler);
	}

	printk("Connecting to LTE...\n");

	if (!IS_ENABLED(CONFIG_NRF_MODEM_LIB_NET_IF_AUTO_START)) {
		boot_mark("nrf_modem_lib_init_begin");
		printk("[lte] manual modem init path\n");
		err = nrf_modem_lib_init();
		if (err) {
			printk("Modem library initialization failed, error: %d\n", err);
			return err;
		}
		modem_ready_for_debug = true;
		boot_mark("nrf_modem_lib_init_done");
		printk("[lte] modem library initialized\n");
	}
	modem_ready_for_debug = true;
	log_modem_firmware_version();
	log_modem_hardware_version();
	log_modem_snapshot("AT+CFUN?", "CFUN");
	log_modem_snapshot("AT%XMODEMUUID", "MODEMUUID");

#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
	/* Modem sec tag PSK via AT%%CMNG before LTE (matches master / stratus9161 path). */
	boot_mark("golioth_psk_provision");
	err = golioth_provision_psk_credentials();
	if (err) {
		return err;
	}
#endif

	boot_mark("lte_handler_register");
	printk("[lte] registering LTE event handler\n");
	lte_lc_register_handler(lte_event_handler);

	if (!lte_is_registered()) {
		boot_mark("lte_connect_request");
		printk("[lte] requesting LTE connection\n");
		printk("[dbg] lte_lc_connect() call uptime=%lld ms\n", k_uptime_get());
		err = lte_lc_connect();
		printk("[dbg] lte_lc_connect() returned %d uptime=%lld ms\n", err,
		       k_uptime_get());
		if (err) {
			printk("LTE connection request failed, error: %d\n", err);
			return err;
		}
	} else {
		printk("[dbg] skip lte_lc_connect: already registered uptime=%lld ms\n",
		       k_uptime_get());
	}

	if (!lte_is_registered()) {
		boot_mark("lte_wait_registered");
		printk("[lte] waiting for LTE registration semaphore\n");
		printk("[dbg] k_sem_take(&lte_connected) blocking uptime=%lld ms\n",
		       k_uptime_get());
		k_sem_take(&lte_connected, K_FOREVER);
		printk("[dbg] k_sem_take(&lte_connected) done uptime=%lld ms\n",
		       k_uptime_get());
		printk("[lte] LTE registration semaphore received\n");
	} else {
		printk("[dbg] skip lte wait: already registered uptime=%lld ms\n",
		       k_uptime_get());
	}
	boot_mark("lte_registered");

	boot_mark("gnss_antenna_enable");
	err = enable_onboard_gnss_antenna_if_needed();
	if (err) {
		return err;
	}

#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
	boot_mark("network_wait_ready");
	printk("[net] waiting for usable network\n");
	err = wait_for_network_ready();
	if (err) {
		printk("Network did not become ready, error: %d\n", err);
		return err;
	}

	boot_mark("golioth_client_create");
	printk("[golioth] creating client after LTE/network readiness\n");
	golioth_client = golioth_client_create(&golioth_cfg);
	if (!golioth_client) {
		printk("[golioth] client create returned NULL\n");
		return -ENOMEM;
	}
	printk("[golioth] client created: %p\n", golioth_client);

	boot_mark("golioth_wait_connected");
	printk("Connecting to Golioth...\n");
	printk("[golioth] using sec tag %d\n",
	       CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG);

	golioth_client_register_event_callback(golioth_client, golioth_on_client_event, NULL);
	printk("[golioth] waiting up to 60 seconds for connection\n");
	printk("[dbg] k_sem_take(golioth_connected) start uptime=%lld ms\n",
	       k_uptime_get());

	int golioth_ret = k_sem_take(&golioth_connected_sem, K_SECONDS(60));
	printk("[dbg] k_sem_take(golioth_connected) ret=%d uptime=%lld ms\n", golioth_ret,
	       k_uptime_get());
	printk("[golioth] connection wait returned: %d\n", golioth_ret);

	if (golioth_ret == -EAGAIN) {
		printk("Timed out waiting for Golioth connection, continuing anyway\n");
	} else {
		publish_status_to_golioth("golioth_connected", 0);
	}
#endif /* CONFIG_GOLIOTH_FIRMWARE_SDK */

	/* A-GNSS/P-GPS needs to know the current time. */
	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		boot_mark("date_time_wait");
		printk("Waiting for current time\n");
		printk("[dbg] k_sem_take(time_update_finished) start uptime=%lld ms\n",
		       k_uptime_get());

		/* Wait for an event from the Date Time library. */
		int time_ret = k_sem_take(&time_update_finished, K_MINUTES(10));
		printk("[dbg] k_sem_take(time_update_finished) ret=%d uptime=%lld ms\n",
		       time_ret, k_uptime_get());
		printk("[time] wait returned: %d\n", time_ret);

		if (!date_time_is_valid()) {
			printk("Failed to get current time. Continuing anyway.\n");
		} else {
			printk("[time] current time is valid\n");
		}
	}

	boot_mark("location_init");
	printk("[location] initializing Location library\n");
	err = location_init(location_event_handler);
	if (err) {
		printk("Initializing the Location library failed, error: %d\n", err);
		return -1;
	}
	printk("[location] Location library initialized\n");

	boot_mark("location_high_accuracy_start");
	location_gnss_high_accuracy_get();

#if defined(CONFIG_LOCATION_METHOD_WIFI)
	location_wifi_get();
#endif

	location_gnss_periodic_get();
	boot_mark("location_periodic_running");

	return 0;
}
