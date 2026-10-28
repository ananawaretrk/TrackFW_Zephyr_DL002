/*
 * Temperature: the HCT6911 ADC (hct6911.c) and the PT100 maths (pt100.c) put together.
 * Gives the temperature in 0.01 °C, or TEMP_NO_READING.
 */

#include <zephyr/logging/log.h>
#include <stdbool.h>
#include <errno.h>       /* EIO */
#include <math.h>        /* lroundf() */
#include "hct6911.h"     /* temperature ADC (PT100 front end) */
#include "pt100.h"       /* code -> ohm -> °C */
#include "temperature.h"

LOG_MODULE_REGISTER(temperature);

static bool adc_ready;   /* false until temperature_init() succeeds */

/* Temperature ADC (powered from boot, see adc_3v3 in dl002.dts): init, wake, reset,
 * configure for the PT100, then sleep until the first reading.
 * Wake first: after a reset without a power cycle (e.g. flashing), the HCT6911 can still be
 * asleep from the previous run, and a sleeping HCT6911 ignores a reset ([HCT] p.20).
 * Stops at the first failing step; that function prints why.
 * Returns 0 on success, -EIO if any step failed (temperature_read() then always gives
 * TEMP_NO_READING).
 */
int temperature_init(void)
{
	adc_ready = (hct6911_init() == 0) &&
		    (hct6911_wake() == 0) &&
		    (hct6911_reset() == 0) &&
		    (hct6911_setup_pt100() == 0) &&
		    (hct6911_sleep() == 0);

	if (!adc_ready) {
		LOG_ERR("HCT6911 not ready: no temperature readings");
		return -EIO;
	}
	return 0;
}

/* Wake the HCT6911, turn the excitation current back on (setup_pt100), take one reading and put
 * it back to sleep. Returns the temperature in 0.01 °C, or TEMP_NO_READING on any failure
 * (the function that failed has already printed why).
 */
int16_t temperature_read(void)
{
	if (!adc_ready) {
		return TEMP_NO_READING;
	}

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
		LOG_WRN("PT100 out of range: R = %.3f ohm (probe unplugged or shorted?)", (double)r_ohm);
		return TEMP_NO_READING;
	}

	/* %f expects a double: (double) makes the float -> double step explicit */
	LOG_INF("PT100 R = %.3f ohm, T = %.2f C", (double)r_ohm, (double)temp_c);

	/* 23.40 °C -> 2340 (rounded, so 23.3999 doesn't become 2339) */
	return (int16_t)lroundf(temp_c * 100.0f);
}
