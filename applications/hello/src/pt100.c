/*
 * PT100 maths for the DL002 front end.
 * [SCH] PT100 box: 4-wire, IDAC 1 mA, gain 8, reference R74 = 1.62 kΩ 0.1 %,
 *       range -100 ... +220 °C, error 0.5 °C
 * [CVD] IEC 60751 Callendar–Van Dusen equation (also TI SBAA275, "A Basic Guide to RTD Measurements")
 */

#include <math.h>	/* sqrtf() */
#include "pt100.h"

/* code -> resistance ([SCH] formula, [HCT] p.49 + p.53): R = code / 2^23 / gain * Rref */
#define PT100_RREF_OHM        1620.0f		/* R74 */
#define PT100_ADC_GAIN        8.0f		/* must match GA in hct6911_setup_pt100() */
#define PT100_ADC_FULL_SCALE  8388608.0f	/* 2^23: the result is 24-bit signed */

float pt100_code_to_ohm(int32_t code)
{
	return (float)code / PT100_ADC_FULL_SCALE / PT100_ADC_GAIN * PT100_RREF_OHM;
}

/*
 * [CVD], R0 = 100 Ω:
 *   R(T) / R0 = 1 + A*T + B*T^2                        at or above 0 °C
 *   R(T) / R0 = 1 + A*T + B*T^2 + C*(T - 100)*T^3      below 0 °C
 */
#define PT100_R0_OHM  100.0f
#define CVD_A         3.9083e-3f
#define CVD_B         (-5.775e-7f)
#define CVD_C         (-4.183e-12f)

/*
 * 1. Quadratic formula: exact at or above 0 °C (no C term there).
 * 2. Below 0 °C it ignores C (up to 0.2 °C off at -100 °C), so correct it once with
 *    Newton's method: work out R/R0 at that temperature, see how far it is from what
 *    we measured, and move the temperature by error / slope.
 * Checked on PC (float maths, -100 ... +220 °C): error < 0.001 °C.
 */
float pt100_ohm_to_celsius(float r_ohm)
{
	float measured_ratio = r_ohm / PT100_R0_OHM;	/* R / R0 */

	/* 1. solve 1 + A*T + B*T^2 = R/R0 for T */
	float temp_c = (-CVD_A + sqrtf(CVD_A * CVD_A - 4.0f * CVD_B * (1.0f - measured_ratio))) /
		       (2.0f * CVD_B);

	if (temp_c >= 0.0f) {
		return temp_c;
	}

	/* 2. below 0 °C: one Newton correction including the C term */
	float temp_c_squared = temp_c * temp_c;

	/* R/R0 the full formula gives at temp_c, minus what we measured */
	float ratio_error = 1.0f + CVD_A * temp_c + CVD_B * temp_c_squared +
			    CVD_C * (temp_c - 100.0f) * temp_c_squared * temp_c - measured_ratio;
	/* how fast R/R0 changes per °C at temp_c */
	float ratio_slope = CVD_A + 2.0f * CVD_B * temp_c +
			    CVD_C * (4.0f * temp_c_squared * temp_c - 300.0f * temp_c_squared);

	return temp_c - ratio_error / ratio_slope;
}
