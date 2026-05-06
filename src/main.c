/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <zephyr/kernel.h>
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
#include <zephyr/net/socket.h>
#include <zcbor_encode.h>

#define GOLIOTH_PSK_ID CONFIG_GOLIOTH_SAMPLE_PSK_ID
#define GOLIOTH_PSK    CONFIG_GOLIOTH_SAMPLE_PSK

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
	char psk_hex[(sizeof(GOLIOTH_PSK) - 1) * 2 + 1];
	const char hex_digits[] = "0123456789abcdef";
	const char *psk = GOLIOTH_PSK;
	int err;

	for (size_t i = 0; i < sizeof(GOLIOTH_PSK) - 1; i++) {
		unsigned char value = (unsigned char)psk[i];

		psk_hex[i * 2] = hex_digits[value >> 4];
		psk_hex[i * 2 + 1] = hex_digits[value & 0x0f];
	}
	psk_hex[sizeof(psk_hex) - 1] = '\0';

	/* Ignore delete failures when the credentials do not exist yet. */
	(void)nrf_modem_at_printf("AT%%CMNG=3,%d,4",
				 CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG);
	(void)nrf_modem_at_printf("AT%%CMNG=3,%d,3",
				 CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG);

	err = nrf_modem_at_printf("AT%%CMNG=0,%d,4,\"%s\"",
				  CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG,
				  GOLIOTH_PSK_ID);
	if (err) {
		printk("[golioth] failed to provision PSK ID to sec tag %d: %d\n",
		       CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG, err);
		return err;
	}

	err = nrf_modem_at_printf("AT%%CMNG=0,%d,3,\"%s\"",
				  CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG,
				  psk_hex);
	if (err) {
		printk("[golioth] failed to provision PSK to sec tag %d: %d\n",
		       CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG, err);
		return err;
	}

	printk("[golioth] provisioned PSK credentials to sec tag %d\n",
	       CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG);

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
#endif /* CONFIG_GOLIOTH_FIRMWARE_SDK */

static K_SEM_DEFINE(location_event, 0, 1);

static K_SEM_DEFINE(lte_connected, 0, 1);

static K_SEM_DEFINE(time_update_finished, 0, 1);

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

	for (int i = 0; i < 24; i++) {
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

	if (!iface) {
		printk("[net] no default network interface\n");
		return -ENODEV;
	}

	printk("[net] default iface=%p dormant=%d\n", iface, net_if_is_dormant(iface));

	err = net_if_up(iface);
	if (err && err != -EALREADY) {
		printk("[net] net_if_up failed: %d\n", err);
		return err;
	}

	printk("[net] net_if_up returned: %d\n", err);

	if (net_if_is_dormant(iface)) {
		printk("Waiting for LTE network interface...\n");
		err = net_mgmt_event_wait_on_iface(iface, NET_EVENT_L4_CONNECTED,
						   &raised_event, NULL, NULL,
						   K_SECONDS(120));
		if (err) {
			printk("[net] wait for L4 connected failed: %d\n", err);
			return err;
		}

		printk("[net] L4 connected event: 0x%llx\n", raised_event);
	}

	printk("LTE network interface ready\n");

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
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		printk("[lte] network registration status: %d\n", evt->nw_reg_status);
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		     (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
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

static void location_event_handler(const struct location_event_data *event_data)
{
	printk("[location] event id=%d method=%s\n",
	       event_data->id, location_method_str(event_data->method));

	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
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
		publish_status_to_golioth("location_fix", 0);
		publish_location_to_golioth(event_data);
#endif
		break;

	case LOCATION_EVT_TIMEOUT:
		printk("Getting location timed out\n\n");
#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
		publish_status_to_golioth("location_timeout", -ETIMEDOUT);
#endif
		break;

	case LOCATION_EVT_ERROR:
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
}

int main(void)
{
	int err;

	printk("Location sample started\n\n");

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		/* Registering early for date_time event handler to avoid missing
		 * the first event after LTE is connected.
		 */
		printk("[time] registering date_time handler\n");
		date_time_register_handler(date_time_evt_handler);
	}

	printk("Connecting to LTE...\n");

	if (!IS_ENABLED(CONFIG_NRF_MODEM_LIB_NET_IF_AUTO_START)) {
		printk("[lte] manual modem init path\n");
		err = nrf_modem_lib_init();
		if (err) {
			printk("Modem library initialization failed, error: %d\n", err);
			return err;
		}
		printk("[lte] modem library initialized\n");
	}

	printk("[lte] registering LTE event handler\n");
	lte_lc_register_handler(lte_event_handler);

#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
	/*
	 * nRF91 offloaded DTLS uses modem sec tags. Provision the PSK credentials
	 * before LTE activation so the secure tag is ready for the DTLS session.
	 */
	err = golioth_provision_psk_credentials();
	if (err) {
		return err;
	}
#endif

	if (!lte_is_registered()) {
		printk("[lte] requesting LTE connection\n");
		err = lte_lc_connect();
		if (err) {
			printk("LTE connection request failed, error: %d\n", err);
			return err;
		}
	}

	if (!lte_is_registered()) {
		printk("[lte] waiting for LTE registration semaphore\n");
		k_sem_take(&lte_connected, K_FOREVER);
		printk("[lte] LTE registration semaphore received\n");
	}

#if defined(CONFIG_GOLIOTH_FIRMWARE_SDK)
	printk("[net] waiting for usable network\n");
	err = wait_for_network_ready();
	if (err) {
		printk("Network did not become ready, error: %d\n", err);
		return err;
	}

	printk("[golioth] creating client after LTE/network readiness\n");
	golioth_client = golioth_client_create(&golioth_cfg);
	if (!golioth_client) {
		printk("[golioth] client create returned NULL\n");
		return -ENOMEM;
	}
	printk("[golioth] client created: %p\n", golioth_client);

	printk("Connecting to Golioth...\n");
	printk("[golioth] using sec tag %d\n",
	       CONFIG_GOLIOTH_COAP_CLIENT_CREDENTIALS_TAG);

	golioth_client_register_event_callback(golioth_client, golioth_on_client_event, NULL);
	printk("[golioth] waiting up to 60 seconds for connection\n");

	int golioth_ret = k_sem_take(&golioth_connected_sem, K_SECONDS(60));
	printk("[golioth] connection wait returned: %d\n", golioth_ret);

	if (golioth_ret == -EAGAIN) {
		printk("Timed out waiting for Golioth connection, continuing anyway\n");
	} else {
		publish_status_to_golioth("golioth_connected", 0);
	}
#endif /* CONFIG_GOLIOTH_FIRMWARE_SDK */

	/* A-GNSS/P-GPS needs to know the current time. */
	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		printk("Waiting for current time\n");

		/* Wait for an event from the Date Time library. */
		int time_ret = k_sem_take(&time_update_finished, K_MINUTES(10));
		printk("[time] wait returned: %d\n", time_ret);

		if (!date_time_is_valid()) {
			printk("Failed to get current time. Continuing anyway.\n");
		} else {
			printk("[time] current time is valid\n");
		}
	}

	printk("[location] initializing Location library\n");
	err = location_init(location_event_handler);
	if (err) {
		printk("Initializing the Location library failed, error: %d\n", err);
		return -1;
	}
	printk("[location] Location library initialized\n");

	location_gnss_high_accuracy_get();

#if defined(CONFIG_LOCATION_METHOD_WIFI)
	location_wifi_get();
#endif

	location_gnss_periodic_get();

	return 0;
}
