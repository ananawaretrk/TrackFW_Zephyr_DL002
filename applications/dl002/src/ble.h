#ifndef BLE_H
#define BLE_H

#include <stdint.h>

int ble_init(void);
void ble_set_temperature(int16_t temp_centi_celsius);
int ble_adv_start(void);

#endif
