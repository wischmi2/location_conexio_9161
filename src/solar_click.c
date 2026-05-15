/*
 * Solar Energy Click GPIO control and VBAT_OK monitor.
 */

#include "solar_click.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#if DT_HAS_ALIAS(solar_vout_en)
#define SOLAR_VOUT_EN_NODE DT_ALIAS(solar_vout_en)
static const struct gpio_dt_spec solar_vout_en = GPIO_DT_SPEC_GET(SOLAR_VOUT_EN_NODE, gpios);
static bool solar_vout_en_ready;
#endif

#if DT_HAS_ALIAS(solar_en)
#define SOLAR_EN_NODE DT_ALIAS(solar_en)
static const struct gpio_dt_spec solar_en = GPIO_DT_SPEC_GET(SOLAR_EN_NODE, gpios);
static bool solar_en_ready;
#endif

#if DT_HAS_ALIAS(solar_int)
#define SOLAR_INT_NODE DT_ALIAS(solar_int)
static const struct gpio_dt_spec solar_int = GPIO_DT_SPEC_GET(SOLAR_INT_NODE, gpios);
static struct gpio_callback solar_int_cb;
static struct k_work_delayable solar_int_work;
static int solar_int_last_level = -1;
#endif

static solar_click_vbat_ok_handler_t vbat_ok_handler;
static void *vbat_ok_user_data;

int solar_click_register_vbat_ok_handler(solar_click_vbat_ok_handler_t handler, void *user_data)
{
	vbat_ok_handler = handler;
	vbat_ok_user_data = user_data;
	return 0;
}

int solar_click_set_vout_enabled(bool enable)
{
#if DT_HAS_ALIAS(solar_vout_en)
	if (!solar_vout_en_ready) {
		return -ENODEV;
	}

	return gpio_pin_set_dt(&solar_vout_en, enable ? 1 : 0);
#else
	ARG_UNUSED(enable);
	return -ENOTSUP;
#endif
}

int solar_click_set_en(bool enable)
{
#if DT_HAS_ALIAS(solar_en)
	if (!solar_en_ready) {
		return -ENODEV;
	}

	return gpio_pin_set_dt(&solar_en, enable ? 1 : 0);
#else
	ARG_UNUSED(enable);
	return -ENOTSUP;
#endif
}

int solar_click_get_vbat_ok_level(int *level)
{
#if DT_HAS_ALIAS(solar_int)
	if (level == NULL) {
		return -EINVAL;
	}

	*level = gpio_pin_get_dt(&solar_int);
	return *level < 0 ? *level : 0;
#else
	ARG_UNUSED(level);
	return -ENOTSUP;
#endif
}

static int solar_vout_en_init(void)
{
#if DT_HAS_ALIAS(solar_vout_en)
	int err;

	if (!gpio_is_ready_dt(&solar_vout_en)) {
		printk("[solar] VOUT_EN gpio not ready\n");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&solar_vout_en, GPIO_OUTPUT_INACTIVE);
	if (err) {
		printk("[solar] VOUT_EN pin config failed: %d\n", err);
		return err;
	}

	solar_vout_en_ready = true;
	printk("[solar] VOUT_EN control enabled on P0.%u (default=0)\n", solar_vout_en.pin);
	return 0;
#else
	printk("[solar] no solar_vout_en alias in devicetree; VOUT_EN control disabled\n");
	return 0;
#endif
}

static int solar_en_init(void)
{
#if DT_HAS_ALIAS(solar_en)
	int err;

	if (!gpio_is_ready_dt(&solar_en)) {
		printk("[solar] EN gpio not ready\n");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&solar_en, GPIO_OUTPUT_INACTIVE);
	if (err) {
		printk("[solar] EN pin config failed: %d\n", err);
		return err;
	}

	solar_en_ready = true;
	printk("[solar] EN control enabled on P0.%u (default=0)\n", solar_en.pin);
	return 0;
#else
	printk("[solar] no solar_en alias in devicetree; EN control disabled\n");
	return 0;
#endif
}

#if DT_HAS_ALIAS(solar_int)
static void solar_int_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int level = gpio_pin_get_dt(&solar_int);

	if (level < 0) {
		printk("[solar] failed to read INT pin: %d\n", level);
		return;
	}

	if (solar_int_last_level != level) {
		solar_int_last_level = level;
		printk("[solar] INT level=%d -> %s\n", level,
		       level == 0 ? "low-battery" : "battery-recovered");
		if (vbat_ok_handler != NULL) {
			vbat_ok_handler(level == 0, level, vbat_ok_user_data);
		}
	}
}

static void solar_int_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Debounce/suppress chatter from threshold transitions. */
	k_work_reschedule(&solar_int_work, K_MSEC(25));
}
#endif

static int solar_int_init(void)
{
#if DT_HAS_ALIAS(solar_int)
	int err;

	k_work_init_delayable(&solar_int_work, solar_int_work_handler);

	if (!gpio_is_ready_dt(&solar_int)) {
		printk("[solar] INT gpio not ready\n");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&solar_int, GPIO_INPUT);
	if (err) {
		printk("[solar] INT pin config failed: %d\n", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&solar_int, GPIO_INT_EDGE_BOTH);
	if (err) {
		printk("[solar] INT interrupt config failed: %d\n", err);
		return err;
	}

	gpio_init_callback(&solar_int_cb, solar_int_callback, BIT(solar_int.pin));
	err = gpio_add_callback(solar_int.port, &solar_int_cb);
	if (err) {
		printk("[solar] INT callback add failed: %d\n", err);
		return err;
	}

	k_work_reschedule(&solar_int_work, K_NO_WAIT);
	printk("[solar] INT monitoring enabled on P0.%u\n", solar_int.pin);

	return 0;
#else
	printk("[solar] no solar_int alias in devicetree; INT monitoring disabled\n");
	return 0;
#endif
}

int solar_click_init(void)
{
	int err;
	int first_err = 0;

	err = solar_en_init();
	if (err && first_err == 0) {
		first_err = err;
	}

	err = solar_vout_en_init();
	if (err && first_err == 0) {
		first_err = err;
	}

	/* Safe defaults while energy state is unknown. */
	(void)solar_click_set_en(false);
	(void)solar_click_set_vout_enabled(false);

	err = solar_int_init();
	if (err && first_err == 0) {
		first_err = err;
	}

	if (first_err == 0) {
		printk("[solar] initialization complete (EN=0, VOUT_EN=0)\n");
	}

	return first_err;
}
