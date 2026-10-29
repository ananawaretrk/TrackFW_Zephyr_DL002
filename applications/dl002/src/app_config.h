#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <zephyr/toolchain.h>   /* BUILD_ASSERT() */

/* Each cycle: read the temperature if one is due, start advertising (an alarm stops it after
 * DL002_ADV_DURATION_MS), sleep DL002_ADV_CYCLE_MS
 */
#define DL002_ADV_INTERVAL_MS   20      /* time between packets while advertising (BLE min 20 ms) */
#define DL002_ADV_DURATION_MS   50     /* how long we advertise each cycle */
#define DL002_ADV_CYCLE_MS      1000    /* start of one cycle to the start of the next (+ reading time) */
#define DL002_ADV_TX_POWER_DBM  0       /* radio output power, max +6 dBm (lowest step -27.5) */

BUILD_ASSERT(DL002_ADV_DURATION_MS < DL002_ADV_CYCLE_MS, "advertising must be shorter than the cycle");

/* A new temperature reading at most every DL002_TEMP_MEASURE_INTERVAL_MS, checked once per cycle */
#define DL002_TEMP_MEASURE_INTERVAL_MS  60000   /* 1 min */

#endif
