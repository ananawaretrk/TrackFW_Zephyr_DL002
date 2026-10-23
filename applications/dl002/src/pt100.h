#ifndef PT100_H
#define PT100_H

#include <stdint.h>

/* Rated probe range ([SCH]: -100 ... +220 °C). A reading outside it is not trusted:
 * most likely the probe is unplugged, broken or shorted.
 */
#define PT100_MIN_CELSIUS  (-100.0f)
#define PT100_MAX_CELSIUS  (220.0f)

float pt100_code_to_ohm(int32_t code);
float pt100_ohm_to_celsius(float r_ohm);

#endif
