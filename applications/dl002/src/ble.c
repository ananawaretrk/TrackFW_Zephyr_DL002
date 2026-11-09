/*
 * Bluetooth: start the stack, set the TX power, build our advertisement and advertise for
 * DL002_ADV_DURATION_MS at a time.
 */

#include <zephyr/logging/log.h>
#include <stdint.h>
#include <errno.h>                  /* ENOENT */
#include <stdlib.h>                 /* abs() */
#include <zephyr/kernel.h>          /* k_work_schedule() */
#include <zephyr/sys/byteorder.h>   /* sys_put_be16(), sys_put_le16() */
#include <zephyr/sys/util.h>        /* MIN() */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>  /* BT_CONN_CB_DEFINE(), bt_conn_get_dst() */
#include "app_config.h"             /* DL002_ADV_* settings */
#include "ble.h"

LOG_MODULE_REGISTER(ble);

/* Manufacturer data, 12 bytes, as seen in nRF Connect:
 * F7 0E             company 0x0EF7 "Trackonomy Systems, Inc." (little-endian, set by the Bluetooth spec)
 * 52 58             Trackonomy marker (same as AL300)
 * D4 4A 85 BF 3E 17 MAC, same order as the boot log
 * 09 24             temperature, int16 big-endian, 0.01 °C (0x0924 = 2340 = 23.40 °C)
 */
#define TRACKONOMY_COMPANY_ID  0x0EF7
#define TRACKONOMY_MARKER      0x5258

#define MFG_COMPANY_OFFSET  0
#define MFG_MARKER_OFFSET   (MFG_COMPANY_OFFSET + sizeof(uint16_t))
#define MFG_MAC_OFFSET      (MFG_MARKER_OFFSET + sizeof(uint16_t))
#define MFG_TEMP_OFFSET     (MFG_MAC_OFFSET + BT_ADDR_SIZE)
#define MFG_DATA_LEN        (MFG_TEMP_OFFSET + sizeof(int16_t))

/* BG22 datasheet table 4.13 note 3: specified up to 6 dBm; above that, harmonics may exceed
 * regulatory limits. The Silabs library itself allows up to 8.5 dBm, so we limit it here.
 */
#define TX_POWER_MAX_DBM  6
#define DECI_DBM_PER_DBM  10      /* the Silabs library counts power in 0.1 dBm steps */

/* In the Silabs Bluetooth library, but not in any header: Zephyr's driver declares it the
 * same way (zephyr/drivers/bluetooth/hci/hci_silabs_efr32.c). Returns the power actually set.
 */
int16_t BTLE_LL_SetMaxPower(int16_t power);

/* Fixed part filled in ble_init(), temperature by ble_set_temperature() */
static uint8_t mfg_data[MFG_DATA_LEN];

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

/* Connectable window: "general discoverable" tells phones they may list us and connect */
static const struct bt_data ad_connectable[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

/* Scan response, sent only when a phone asks: our name (CONFIG_BT_DEVICE_NAME), so the phone lists us */
static const struct bt_data sd_connectable[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Stop advertising (does nothing if we aren't advertising) */
void ble_adv_stop(void)
{
	int err = bt_le_adv_stop();

	if (err) {
		LOG_ERR("Advertising failed to stop (err %d)", err);
	}
}

/* Runs DL002_ADV_DURATION_MS after advertising starts (same idea as AL300's stop_adv_wk): radio off */
static void stop_adv_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	ble_adv_stop();
}

static K_WORK_DELAYABLE_DEFINE(stop_adv_work, stop_adv_handler);

/* Called by the Bluetooth stack when a phone connects (or the attempt fails) */
static void connected(struct bt_conn *conn, uint8_t err)
{
	char address_text[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), address_text, sizeof(address_text));
	if (err) {
		LOG_ERR("Connection from %s failed (err 0x%02x)", address_text, err);
		return;
	}
	LOG_INF("Connected: %s", address_text);
}

/* Called when the connection ends; reason 0x13 = the phone disconnected */
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char address_text[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), address_text, sizeof(address_text));
	LOG_INF("Disconnected: %s (reason 0x%02x)", address_text, reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Set the Bluetooth TX power (at most TX_POWER_MAX_DBM). The radio uses the nearest step at or
 * just below the request (lowest step -27.5 dBm); the Silabs library returns that step, so we
 * print it. Call after bt_enable().
 */
static void ble_set_tx_power(int8_t requested_dbm)
{
	int8_t allowed_dbm = MIN(requested_dbm, TX_POWER_MAX_DBM);

	if (allowed_dbm != requested_dbm) {
		LOG_WRN("TX power %d dBm is above the %d dBm limit, using %d dBm",
		        requested_dbm, TX_POWER_MAX_DBM, allowed_dbm);
	}

	int16_t set_deci_dbm = BTLE_LL_SetMaxPower(allowed_dbm * DECI_DBM_PER_DBM);
	int set_deci_dbm_abs = abs(set_deci_dbm);

	/* Whole numbers, no %f: -275 -> "-27.5" */
	LOG_INF("TX power: requested %d dBm, radio uses %s%d.%d dBm",
	        allowed_dbm, (set_deci_dbm < 0) ? "-" : "",
	        set_deci_dbm_abs / DECI_DBM_PER_DBM, set_deci_dbm_abs % DECI_DBM_PER_DBM);
}

/* Start the Bluetooth stack, set the TX power and fill the fixed part of the advertisement
 * (company ID, marker, MAC). Returns 0, the bt_enable() error, or -ENOENT if there is no address.
 */
int ble_init(void)
{
	/* Start the Bluetooth stack (radio library, controller) */
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}
	LOG_INF("Bluetooth initialized");

	ble_set_tx_power(DL002_ADV_TX_POWER_DBM);   /* prints the power the radio actually uses */

	/* Our own (identity) address, e.g. D4:4A:85:BF:3E:17 (public) */
	bt_addr_le_t identity_address;
	size_t identity_count = 1;
	char address_text[BT_ADDR_LE_STR_LEN];

	bt_id_get(&identity_address, &identity_count);
	if (identity_count == 0) {
		LOG_ERR("Bluetooth address FAILED: no identity address");
		return -ENOENT;
	}
	bt_addr_le_to_str(&identity_address, address_text, sizeof(address_text));
	LOG_INF("Bluetooth address %s", address_text);

	/* Fixed part of the advertisement: company ID (little-endian, Bluetooth spec), then the marker */
	sys_put_le16(TRACKONOMY_COMPANY_ID, &mfg_data[MFG_COMPANY_OFFSET]);
	sys_put_be16(TRACKONOMY_MARKER, &mfg_data[MFG_MARKER_OFFSET]);

	/* Copy the MAC into the advertisement. Zephyr stores it reversed, so val[5] goes first
	 * to make it read the same as the boot log.
	 */
	for (int byte_index = 0; byte_index < BT_ADDR_SIZE; byte_index++) {
		mfg_data[MFG_MAC_OFFSET + byte_index] = identity_address.a.val[BT_ADDR_SIZE - 1 - byte_index];
	}
	return 0;
}

/* Put the temperature into the advertisement (big-endian: 2340 -> 09 24).
 * Goes on air from the next ble_adv_start().
 */
void ble_set_temperature(int16_t temp_centi_celsius)
{
	sys_put_be16((uint16_t)temp_centi_celsius, &mfg_data[MFG_TEMP_OFFSET]);
}

/* Start advertising: non-connectable, fixed identity address, one packet every
 * DL002_ADV_INTERVAL_MS. An alarm stops it after DL002_ADV_DURATION_MS.
 * Returns 0, or the bt_le_adv_start() error.
 */
int ble_adv_start(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_USE_IDENTITY,
						  BT_GAP_MS_TO_ADV_INTERVAL(DL002_ADV_INTERVAL_MS),
						  BT_GAP_MS_TO_ADV_INTERVAL(DL002_ADV_INTERVAL_MS), NULL),
				  ad, ARRAY_SIZE(ad), NULL, 0);

	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return err;
	}

	/* Alarm: stop advertising after DL002_ADV_DURATION_MS */
	k_work_schedule(&stop_adv_work, K_MSEC(DL002_ADV_DURATION_MS));
	return 0;
}

/* Start connectable advertising (one packet every DL002_CONNECTABLE_ADV_INTERVAL_MS).
 * Runs until ble_adv_stop(), or until a phone connects (then the stack stops it).
 * Returns 0, or the bt_le_adv_start() error.
 */
int ble_adv_start_connectable(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
						  BT_GAP_MS_TO_ADV_INTERVAL(DL002_CONNECTABLE_ADV_INTERVAL_MS),
						  BT_GAP_MS_TO_ADV_INTERVAL(DL002_CONNECTABLE_ADV_INTERVAL_MS), NULL),
				  ad_connectable, ARRAY_SIZE(ad_connectable),
				  sd_connectable, ARRAY_SIZE(sd_connectable));

	if (err) {
		LOG_ERR("Connectable advertising failed to start (err %d)", err);
		return err;
	}
	LOG_INF("Connectable for %d ms", DL002_CONNECTABLE_WINDOW_MS);
	return 0;
}
