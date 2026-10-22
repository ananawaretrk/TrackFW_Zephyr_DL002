/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>                 /* INT16_MIN */
#include <math.h>                   /* lroundf() */
#include <zephyr/kernel.h>          /* k_msleep() */
#include <zephyr/drivers/gpio.h>    /* GPIO / LED functions */
#include <zephyr/sys/byteorder.h>   /* sys_put_be16() */
#include <zephyr/bluetooth/bluetooth.h>
#include "hct6911.h"                /* temperature ADC (PT100 front end) */
#include "pt100.h"                  /* code -> ohm -> °C */

/* LED pin info from the devicetree (dl002.dts) */
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

/* Button pin info from the devicetree (alias "sw0" -> PB01, active low, pull-up) */
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* Holds our callback registration (Zephyr needs this to remember who to call) */
static struct gpio_callback button_cb;

static volatile int press_count;

/* Manufacturer data, 10 bytes, as seen in nRF Connect:
 * FF FF             company 0xFFFF (test only, not shippable)
 * D4 4A 85 BF 3E 17 MAC, same order as the boot log
 * 09 24             temperature, int16 big-endian, 0.01 °C (0x0924 = 2340 = 23.40 °C)
 */
#define MFG_MAC_OFFSET   2
#define MFG_TEMP_OFFSET  8
#define TEMP_NO_READING  INT16_MIN   /* 0x8000: no valid reading */

/* Each cycle: read the temperature, start advertising (an alarm stops it after
 * ADV_DURATION_MS), sleep ADV_CYCLE_MS
 */
#define ADV_INTERVAL_MS  20     /* time between packets while advertising (BLE min 20 ms) */
#define ADV_DURATION_MS  100    /* how long we advertise each cycle */
#define ADV_CYCLE_MS     5000   /* start of one cycle to the start of the next (+ reading time) */

BUILD_ASSERT(ADV_DURATION_MS < ADV_CYCLE_MS, "advertising must be shorter than the cycle");

static uint8_t mfg_data[10] = { 0xFF, 0xFF, [MFG_TEMP_OFFSET] = 0x80 };   /* temp starts as 0x8000 */

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

/* Runs ADV_DURATION_MS after advertising starts (same idea as AL300's stop_adv_wk): radio off */
static void stop_adv_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	bt_le_adv_stop();
}

static K_WORK_DELAYABLE_DEFINE(stop_adv_work, stop_adv_handler);

/* Wake the HCT6911, turn the excitation current back on (setup_pt100), take one reading and put
 * it back to sleep. Returns the temperature in 0.01 °C, or TEMP_NO_READING on any failure
 * (the function that failed has already printed why).
 */
static int16_t read_temperature(void)
{
	int32_t pt100_code;
	bool reading_ok = (hct6911_wake() == 0) &&
			  (hct6911_setup_pt100() == 0) &&
			  (hct6911_read_raw(&pt100_code) == 0);

	/* Back to sleep (< 1 uA) even if the reading failed */
	hct6911_sleep();

	if (!reading_ok) {
		return TEMP_NO_READING;
	}

	float r_ohm = pt100_code_to_ohm(pt100_code);
	float temp_c = pt100_ohm_to_celsius(r_ohm);

	if (temp_c < PT100_MIN_CELSIUS || temp_c > PT100_MAX_CELSIUS) {
		printf("PT100 out of range: R = %.3f ohm (probe unplugged or shorted?)\n", (double)r_ohm);
		return TEMP_NO_READING;
	}

	/* printf takes doubles: (double) makes the float -> double step explicit */
	printf("PT100 R = %.3f ohm, T = %.2f C\n", (double)r_ohm, (double)temp_c);

	/* 23.40 °C -> 2340 (rounded, so 23.3999 doesn't become 2339) */
	return (int16_t)lroundf(temp_c * 100.0f);
}

int main(void)
{
	if (!gpio_is_ready_dt(&led_green) || !gpio_is_ready_dt(&led_red)) {
		printf("LED GPIO not ready\n");
		return 0;
	}

	/* Both pins are outputs, both LEDs off (each one lit costs mA) */
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

	/* Temperature ADC (powered from boot, see adc_3v3 in dl002.dts): init, reset, configure
	 * for the PT100, then sleep until the first reading.
	 * Stops at the first failing step; that function prints why.
	 */
	bool adc_ready = (hct6911_init() == 0) &&
			 (hct6911_reset() == 0) &&
			 (hct6911_setup_pt100() == 0) &&
			 (hct6911_sleep() == 0);

	if (!adc_ready) {
		printf("HCT6911 not ready: no temperature readings\n");
	}

	/* Start the Bluetooth stack (radio library, controller) */
	int err = bt_enable(NULL);
	if (err) {
		printf("Bluetooth init failed (err %d)\n", err);
		return 0;
	}
	printf("Bluetooth initialized\n");

	/* Our own (identity) address, e.g. D4:4A:85:BF:3E:17 (public) */
	bt_addr_le_t identity_address;
	size_t identity_count = 1;
	char address_text[BT_ADDR_LE_STR_LEN];

	bt_id_get(&identity_address, &identity_count);
	bt_addr_le_to_str(&identity_address, address_text, sizeof(address_text));
	printf("Bluetooth address %s\n", address_text);

	/* Copy the MAC into the advertisement. Zephyr stores it reversed, so val[5] goes first
	 * to make it read the same as the boot log.
	 */
	for (int byte_index = 0; byte_index < 6; byte_index++) {
		mfg_data[MFG_MAC_OFFSET + byte_index] = identity_address.a.val[5 - byte_index];
	}

	printf("Advertising as %s: %d ms (packet every %d ms) every %d ms\n", CONFIG_BT_DEVICE_NAME,
	       ADV_DURATION_MS, ADV_INTERVAL_MS, ADV_CYCLE_MS);

	while (1) {
		/* Stays 0x8000 unless we get a valid reading below */
		int16_t temp_centi_celsius = TEMP_NO_READING;

		if (adc_ready) {
			temp_centi_celsius = read_temperature();
		}

		/* Put the temperature into the advertisement (big-endian: 2340 -> 09 24) */
		sys_put_be16((uint16_t)temp_centi_celsius, &mfg_data[MFG_TEMP_OFFSET]);

		/* Start advertising: non-connectable, fixed identity address, one packet every ADV_INTERVAL_MS */
		err = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_USE_IDENTITY,
						      BT_GAP_MS_TO_ADV_INTERVAL(ADV_INTERVAL_MS),
						      BT_GAP_MS_TO_ADV_INTERVAL(ADV_INTERVAL_MS), NULL),
				      ad, ARRAY_SIZE(ad), NULL, 0);
		if (err) {
			printf("Advertising failed to start (err %d)\n", err);
		} else {
			/* Alarm: stop advertising after ADV_DURATION_MS */
			k_work_schedule(&stop_adv_work, K_MSEC(ADV_DURATION_MS));
		}

		/* The alarm stops advertising during this sleep; CONFIG_PM puts the chip in EM2 */
		k_msleep(ADV_CYCLE_MS);
	}

	return 0;
}
