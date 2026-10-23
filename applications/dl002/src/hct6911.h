#ifndef HCT6911_H
#define HCT6911_H

#include <stdint.h>
#include <zephyr/sys/util.h>	/* BIT() */

/* Register addresses ([HCT] p.33-34, Table 5-1) */
#define HCT6911_OS_CH0     0x00	/* 24 bit, R/W, default 0x000000 */
#define HCT6911_GAIN_CH0   0x01	/* 24 bit, R/W, default 0x400000 */
#define HCT6911_D_TARG     0x10	/* 24 bit, R/W, default 0x7FFFFF */
#define HCT6911_CONV_CONF0 0x11	/* 32 bit, R/W, default 0x00000000 */
#define HCT6911_SYS_CONF0  0x1B	/* 32 bit, R/W, default 0x00008000 */
#define HCT6911_SYS_CONF1  0x1C	/* 32 bit, R/W, default 0x00000000 */
#define HCT6911_SYS_CONF2  0x1D	/* 32 bit, R/W, default 0x00000000 */

/* Register sizes in bytes ([HCT] p.33-34, Table 5-1) */
#define HCT6911_REG_24BIT  3
#define HCT6911_REG_32BIT  4

/* Conversion frame ([HCT] p.25-26, Table 4-4, Fig 4-7) */
#define HCT6911_CMD_SINGLE_CONVERSION  0x80	/* bit 7 = 1 conversion frame, CONV_MOD 0 = single, parity 0 */
#define HCT6911_CMD_READ_RESULT        0x42	/* must be sent in the 8 clocks before the result */
#define HCT6911_RESULT_BYTES           3	/* result is 24-bit signed ([HCT] p.49) */

/* SYS_CONF0 bits ([HCT] p.40-42, Table 5-6) */
#define HCT6911_SYS_CONF0_RS_SYS   BIT(31)	/* write 1 = start reset; reads 1 while resetting */
#define HCT6911_SYS_CONF0_RS_V     BIT(15)	/* 1 = last reset succeeded */
#define HCT6911_SYS_CONF0_CKS_ERR  BIT(10)	/* 1 = a command byte was rejected (parity/CRC) */
#define HCT6911_SYS_CONF1_POWD     BIT(15)	/* 1 = sleep, < 1 uA ([HCT] p.20, p.44) */

int hct6911_read_reg(uint8_t addr, int nbytes, uint32_t *value);
int hct6911_write_reg(uint8_t addr, int nbytes, uint32_t value);

int hct6911_init(void);
int hct6911_reset(void);
int hct6911_setup_pt100(void);
int hct6911_read_raw(int32_t *code);
int hct6911_sleep(void);
int hct6911_wake(void);

#endif
