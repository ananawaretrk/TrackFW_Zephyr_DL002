#include <stdio.h>
#include <stdint.h>
#include <zephyr/kernel.h>          /* k_msleep() */
#include <zephyr/drivers/gpio.h>    /* GPIO / LED functions */
#include "app_config.h"             /* DL002_ADV_* settings */
#include "temperature.h"            /* temperature_init(), temperature_read() */
#include "ble.h"                    /* ble_init(), ble_set_temperature(), ble_adv_start() */

/* LED pin info from the devicetree (dl002.dts) */
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

/* Button pin info from the devicetree (alias "sw0" -> PB01, active low, pull-up) */
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* Holds our callback registration (Zephyr needs this to remember who to call) */
static struct gpio_callback button_cb;

static volatile int press_count;

/* Called by Zephyr the instant the button is pressed (runs in interrupt context) */
static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	press_count++;
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

	/* On failure it prints why, and every temperature_read() gives TEMP_NO_READING */
	temperature_init();

	/* On failure it prints why; without Bluetooth there is nothing to do */
	if (ble_init() != 0) {
		return 0;
	}

	printf("Advertising %d ms (packet every %d ms) every %d ms\n",
	       DL002_ADV_DURATION_MS, DL002_ADV_INTERVAL_MS, DL002_ADV_CYCLE_MS);

	while (1) {
		/* 0.01 °C, or TEMP_NO_READING (0x8000) if there is no valid reading */
		int16_t temp_centi_celsius = temperature_read();

		ble_set_temperature(temp_centi_celsius);

		/* On failure it prints why; we try again next cycle */
		ble_adv_start();

		/* The alarm stops advertising during this sleep; CONFIG_PM puts the chip in EM2 */
		k_msleep(DL002_ADV_CYCLE_MS);
	}

	return 0;
}
