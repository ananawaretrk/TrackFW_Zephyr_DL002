#include <zephyr/logging/log.h>
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>          /* k_msleep() */
#include <zephyr/drivers/gpio.h>    /* GPIO / LED functions */
#include "app_config.h"             /* DL002_ADV_* settings */
#include "temperature.h"            /* temperature_init(), temperature_read() */
#include "ble.h"                    /* ble_init(), ble_set_temperature(), ble_adv_start() */
#include "battery.h"                /* battery_init(), battery_read_mv() */
#include <zephyr/dfu/mcuboot.h>     /* boot_is_img_confirmed(), boot_write_img_confirmed() */
#include <app_version.h>            /* APP_VERSION_STRING, from the VERSION file */

LOG_MODULE_REGISTER(main);

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

/* After a firmware update the new firmware runs "on trial": if the device resets before this
 * is called, MCUboot puts the old firmware back. Does nothing on normal boots.
 */
static void confirm_firmware(void)
{
	if (boot_is_img_confirmed()) {
		return;
	}

	int err = boot_write_img_confirmed();

	if (err) {
		LOG_ERR("Firmware confirm FAILED (err %d)", err);
		return;
	}
	LOG_INF("New firmware v%s confirmed", APP_VERSION_STRING);
}

int main(void)
{
	LOG_INF("DL002 firmware v%s", APP_VERSION_STRING);

	if (!gpio_is_ready_dt(&led_green) || !gpio_is_ready_dt(&led_red)) {
		LOG_ERR("LED GPIO not ready");
		return 0;
	}

	/* Both pins are outputs, both LEDs off (each one lit costs mA) */
	int err = gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);

	if (err) {
		LOG_ERR("Green LED pin configure FAILED (err %d)", err);
	}

	err = gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Red LED pin configure FAILED (err %d)", err);
	}

	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("Button GPIO not ready");
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

	/* On failure it prints why, and every battery_read_mv() gives BATTERY_NO_READING */
	battery_init();

	/* On failure it prints why; without Bluetooth there is nothing to do */
	if (ble_init() != 0) {
		return 0;
	}

	/* No reading yet: advertise "no reading" rather than 0.00 °C during the window */
	ble_set_temperature(TEMP_NO_READING);

	/* Connectable for DL002_CONNECTABLE_WINDOW_MS after power-on, so a phone can connect */
	if (ble_adv_start_connectable() == 0) {
		k_msleep(DL002_CONNECTABLE_WINDOW_MS);
		ble_adv_stop();
	}

	/* BLE works, so a phone could connect and send the next update: keep this firmware */
	confirm_firmware();

	LOG_INF("Advertising %d ms (packet every %d ms) every %d ms",
	        DL002_ADV_DURATION_MS, DL002_ADV_INTERVAL_MS, DL002_ADV_CYCLE_MS);

	int64_t last_reading_ms = 0;
	bool first_reading = true;

	while (1) {
		/* Temperature: a new reading on the first cycle, then once a minute has passed */
		if (first_reading || k_uptime_get() - last_reading_ms >= DL002_TEMP_MEASURE_INTERVAL_MS) {
			/* 0.01 °C, or TEMP_NO_READING (0x8000) if there is no valid reading */
			int16_t temp_centi_celsius = temperature_read();

			ble_set_temperature(temp_centi_celsius);
			battery_read_mv();
			last_reading_ms = k_uptime_get();
			first_reading = false;
		}

		/* Advertising: every cycle, with the latest temperature. On failure it prints why */
		ble_adv_start();

		/* The alarm stops advertising during this sleep; CONFIG_PM puts the chip in EM2 */
		k_msleep(DL002_ADV_CYCLE_MS);
	}

	return 0;
}
