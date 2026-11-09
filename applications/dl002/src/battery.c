/*
 * Battery voltage. [SCH] The coin cell feeds the chip directly (BAT+ -> Q1 reverse-polarity
 * P-MOSFET -> VDD = AVDD/IOVDD), with no divider and no spare pin, so the chip's own ADC (IADC)
 * measures its supply (AVDD).
 */

#include <zephyr/logging/log.h>
#include <zephyr/drivers/adc.h>
#include <errno.h>          /* ENODEV */
#include <stdbool.h>
#include "battery.h"

LOG_MODULE_REGISTER(battery);

/* AVDD channel from the devicetree (zephyr,user io-channels in dl002.dts) */
static const struct adc_dt_spec avdd_channel = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

/* The IADC measures supply inputs divided by 4 ([RM] §24.3.5.2, Table 24.3: AVDD -> AVDD / 4) */
#define AVDD_INPUT_DIVIDER  4

static bool adc_ready;   /* false until battery_init() succeeds */

/* Set up the ADC channel. Returns 0, or a negative error (battery_read_mv() then always gives
 * BATTERY_NO_READING).
 */
int battery_init(void)
{
	if (!adc_is_ready_dt(&avdd_channel)) {
		LOG_ERR("Battery ADC not ready");
		return -ENODEV;
	}

	int err = adc_channel_setup_dt(&avdd_channel);

	if (err) {
		LOG_ERR("Battery ADC channel setup FAILED (err %d)", err);
		return err;
	}

	adc_ready = true;
	return 0;
}

/* One reading of the battery voltage in mV, or BATTERY_NO_READING on any failure */
uint16_t battery_read_mv(void)
{
	if (!adc_ready) {
		return BATTERY_NO_READING;
	}

	uint16_t adc_code;
	struct adc_sequence sequence = {
		.buffer = &adc_code,
		.buffer_size = sizeof(adc_code),
	};

	adc_sequence_init_dt(&avdd_channel, &sequence);

	int err = adc_read_dt(&avdd_channel, &sequence);

	if (err) {
		LOG_ERR("Battery ADC read FAILED (err %d)", err);
		return BATTERY_NO_READING;
	}

	/* ADC code -> mV at the ADC input (AVDD / 4), using the internal 1.21 V reference */
	int32_t input_mv = adc_code;

	err = adc_raw_to_millivolts_dt(&avdd_channel, &input_mv);
	if (err) {
		LOG_ERR("Battery ADC conversion FAILED (err %d)", err);
		return BATTERY_NO_READING;
	}

	uint16_t battery_mv = input_mv * AVDD_INPUT_DIVIDER;

	LOG_INF("Battery %u mV (ADC code %u)", battery_mv, adc_code);
	return battery_mv;
}
