/*
 * HCT6911 24-bit ADC: SPI register access + PT100 measurement (bring-up steps 3 and 4).
 * [HCT] = HCT6911 datasheet, printed page numbers
 *         (English pages: HCT6911_datasheet_EN_selected_pages.pdf).
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>	/* sys_get_be24/32, sys_put_be24/32 */
#include "hct6911.h"

/* SPI mode 0 (no CPOL/CPHA flags): SCLK idles low, sampled on rising edge ([HCT] p.31-32) */
static const struct spi_dt_spec hct = SPI_DT_SPEC_GET(DT_NODELABEL(hct6911),
						      SPI_WORD_SET(8) | SPI_TRANSFER_MSB);

/* Same device, but CS stays LOW between calls until spi_release_dt().
 * A conversion frame needs CS low from the command until the data is read ([HCT] p.26, Fig 4-7).
 */
static const struct spi_dt_spec hct_conv = SPI_DT_SPEC_GET(DT_NODELABEL(hct6911),
							   SPI_WORD_SET(8) | SPI_TRANSFER_MSB |
							   SPI_HOLD_ON_CS | SPI_LOCK_ON);

/* DOUT/DRDY# shares the MISO pin: it goes LOW when a conversion result is ready */
static const struct gpio_dt_spec drdy = GPIO_DT_SPEC_GET(DT_NODELABEL(hct6911), drdy_gpios);

/*
 * Command byte ([HCT] p.21, Table 4-1/4-2):
 *   bit 7    = 0 (read/write frame)
 *   bits 6:2 = register address
 *   bit 1    = 1 read, 0 write
 *   bit 0    = odd parity: makes the total number of 1s in the byte odd
 */
static uint8_t hct6911_cmd(uint8_t addr, bool read)
{
	uint8_t cmd = (addr << 2) | (read ? BIT(1) : 0);

	if (__builtin_popcount(cmd) % 2 == 0) {
		cmd |= BIT(0);
	}
	return cmd;
}

/*
 * Read one HCT6911 register.
 *   addr:   register address, e.g. HCT6911_SYS_CONF0
 *   nbytes: register size, HCT6911_REG_24BIT or HCT6911_REG_32BIT
 *   value:  where to store the result (pass &my_variable)
 * Returns 0 on success, or a negative SPI error code.
 *
 * On the wire ([HCT] p.24, Fig 4-3/4-4), CS low for the whole frame:
 *   MOSI: [read command] [00] [00] [00] ([00])
 *   MISO: [junk        ] [data, biggest byte first]
 */
int hct6911_read_reg(uint8_t addr, int nbytes, uint32_t *value)
{
	uint8_t tx[1 + HCT6911_REG_32BIT] = { hct6911_cmd(addr, true) };	/* rest is 0x00 while we listen */
	uint8_t rx[1 + HCT6911_REG_32BIT];
	const struct spi_buf tx_buf = { .buf = tx, .len = 1 + nbytes };
	const struct spi_buf rx_buf = { .buf = rx, .len = 1 + nbytes };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	int err = spi_transceive_dt(&hct, &tx_set, &rx_set);

	/* rx[0] arrived while the chip was still receiving the command: skip it */
	if (nbytes == HCT6911_REG_32BIT) {
		*value = sys_get_be32(&rx[1]);
	} else {
		*value = sys_get_be24(&rx[1]);
	}
	return err;
}

/*
 * Write one HCT6911 register.
 *   addr:   register address, e.g. HCT6911_CONV_CONF0
 *   nbytes: register size, HCT6911_REG_24BIT or HCT6911_REG_32BIT
 *   value:  the new register value
 * Returns 0 on success, or a negative SPI error code.
 *
 * On the wire ([HCT] p.23, Fig 4-1/4-2), CS low for the whole frame:
 *   MOSI: [write command] [data, biggest byte first]
 */
int hct6911_write_reg(uint8_t addr, int nbytes, uint32_t value)
{
	uint8_t tx[1 + HCT6911_REG_32BIT] = { hct6911_cmd(addr, false) };

	if (nbytes == HCT6911_REG_32BIT) {
		sys_put_be32(value, &tx[1]);
	} else {
		sys_put_be24(value, &tx[1]);
	}

	const struct spi_buf tx_buf = { .buf = tx, .len = 1 + nbytes };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };

	return spi_write_dt(&hct, &tx_set);
}

/* One-time set-up: check the SPI bus and the DRDY pin, and configure the DRDY pin as an input.
 * gpio_pin_configure_dt() is what makes GPIO_ACTIVE_LOW take effect for gpio_pin_get_dt().
 */
int hct6911_init(void)
{
	if (!spi_is_ready_dt(&hct)) {
		printf("HCT6911 init FAILED: SPI bus not ready\n");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&drdy)) {
		printf("HCT6911 init FAILED: DRDY GPIO port not ready\n");
		return -ENODEV;
	}
	return gpio_pin_configure_dt(&drdy, GPIO_INPUT);
}

#define HCT6911_RESET_WAIT_MS  15	/* reset takes 10 ms ([HCT] p.40) + margin */

/* Step 4.1: global reset. Write 1 to SYS_CONF0 bit 31 RS_SYS; it clears itself after 10 ms
 * ([HCT] p.40, Table 5-6; procedure step 1 on p.60).
 * On failure: prints why and returns a negative error code.
 */
int hct6911_reset(void)
{
	uint32_t sys_conf0;

	int err = hct6911_write_reg(HCT6911_SYS_CONF0, HCT6911_REG_32BIT, HCT6911_SYS_CONF0_RS_SYS);

	if (err) {
		printf("HCT6911 reset FAILED: SPI error %d\n", err);
		return err;
	}
	k_msleep(HCT6911_RESET_WAIT_MS);

	err = hct6911_read_reg(HCT6911_SYS_CONF0, HCT6911_REG_32BIT, &sys_conf0);
	if (err) {
		printf("HCT6911 reset FAILED: SPI error %d\n", err);
		return err;
	}

	bool reset_still_running = (sys_conf0 & HCT6911_SYS_CONF0_RS_SYS) != 0;
	bool reset_succeeded     = (sys_conf0 & HCT6911_SYS_CONF0_RS_V) != 0;

	if (reset_still_running || !reset_succeeded) {
		printf("HCT6911 reset FAILED: SYS_CONF0 = 0x%08X (expected 0x00008000)\n", sys_conf0);
		return -EIO;
	}
	return 0;
}

/* Write a 32-bit register, read it back and compare.
 * On failure: prints what went wrong and returns a negative error code.
 */
static int write_reg32_and_verify(uint8_t addr, uint32_t value)
{
	uint32_t readback;

	int err = hct6911_write_reg(addr, HCT6911_REG_32BIT, value);

	if (err) {
		printf("HCT6911 write register 0x%02X FAILED: SPI error %d\n", addr, err);
		return err;
	}
	err = hct6911_read_reg(addr, HCT6911_REG_32BIT, &readback);
	if (err) {
		printf("HCT6911 read register 0x%02X FAILED: SPI error %d\n", addr, err);
		return err;
	}
	if (readback != value) {
		printf("HCT6911 register 0x%02X FAILED: read back 0x%08X, wrote 0x%08X\n",
		       addr, readback, value);
		return -EIO;
	}
	return 0;
}

/*
 * Step 4.2: configure for the DL002's 4-wire PT100 ([HCT] p.60, procedure steps 2-3), using
 * this board's HCT6911 pin names ([HCT] p.14-15, Table 2-1):
 *   IDAC0 1 mA out of AIN0 (pin 6) -> RTD_B2 -> PT100 -> RTD_A1 -> R74 1.62k -> GND
 *   measure AIN5 (pin 10, RTD_B1) minus AIN6 (pin 11, RTD_A2)
 *   reference REFP1/REFN1 (pins 9/8) = voltage across R74
 * On failure: prints why and returns a negative error code.
 */
int hct6911_setup_pt100(void)
{
	/* CONV_CONF0 fields: [HCT] p.36-39, Table 5-5 */
	uint32_t conv_conf0 =
		BIT(31) |		/* CONV_EN: use this conversion setting */
		(0x8 << 20) |		/* DR: 25 Hz data rate */
		(0x1 << 16) |		/* FLIT_TYPE: 3rd-order filter */
		(0x0 << 14) |		/* REFSEL: REFP1/REFN1 = across R74 */
					/* bits 13/12 = 0: signal + reference buffers ON (default) */
		(0x3 << 8) |		/* GA: gain x8 (schematic) */
		(0x6 << 4) |		/* VNSEL: negative input = AIN6 (pin 11) */
		(0x5 << 0);		/* VPSEL: positive input = AIN5 (pin 10) */

	/* SYS_CONF2 fields: [HCT] p.45-46, Table 5-8 */
	uint32_t sys_conf2 =
		(0x0 << 8) |		/* IDAC0_CH: current out of AIN0 (pin 6) */
		(0x5 << 0);		/* IDAC0: 1000 uA (schematic) */

	int err = write_reg32_and_verify(HCT6911_CONV_CONF0, conv_conf0);

	if (err) {
		return err;
	}

	err = write_reg32_and_verify(HCT6911_SYS_CONF2, sys_conf2);
	
	return err;
}

/*
 * DOUT/DRDY# (pin 15) is read on PC03 (= MISO) while the chip converts ([HCT] p.26):
 *   busy  = chip has released the pin, R82 1 MΩ pulls it HIGH
 *   ready = chip pulls it LOW, result can be clocked out
 * drdy is GPIO_ACTIVE_LOW in the DTS, so gpio_pin_get_dt() returns 1 when ready.
 */
static bool drdy_is_ready(void)
{
	return gpio_pin_get_dt(&drdy) == 1;
}

/* Poll DRDY every 1 ms until it shows `want_ready`. Returns the ms waited, or -ETIMEDOUT. */
static int wait_for_drdy(bool want_ready, int timeout_ms)
{
	for (int waited_ms = 0; waited_ms <= timeout_ms; waited_ms++) {
		if (drdy_is_ready() == want_ready) {
			return waited_ms;
		}
		k_msleep(1);
	}
	return -ETIMEDOUT;
}

/*
 * Step 4.3: one single conversion ([HCT] p.25-26, Table 4-4/4-5, Fig 4-7).
 * CS is held LOW from the command until the data has been read. Every stage is checked:
 *   1. before start: DOUT is HIGH (idle, R82 pull-up)
 *   2. send CMD_SINGLE_CONVERSION (0x80) = "start measuring"
 *   3. DOUT goes HIGH: chip accepted the command and is converting
 *   4. DOUT goes LOW: conversion finished (~120 ms at 25 Hz, 3rd-order filter, [HCT] p.19)
 *   5. send CMD_READ_RESULT (0x42) = "send me the result", then clock in 24 data bits, CS high
 *   6. SYS_CONF0 CKS_ERR = 0: no command byte was rejected ([HCT] p.41)
 */
int hct6911_read_raw(int32_t *code)
{
	uint8_t cmd = HCT6911_CMD_SINGLE_CONVERSION;
	const struct spi_buf cmd_buf = { .buf = &cmd, .len = 1 };
	const struct spi_buf_set cmd_set = { .buffers = &cmd_buf, .count = 1 };

	uint8_t tx[1 + HCT6911_RESULT_BYTES] = { HCT6911_CMD_READ_RESULT };	/* rest is 0x00 while we listen */
	uint8_t rx[1 + HCT6911_RESULT_BYTES];
	const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	/* 1. Idle line must be HIGH before we start */
	if (drdy_is_ready()) {
		printf("HCT6911 step 1 FAILED: DOUT is LOW before start (expected HIGH)\n");
		return -EIO;
	}

	/* 2. Send the single-conversion command; CS stays LOW (hct_conv has SPI_HOLD_ON_CS) */
	int err = spi_write_dt(&hct_conv, &cmd_set);

	if (err) {
		spi_release_dt(&hct_conv);
		printf("HCT6911 step 2 FAILED: SPI error %d sending the conversion command\n", err);
		return err;
	}

	/* 3. Chip must release DOUT (HIGH = busy) to show it started converting */
	int start_ms = wait_for_drdy(false, 10);

	if (start_ms < 0) {
		spi_release_dt(&hct_conv);
		printf("HCT6911 step 3 FAILED: DOUT never went HIGH, conversion did not start\n");
		return -EIO;
	}

	/* 4. Wait for DOUT LOW = conversion finished */
	int done_ms = wait_for_drdy(true, 500);

	if (done_ms < 0) {
		spi_release_dt(&hct_conv);
		printf("HCT6911 step 4 FAILED: conversion did not finish within 500 ms\n");
		return -ETIMEDOUT;
	}

	/* 5. Send CMD_READ_RESULT and read the 24-bit result, then CS HIGH to end the frame */
	err = spi_transceive_dt(&hct_conv, &tx_set, &rx_set);
	spi_release_dt(&hct_conv);
	if (err) {
		printf("HCT6911 step 5 FAILED: SPI error %d reading data\n", err);
		return err;
	}

	/* 6. Did the chip reject any command byte? (reading SYS_CONF0 also clears the flag) */
	uint32_t sys_conf0;

	hct6911_read_reg(HCT6911_SYS_CONF0, HCT6911_REG_32BIT, &sys_conf0);
	if (sys_conf0 & HCT6911_SYS_CONF0_CKS_ERR) {
		printf("HCT6911 step 6 FAILED: CKS_ERR set, SYS_CONF0 = 0x%08X\n", sys_conf0);
		return -EIO;
	}

	/* Bring-up debug: timing and raw bytes. Remove once readings are stable. */
	printf("HCT6911 debug: busy after %d ms, ready after %d ms, rx = %02X %02X %02X %02X, SYS_CONF0 = 0x%08X\n",
	       start_ms, done_ms, rx[0], rx[1], rx[2], rx[3], sys_conf0);

	/* rx[0] arrived during the CMD_READ_RESULT byte: skip it.
	 * Result is 24-bit signed, biggest byte first; bit 23 is the sign bit ([HCT] p.49).
	 */
	*code = sign_extend(sys_get_be24(&rx[1]), 23);
	return 0;
}
