#ifndef BLE_H
#define BLE_H

#include <stdint.h>

int ble_init(void);
void ble_set_temperature(int16_t temp_centi_celsius);
void ble_set_battery(uint16_t battery_mv);
int ble_adv_start(void);
int ble_adv_start_connectable(void);
void ble_adv_stop(void);

#endif
