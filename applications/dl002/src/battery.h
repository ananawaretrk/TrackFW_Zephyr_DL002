#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

#define BATTERY_NO_READING  0   /* no valid reading (a real battery is never 0 mV) */

int battery_init(void);
uint16_t battery_read_mv(void);

#endif
