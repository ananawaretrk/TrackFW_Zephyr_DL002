/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <zephyr/kernel.h>          /* k_msleep() */
#include <zephyr/drivers/gpio.h>    /* GPIO / LED functions */
#include <zephyr/bluetooth/bluetooth.h>
#include "hct6911.h"                /* temperature ADC (PT100 front end) */

/* LED pin info from the devicetree (dl002.dts) */
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

/* Button pin info from the devicetree (alias "sw0" -> PB01, active low, pull-up) */
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* Holds our callback registration (Zephyr needs this to remember who to call) */
static struct gpio_callback button_cb;

static volatile int press_count;

/* Manufacturer data: company 0xFFFF (test only, not shippable) + 1 byte press count */
static uint8_t mfg_data[] = { 0xFF, 0xFF, 0x00 };

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

/* Called by Zephyr the instant the button is pressed (runs in interrupt context) */
static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	press_count++;
	printk("BUTTON pressed! (%d)\n", press_count);
}

int main(void)
{
	int count = 0;
	bool green_on = false;

	if (!gpio_is_ready_dt(&led_green) || !gpio_is_ready_dt(&led_red)) {
		printf("LED GPIO not ready\n");
		return 0;
	}

	/* Both pins are outputs, both LEDs start off */
	gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);

	if (!gpio_is_ready_dt(&button)) {
		printf("Button GPIO not ready\n");
		return 0;
	}

	/* Make the pin an input (pull-up comes from the DTS flags) */
	gpio_pin_configure_dt(&button, GPIO_INPUT);

	/* Interrupt when the button becomes "active" = pressed */
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);

	/* Tell Zephyr: on an interrupt for this pin, call button_pressed() */
	gpio_init_callback(&button_cb, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);

	printf("Hello World! %s\n", CONFIG_BOARD_TARGET);

	/* Step 3: can we talk to the temperature ADC? */
	//hct6911_selftest();

	/* Step 4: init, reset, then configure for the PT100 */
	if (hct6911_init() != 0) {
		printf("HCT6911 init FAILED\n");
	}
	if (hct6911_reset() != 0) {
		printf("HCT6911 reset FAILED\n");
	}
	if (hct6911_setup_pt100() != 0) {
		printf("HCT6911 PT100 setup FAILED\n");
	}

	/* Start the Bluetooth stack (radio library, controller) */
	int err = bt_enable(NULL);
	if (err) {
		printf("Bluetooth init failed (err %d)\n", err);
		return 0;
	}
	printf("Bluetooth initialized\n");

	/* Advertise forever: non-connectable, fixed (identity) address, every 100-150 ms */
	err = bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		printf("Advertising failed to start (err %d)\n", err);
		return 0;
	}
	printf("Advertising as %s\n", CONFIG_BT_DEVICE_NAME);

	while (1) {
		green_on = !green_on;
		gpio_pin_set_dt(&led_green, green_on);   /* green on...  */
		gpio_pin_set_dt(&led_red, !green_on);    /* ...red is the opposite */

		printf("Hello %d, %s\n", count, green_on ? "GREEN" : "RED");
		count++;

		/* Put the latest button count into the advertisement (not allowed from the interrupt) */
		mfg_data[2] = (uint8_t)press_count;
		bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);

		/* Step 4: one PT100 measurement */
		int32_t pt100_code;
		int pt100_err = hct6911_read_raw(&pt100_code);

		if (pt100_err) {
			printf("PT100 read error %d\n", pt100_err);
		} else {
			/* R = code / gain / 2^23 * Rref, in milliohms (schematic formula) */
			int64_t r_milliohm = (int64_t)pt100_code * 1620000 / (8LL * 8388608);

			printf("PT100 code = %d, R = %d.%03d ohm\n", pt100_code,
			       (int)(r_milliohm / 1000), (int)(r_milliohm % 1000));
		}

		k_msleep(500);
	}

	return 0;
}
