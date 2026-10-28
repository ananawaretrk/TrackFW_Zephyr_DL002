#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include <stdint.h>

#define TEMP_NO_READING  INT16_MIN   /* 0x8000: no valid reading */

int temperature_init(void);
int16_t temperature_read(void);

#endif
