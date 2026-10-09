// SPDX-License-Identifier: GPL-2.0
/*
 * SK Hynix Hi-259 2MP camera sensor (the macro camera of the Xiaomi POCO X3 NFC "surya").
 *
 * There is no public datasheet. The register tables were decoded from the phone's CamX
 * sensor module (com.qti.sensormodule.j20c_ofilm_hi259_macro.bin) and cross-checked against
 * Xiaomi's open-source MediaTek driver (hi259ofilm_mipiraw_Sensor.c); the register meanings
 * below come from that driver. Registers and values are 8 bit; 0x03 selects the page.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define HI259_REG_PAGE			CCI_REG8(0x03)
#define HI259_REG_CHIP_ID		CCI_REG8(0x04)	/* page 0 */
#define HI259_CHIP_ID			0xe1
#define HI259_REG_MODE			CCI_REG8(0x01)	/* page 0: 0x00 streaming, 0x01 standby */
#define HI259_REG_GROUP_HOLD		CCI_REG8(0x1f)	/* page 0 */
#define HI259_REG_TEST_PATTERN		CCI_REG8(0x60)	/* page 0: 0x04 = colour bars */
#define HI259_REG_EXPOSURE_H		CCI_REG8(0x22)	/* page 0x20, in lines */
#define HI259_REG_EXPOSURE_L		CCI_REG8(0x23)
#define HI259_REG_GAIN_H		CCI_REG8(0x60)	/* page 0x20: gain code >> 1 */
#define HI259_REG_GAIN_L		CCI_REG8(0x61)	/* page 0x20: gain code & 1 */

#define HI259_WIDTH			1600
#define HI259_HEIGHT			1200
#define HI259_LINE_LENGTH		1800
#define HI259_FRAME_LENGTH		1444
#define HI259_PIXEL_RATE		78080000ULL
/* RAW10 on one lane, double data rate */
#define HI259_LINK_FREQ			(HI259_PIXEL_RATE * 10 / 2)
#define HI259_MCLK			19200000
#define HI259_EXPOSURE_MARGIN		4

/*
 * V4L2 analogue gain in 1/64 steps, 64 = 1x (MediaTek BASEGAIN convention). MediaTek stops at 8x (code 30),
 * but the code goes down to 0 (about 15x): the macro camera stayed far too dark indoors with libcamera's
 * software ISP, so up to 14x (code 2). The auto exposure only goes there when the picture is dark.
 */
#define HI259_GAIN_MIN			64
#define HI259_GAIN_MAX			896

static const s64 hi259_link_freqs[] = { HI259_LINK_FREQ };

static const char * const hi259_supply_names[] = {
	"dovdd",	/* 1.8 V I/O */
	"avdd",		/* 2.8 V analog */
};

static const char * const hi259_test_pattern_menu[] = {
	"Disabled",
	"Colour Bars",
};

struct hi259 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *clk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(hi259_supply_names)];
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
};

static inline struct hi259 *to_hi259(struct v4l2_subdev *sd)
{
	return container_of(sd, struct hi259, sd);
}

/* Global init, 673 writes, from the surya CamX sensor module (ofilm hi259, MCLK 19.2 MHz) */
static const struct reg_sequence hi259_init_regs[] = {
	{ CCI_REG8(0x03), 0x00 },
	{ CCI_REG8(0x01), 0x01 },
	{ CCI_REG8(0x01), 0x03 },
	{ CCI_REG8(0x01), 0x01 },
	{ CCI_REG8(0x03), 0x02 },
	{ CCI_REG8(0x1f), 0x01 },
	{ CCI_REG8(0x03), 0x00 },
	{ CCI_REG8(0x07), 0x05 },
	{ CCI_REG8(0x08), 0x7a },
	{ CCI_REG8(0x09), 0x13 },
	{ CCI_REG8(0x07), 0x85 },
	{ CCI_REG8(0x07), 0x85 },
	{ CCI_REG8(0x07), 0x85 },
	{ CCI_REG8(0x0a), 0x80 },
	{ CCI_REG8(0x07), 0xc5 },
	{ CCI_REG8(0x03), 0x00 },
	{ CCI_REG8(0x10), 0x00 },
	{ CCI_REG8(0x11), 0x80 },
	{ CCI_REG8(0x13), 0x01 },
	{ CCI_REG8(0x14), 0x20 },
	{ CCI_REG8(0x15), 0x81 },
	{ CCI_REG8(0x17), 0x10 },
	{ CCI_REG8(0x1a), 0x00 },
	{ CCI_REG8(0x1c), 0x00 },
	{ CCI_REG8(0x1f), 0x00 },
	{ CCI_REG8(0x20), 0x00 },
	{ CCI_REG8(0x21), 0x00 },
	{ CCI_REG8(0x22), 0x00 },
	{ CCI_REG8(0x23), 0x00 },
	{ CCI_REG8(0x24), 0x00 },
	{ CCI_REG8(0x25), 0x00 },
	{ CCI_REG8(0x26), 0x00 },
	{ CCI_REG8(0x27), 0x00 },
	{ CCI_REG8(0x28), 0x04 },
	{ CCI_REG8(0x29), 0xcc },
	{ CCI_REG8(0x2a), 0x06 },
	{ CCI_REG8(0x2b), 0x60 },
	{ CCI_REG8(0x30), 0x00 },
	{ CCI_REG8(0x31), 0x00 },
	{ CCI_REG8(0x32), 0x00 },
	{ CCI_REG8(0x33), 0x00 },
	{ CCI_REG8(0x34), 0x00 },
	{ CCI_REG8(0x35), 0x00 },
	{ CCI_REG8(0x36), 0x00 },
	{ CCI_REG8(0x37), 0x00 },
	{ CCI_REG8(0x38), 0x02 },
	{ CCI_REG8(0x39), 0x60 },
	{ CCI_REG8(0x3a), 0x03 },
	{ CCI_REG8(0x3b), 0x20 },
	{ CCI_REG8(0x4c), 0x07 },
	{ CCI_REG8(0x4d), 0x08 },
	{ CCI_REG8(0x4e), 0x05 },
	{ CCI_REG8(0x4f), 0xa4 },
	{ CCI_REG8(0x54), 0x02 },
	{ CCI_REG8(0x55), 0x03 },
	{ CCI_REG8(0x56), 0x04 },
	{ CCI_REG8(0x57), 0x40 },
	{ CCI_REG8(0x58), 0x03 },
	{ CCI_REG8(0x5c), 0x0a },
	{ CCI_REG8(0x60), 0x00 },
	{ CCI_REG8(0x61), 0x00 },
	{ CCI_REG8(0x62), 0x80 },
	{ CCI_REG8(0x68), 0x03 },
	{ CCI_REG8(0x69), 0x42 },
	{ CCI_REG8(0x80), 0x00 },
	{ CCI_REG8(0x81), 0x08 },
	{ CCI_REG8(0x82), 0x00 },
	{ CCI_REG8(0x83), 0x06 },
	{ CCI_REG8(0x84), 0x06 },
	{ CCI_REG8(0x85), 0x50 },
	{ CCI_REG8(0x86), 0x04 },
	{ CCI_REG8(0x87), 0xc0 },
	{ CCI_REG8(0x88), 0x00 },
	{ CCI_REG8(0x89), 0x06 },
	{ CCI_REG8(0x8a), 0x02 },
	{ CCI_REG8(0x8b), 0x60 },
	{ CCI_REG8(0x90), 0x00 },
	{ CCI_REG8(0x91), 0x02 },
	{ CCI_REG8(0xa0), 0x01 },
	{ CCI_REG8(0xa1), 0x40 },
	{ CCI_REG8(0xa2), 0x40 },
	{ CCI_REG8(0xa3), 0x40 },
	{ CCI_REG8(0xa4), 0x40 },
	{ CCI_REG8(0xe4), 0x10 },
	{ CCI_REG8(0xe5), 0x00 },
	{ CCI_REG8(0x03), 0x01 },
	{ CCI_REG8(0x10), 0x21 },
	{ CCI_REG8(0x11), 0x00 },
	{ CCI_REG8(0x12), 0x3f },
	{ CCI_REG8(0x13), 0x08 },
	{ CCI_REG8(0x14), 0x04 },
	{ CCI_REG8(0x15), 0x01 },
	{ CCI_REG8(0x16), 0x00 },
	{ CCI_REG8(0x17), 0x03 },
	{ CCI_REG8(0x18), 0x00 },
	{ CCI_REG8(0x19), 0x00 },
	{ CCI_REG8(0x20), 0x60 },
	{ CCI_REG8(0x21), 0x00 },
	{ CCI_REG8(0x22), 0x20 },
	{ CCI_REG8(0x23), 0x3c },
	{ CCI_REG8(0x24), 0x5c },
	{ CCI_REG8(0x25), 0x00 },
	{ CCI_REG8(0x26), 0x60 },
	{ CCI_REG8(0x27), 0x07 },
	{ CCI_REG8(0x28), 0x80 },
	{ CCI_REG8(0x29), 0x00 },
	{ CCI_REG8(0x2a), 0xff },
	{ CCI_REG8(0x2b), 0x20 },
	{ CCI_REG8(0x2c), 0x80 },
	{ CCI_REG8(0x2d), 0x80 },
	{ CCI_REG8(0x2e), 0x80 },
	{ CCI_REG8(0x2f), 0x80 },
	{ CCI_REG8(0x85), 0x10 },
	{ CCI_REG8(0x30), 0x7b },
	{ CCI_REG8(0x31), 0x01 },
	{ CCI_REG8(0x32), 0xfe },
	{ CCI_REG8(0x33), 0x00 },
	{ CCI_REG8(0x34), 0x51 },
	{ CCI_REG8(0x35), 0xb6 },
	{ CCI_REG8(0x36), 0xfc },
	{ CCI_REG8(0x38), 0x66 },
	{ CCI_REG8(0x39), 0x66 },
	{ CCI_REG8(0x40), 0x00 },
	{ CCI_REG8(0x41), 0x00 },
	{ CCI_REG8(0x42), 0x00 },
	{ CCI_REG8(0x43), 0x00 },
	{ CCI_REG8(0x44), 0x00 },
	{ CCI_REG8(0x45), 0x00 },
	{ CCI_REG8(0x48), 0x00 },
	{ CCI_REG8(0x49), 0x00 },
	{ CCI_REG8(0x4a), 0x00 },
	{ CCI_REG8(0x4b), 0x00 },
	{ CCI_REG8(0x4c), 0x00 },
	{ CCI_REG8(0x4d), 0x00 },
	{ CCI_REG8(0x50), 0x00 },
	{ CCI_REG8(0x51), 0x00 },
	{ CCI_REG8(0x52), 0x00 },
	{ CCI_REG8(0x53), 0x00 },
	{ CCI_REG8(0x54), 0x00 },
	{ CCI_REG8(0x55), 0x00 },
	{ CCI_REG8(0x58), 0x00 },
	{ CCI_REG8(0x59), 0x00 },
	{ CCI_REG8(0x5a), 0x00 },
	{ CCI_REG8(0x5b), 0x00 },
	{ CCI_REG8(0x5c), 0x00 },
	{ CCI_REG8(0x5d), 0x00 },
	{ CCI_REG8(0x80), 0x00 },
	{ CCI_REG8(0x81), 0x00 },
	{ CCI_REG8(0x82), 0x00 },
	{ CCI_REG8(0x83), 0x00 },
	{ CCI_REG8(0x88), 0x20 },
	{ CCI_REG8(0x8a), 0x30 },
	{ CCI_REG8(0x8c), 0x00 },
	{ CCI_REG8(0x90), 0x00 },
	{ CCI_REG8(0x91), 0x60 },
	{ CCI_REG8(0x92), 0x00 },
	{ CCI_REG8(0x93), 0x60 },
	{ CCI_REG8(0x03), 0x02 },
	{ CCI_REG8(0x10), 0x00 },
	{ CCI_REG8(0x11), 0x00 },
	{ CCI_REG8(0x12), 0x70 },
	{ CCI_REG8(0x13), 0x00 },
	{ CCI_REG8(0x16), 0x00 },
	{ CCI_REG8(0x17), 0x00 },
	{ CCI_REG8(0x19), 0x00 },
	{ CCI_REG8(0x1a), 0x10 },
	{ CCI_REG8(0x1b), 0x00 },
	{ CCI_REG8(0x1c), 0xc0 },
	{ CCI_REG8(0x1d), 0x20 },
	{ CCI_REG8(0x20), 0x04 },
	{ CCI_REG8(0x21), 0x04 },
	{ CCI_REG8(0x22), 0x06 },
	{ CCI_REG8(0x23), 0x10 },
	{ CCI_REG8(0x24), 0x04 },
	{ CCI_REG8(0x28), 0x00 },
	{ CCI_REG8(0x29), 0x06 },
	{ CCI_REG8(0x2a), 0x00 },
	{ CCI_REG8(0x2e), 0x00 },
	{ CCI_REG8(0x2f), 0x2c },
	{ CCI_REG8(0x30), 0x00 },
	{ CCI_REG8(0x31), 0x44 },
	{ CCI_REG8(0x32), 0x02 },
	{ CCI_REG8(0x33), 0x00 },
	{ CCI_REG8(0x34), 0x00 },
	{ CCI_REG8(0x35), 0x00 },
	{ CCI_REG8(0x36), 0x06 },
	{ CCI_REG8(0x37), 0xc0 },
	{ CCI_REG8(0x38), 0x00 },
	{ CCI_REG8(0x39), 0x32 },
	{ CCI_REG8(0x3a), 0x80 },
	{ CCI_REG8(0x3b), 0x04 },
	{ CCI_REG8(0x3c), 0x04 },
	{ CCI_REG8(0x3d), 0xfe },
	{ CCI_REG8(0x3e), 0x00 },
	{ CCI_REG8(0x3f), 0x00 },
	{ CCI_REG8(0x40), 0x00 },
	{ CCI_REG8(0x41), 0x17 },
	{ CCI_REG8(0x42), 0x11 },
	{ CCI_REG8(0x43), 0x25 },
	{ CCI_REG8(0x47), 0x00 },
	{ CCI_REG8(0x48), 0x9a },
	{ CCI_REG8(0x49), 0x24 },
	{ CCI_REG8(0x4a), 0x0f },
	{ CCI_REG8(0x4b), 0x20 },
	{ CCI_REG8(0x4c), 0x06 },
	{ CCI_REG8(0x4d), 0xc0 },
	{ CCI_REG8(0x50), 0xa9 },
	{ CCI_REG8(0x51), 0x1c },
	{ CCI_REG8(0x52), 0x73 },
	{ CCI_REG8(0x54), 0xc0 },
	{ CCI_REG8(0x55), 0x40 },
	{ CCI_REG8(0x56), 0x11 },
	{ CCI_REG8(0x57), 0x00 },
	{ CCI_REG8(0x58), 0x18 },
	{ CCI_REG8(0x59), 0x16 },
	{ CCI_REG8(0x5b), 0x00 },
	{ CCI_REG8(0x62), 0x00 },
	{ CCI_REG8(0x63), 0xc8 },
	{ CCI_REG8(0x67), 0x3f },
	{ CCI_REG8(0x68), 0xc0 },
	{ CCI_REG8(0x70), 0x03 },
	{ CCI_REG8(0x71), 0xc7 },
	{ CCI_REG8(0x72), 0x06 },
	{ CCI_REG8(0x73), 0x75 },
	{ CCI_REG8(0x74), 0x03 },
	{ CCI_REG8(0x75), 0xc7 },
	{ CCI_REG8(0x76), 0x05 },
	{ CCI_REG8(0x77), 0x1d },
	{ CCI_REG8(0xa0), 0x01 },
	{ CCI_REG8(0xa1), 0x2c },
	{ CCI_REG8(0xa2), 0x02 },
	{ CCI_REG8(0xa3), 0xe0 },
	{ CCI_REG8(0xa4), 0x03 },
	{ CCI_REG8(0xa5), 0x84 },
	{ CCI_REG8(0xa6), 0x06 },
	{ CCI_REG8(0xa7), 0xf6 },
	{ CCI_REG8(0xb0), 0x02 },
	{ CCI_REG8(0xb1), 0x10 },
	{ CCI_REG8(0xb2), 0x02 },
	{ CCI_REG8(0xb3), 0xdc },
	{ CCI_REG8(0xb4), 0x03 },
	{ CCI_REG8(0xb5), 0xe2 },
	{ CCI_REG8(0xb6), 0x06 },
	{ CCI_REG8(0xb7), 0xf2 },
	{ CCI_REG8(0xc0), 0x02 },
	{ CCI_REG8(0xc1), 0x10 },
	{ CCI_REG8(0xc2), 0x02 },
	{ CCI_REG8(0xc3), 0xde },
	{ CCI_REG8(0xc4), 0x03 },
	{ CCI_REG8(0xc5), 0xe2 },
	{ CCI_REG8(0xc6), 0x06 },
	{ CCI_REG8(0xc7), 0xf4 },
	{ CCI_REG8(0xc8), 0x01 },
	{ CCI_REG8(0xc9), 0x8e },
	{ CCI_REG8(0xca), 0x01 },
	{ CCI_REG8(0xcb), 0x3e },
	{ CCI_REG8(0xcc), 0x03 },
	{ CCI_REG8(0xcd), 0x1e },
	{ CCI_REG8(0xce), 0x02 },
	{ CCI_REG8(0xcf), 0xe2 },
	{ CCI_REG8(0xd0), 0x00 },
	{ CCI_REG8(0xd1), 0x00 },
	{ CCI_REG8(0xd2), 0x00 },
	{ CCI_REG8(0xd3), 0x00 },
	{ CCI_REG8(0xd4), 0x1b },
	{ CCI_REG8(0xd5), 0x00 },
	{ CCI_REG8(0xe0), 0x1c },
	{ CCI_REG8(0xe1), 0x1c },
	{ CCI_REG8(0xe2), 0x1c },
	{ CCI_REG8(0xe3), 0x04 },
	{ CCI_REG8(0xe4), 0x1c },
	{ CCI_REG8(0xe5), 0x01 },
	{ CCI_REG8(0xe8), 0x00 },
	{ CCI_REG8(0xe9), 0x00 },
	{ CCI_REG8(0xea), 0x00 },
	{ CCI_REG8(0xeb), 0x00 },
	{ CCI_REG8(0xec), 0x00 },
	{ CCI_REG8(0xed), 0x00 },
	{ CCI_REG8(0xf0), 0x70 },
	{ CCI_REG8(0xf1), 0x00 },
	{ CCI_REG8(0xf2), 0x82 },
	{ CCI_REG8(0xf3), 0x00 },
	{ CCI_REG8(0x03), 0x03 },
	{ CCI_REG8(0x10), 0x00 },
	{ CCI_REG8(0x11), 0x80 },
	{ CCI_REG8(0x12), 0x00 },
	{ CCI_REG8(0x13), 0x02 },
	{ CCI_REG8(0x14), 0x06 },
	{ CCI_REG8(0x15), 0xeb },
	{ CCI_REG8(0x16), 0x06 },
	{ CCI_REG8(0x17), 0xf5 },
	{ CCI_REG8(0x18), 0x00 },
	{ CCI_REG8(0x19), 0xe8 },
	{ CCI_REG8(0x1a), 0x06 },
	{ CCI_REG8(0x1b), 0xf6 },
	{ CCI_REG8(0x1c), 0x00 },
	{ CCI_REG8(0x1d), 0xe8 },
	{ CCI_REG8(0x1e), 0x06 },
	{ CCI_REG8(0x1f), 0xf6 },
	{ CCI_REG8(0x20), 0x00 },
	{ CCI_REG8(0x21), 0xe8 },
	{ CCI_REG8(0x22), 0x01 },
	{ CCI_REG8(0x23), 0x1c },
	{ CCI_REG8(0x24), 0x00 },
	{ CCI_REG8(0x25), 0xe8 },
	{ CCI_REG8(0x26), 0x01 },
	{ CCI_REG8(0x27), 0x1c },
	{ CCI_REG8(0x28), 0x00 },
	{ CCI_REG8(0x29), 0xe8 },
	{ CCI_REG8(0x2a), 0x01 },
	{ CCI_REG8(0x2b), 0x1e },
	{ CCI_REG8(0x2c), 0x00 },
	{ CCI_REG8(0x2d), 0xe8 },
	{ CCI_REG8(0x2e), 0x01 },
	{ CCI_REG8(0x2f), 0x1e },
	{ CCI_REG8(0x30), 0x01 },
	{ CCI_REG8(0x31), 0x2c },
	{ CCI_REG8(0x32), 0x06 },
	{ CCI_REG8(0x33), 0xf6 },
	{ CCI_REG8(0x34), 0x01 },
	{ CCI_REG8(0x35), 0x2c },
	{ CCI_REG8(0x36), 0x06 },
	{ CCI_REG8(0x37), 0xf6 },
	{ CCI_REG8(0x38), 0x06 },
	{ CCI_REG8(0x39), 0xf1 },
	{ CCI_REG8(0x3a), 0x06 },
	{ CCI_REG8(0x3b), 0xfb },
	{ CCI_REG8(0x3c), 0x00 },
	{ CCI_REG8(0x3d), 0x04 },
	{ CCI_REG8(0x3e), 0x00 },
	{ CCI_REG8(0x3f), 0x0a },
	{ CCI_REG8(0x40), 0x00 },
	{ CCI_REG8(0x41), 0x04 },
	{ CCI_REG8(0x42), 0x00 },
	{ CCI_REG8(0x43), 0x3c },
	{ CCI_REG8(0x44), 0x00 },
	{ CCI_REG8(0x45), 0x02 },
	{ CCI_REG8(0x46), 0x00 },
	{ CCI_REG8(0x47), 0x74 },
	{ CCI_REG8(0x48), 0x00 },
	{ CCI_REG8(0x49), 0x06 },
	{ CCI_REG8(0x4a), 0x00 },
	{ CCI_REG8(0x4b), 0x3a },
	{ CCI_REG8(0x4c), 0x00 },
	{ CCI_REG8(0x4d), 0x06 },
	{ CCI_REG8(0x4e), 0x00 },
	{ CCI_REG8(0x4f), 0x3a },
	{ CCI_REG8(0x50), 0x00 },
	{ CCI_REG8(0x51), 0x0a },
	{ CCI_REG8(0x52), 0x00 },
	{ CCI_REG8(0x53), 0x38 },
	{ CCI_REG8(0x54), 0x00 },
	{ CCI_REG8(0x55), 0x0a },
	{ CCI_REG8(0x56), 0x00 },
	{ CCI_REG8(0x57), 0x38 },
	{ CCI_REG8(0x58), 0x00 },
	{ CCI_REG8(0x59), 0x0a },
	{ CCI_REG8(0x5a), 0x00 },
	{ CCI_REG8(0x5b), 0x38 },
	{ CCI_REG8(0x60), 0x00 },
	{ CCI_REG8(0x61), 0x07 },
	{ CCI_REG8(0x62), 0x00 },
	{ CCI_REG8(0x63), 0x15 },
	{ CCI_REG8(0x64), 0x00 },
	{ CCI_REG8(0x65), 0x07 },
	{ CCI_REG8(0x66), 0x00 },
	{ CCI_REG8(0x68), 0x00 },
	{ CCI_REG8(0x69), 0x04 },
	{ CCI_REG8(0x6a), 0x00 },
	{ CCI_REG8(0x6b), 0x3e },
	{ CCI_REG8(0x70), 0x00 },
	{ CCI_REG8(0x71), 0xbe },
	{ CCI_REG8(0x72), 0x06 },
	{ CCI_REG8(0x73), 0xfa },
	{ CCI_REG8(0x74), 0x00 },
	{ CCI_REG8(0x75), 0xc8 },
	{ CCI_REG8(0x76), 0x00 },
	{ CCI_REG8(0x77), 0xe4 },
	{ CCI_REG8(0x78), 0x00 },
	{ CCI_REG8(0x79), 0xc8 },
	{ CCI_REG8(0x7a), 0x00 },
	{ CCI_REG8(0x7b), 0xe4 },
	{ CCI_REG8(0x7c), 0x06 },
	{ CCI_REG8(0x7d), 0xf8 },
	{ CCI_REG8(0x7e), 0x06 },
	{ CCI_REG8(0x7f), 0xfc },
	{ CCI_REG8(0x80), 0x02 },
	{ CCI_REG8(0x81), 0xe4 },
	{ CCI_REG8(0x82), 0x03 },
	{ CCI_REG8(0x83), 0x2e },
	{ CCI_REG8(0x84), 0x02 },
	{ CCI_REG8(0x85), 0xe4 },
	{ CCI_REG8(0x86), 0x03 },
	{ CCI_REG8(0x87), 0x2e },
	{ CCI_REG8(0x88), 0x06 },
	{ CCI_REG8(0x89), 0xf8 },
	{ CCI_REG8(0x8a), 0x06 },
	{ CCI_REG8(0x8b), 0xfc },
	{ CCI_REG8(0x90), 0x00 },
	{ CCI_REG8(0x91), 0xbe },
	{ CCI_REG8(0x92), 0x06 },
	{ CCI_REG8(0x93), 0xf6 },
	{ CCI_REG8(0x94), 0x00 },
	{ CCI_REG8(0x95), 0xbe },
	{ CCI_REG8(0x96), 0x06 },
	{ CCI_REG8(0x97), 0xf6 },
	{ CCI_REG8(0x98), 0x06 },
	{ CCI_REG8(0x99), 0xf6 },
	{ CCI_REG8(0x9a), 0x00 },
	{ CCI_REG8(0x9b), 0xbe },
	{ CCI_REG8(0x9c), 0x06 },
	{ CCI_REG8(0x9d), 0xf6 },
	{ CCI_REG8(0x9e), 0x00 },
	{ CCI_REG8(0x9f), 0xbe },
	{ CCI_REG8(0xa0), 0x00 },
	{ CCI_REG8(0xa1), 0x06 },
	{ CCI_REG8(0xa2), 0x00 },
	{ CCI_REG8(0xa3), 0x16 },
	{ CCI_REG8(0xa4), 0x00 },
	{ CCI_REG8(0xa5), 0x06 },
	{ CCI_REG8(0xa6), 0x00 },
	{ CCI_REG8(0xa7), 0x16 },
	{ CCI_REG8(0xa8), 0x00 },
	{ CCI_REG8(0xa9), 0xc0 },
	{ CCI_REG8(0xaa), 0x00 },
	{ CCI_REG8(0xab), 0xd0 },
	{ CCI_REG8(0xac), 0x00 },
	{ CCI_REG8(0xad), 0xc0 },
	{ CCI_REG8(0xae), 0x00 },
	{ CCI_REG8(0xaf), 0xd0 },
	{ CCI_REG8(0xb0), 0x00 },
	{ CCI_REG8(0xb1), 0xe6 },
	{ CCI_REG8(0xb2), 0x06 },
	{ CCI_REG8(0xb3), 0xfa },
	{ CCI_REG8(0xb4), 0x00 },
	{ CCI_REG8(0xb5), 0xe6 },
	{ CCI_REG8(0xb6), 0x06 },
	{ CCI_REG8(0xb7), 0xfa },
	{ CCI_REG8(0xe0), 0x00 },
	{ CCI_REG8(0xe1), 0xbe },
	{ CCI_REG8(0xe2), 0x02 },
	{ CCI_REG8(0xe3), 0xe0 },
	{ CCI_REG8(0xe4), 0x03 },
	{ CCI_REG8(0xe5), 0x05 },
	{ CCI_REG8(0xe6), 0x06 },
	{ CCI_REG8(0xe7), 0xda },
	{ CCI_REG8(0xe8), 0x00 },
	{ CCI_REG8(0xe9), 0xe6 },
	{ CCI_REG8(0xea), 0x02 },
	{ CCI_REG8(0xeb), 0xe0 },
	{ CCI_REG8(0xec), 0x06 },
	{ CCI_REG8(0xed), 0xfc },
	{ CCI_REG8(0xee), 0x00 },
	{ CCI_REG8(0xef), 0x00 },
	{ CCI_REG8(0xf6), 0x00 },
	{ CCI_REG8(0xf7), 0x04 },
	{ CCI_REG8(0xf8), 0x00 },
	{ CCI_REG8(0xf9), 0x0a },
	{ CCI_REG8(0x03), 0x04 },
	{ CCI_REG8(0x10), 0x02 },
	{ CCI_REG8(0x11), 0x04 },
	{ CCI_REG8(0x12), 0x00 },
	{ CCI_REG8(0x13), 0x00 },
	{ CCI_REG8(0x14), 0x02 },
	{ CCI_REG8(0x1a), 0x00 },
	{ CCI_REG8(0x1b), 0x30 },
	{ CCI_REG8(0x1c), 0x00 },
	{ CCI_REG8(0x1d), 0xc0 },
	{ CCI_REG8(0x1e), 0x44 },
	{ CCI_REG8(0x20), 0x00 },
	{ CCI_REG8(0x21), 0x38 },
	{ CCI_REG8(0x22), 0x00 },
	{ CCI_REG8(0x23), 0x70 },
	{ CCI_REG8(0x24), 0x00 },
	{ CCI_REG8(0x25), 0xa8 },
	{ CCI_REG8(0x26), 0x00 },
	{ CCI_REG8(0x27), 0xc5 },
	{ CCI_REG8(0x28), 0x01 },
	{ CCI_REG8(0x29), 0x8a },
	{ CCI_REG8(0x2a), 0x02 },
	{ CCI_REG8(0x2b), 0x4f },
	{ CCI_REG8(0x30), 0x01 },
	{ CCI_REG8(0x31), 0x3c },
	{ CCI_REG8(0x32), 0x01 },
	{ CCI_REG8(0x33), 0x3c },
	{ CCI_REG8(0x34), 0x01 },
	{ CCI_REG8(0x35), 0x34 },
	{ CCI_REG8(0x36), 0x01 },
	{ CCI_REG8(0x37), 0x02 },
	{ CCI_REG8(0x38), 0x01 },
	{ CCI_REG8(0x39), 0x02 },
	{ CCI_REG8(0x3a), 0x01 },
	{ CCI_REG8(0x3b), 0x02 },
	{ CCI_REG8(0x40), 0x01 },
	{ CCI_REG8(0x41), 0x3e },
	{ CCI_REG8(0x42), 0x01 },
	{ CCI_REG8(0x43), 0x3e },
	{ CCI_REG8(0x44), 0x01 },
	{ CCI_REG8(0x45), 0x3e },
	{ CCI_REG8(0x46), 0x01 },
	{ CCI_REG8(0x47), 0x02 },
	{ CCI_REG8(0x48), 0x01 },
	{ CCI_REG8(0x49), 0x02 },
	{ CCI_REG8(0x4a), 0x01 },
	{ CCI_REG8(0x4b), 0x02 },
	{ CCI_REG8(0x50), 0x00 },
	{ CCI_REG8(0x58), 0x00 },
	{ CCI_REG8(0x59), 0xc0 },
	{ CCI_REG8(0x5a), 0x06 },
	{ CCI_REG8(0x5b), 0xfa },
	{ CCI_REG8(0x5c), 0x00 },
	{ CCI_REG8(0x5d), 0xc0 },
	{ CCI_REG8(0x5e), 0x06 },
	{ CCI_REG8(0x5f), 0xfa },
	{ CCI_REG8(0x60), 0x00 },
	{ CCI_REG8(0x61), 0x60 },
	{ CCI_REG8(0x62), 0x00 },
	{ CCI_REG8(0x63), 0x40 },
	{ CCI_REG8(0x64), 0x00 },
	{ CCI_REG8(0x65), 0x60 },
	{ CCI_REG8(0x66), 0x00 },
	{ CCI_REG8(0x67), 0x40 },
	{ CCI_REG8(0x68), 0x00 },
	{ CCI_REG8(0x69), 0x60 },
	{ CCI_REG8(0x6a), 0x00 },
	{ CCI_REG8(0x6b), 0x40 },
	{ CCI_REG8(0x70), 0x18 },
	{ CCI_REG8(0x71), 0x20 },
	{ CCI_REG8(0x72), 0x20 },
	{ CCI_REG8(0x73), 0x00 },
	{ CCI_REG8(0x80), 0x6f },
	{ CCI_REG8(0x81), 0x00 },
	{ CCI_REG8(0x82), 0x2f },
	{ CCI_REG8(0x83), 0x00 },
	{ CCI_REG8(0x84), 0x13 },
	{ CCI_REG8(0x85), 0x01 },
	{ CCI_REG8(0x86), 0x00 },
	{ CCI_REG8(0x87), 0x00 },
	{ CCI_REG8(0x90), 0x03 },
	{ CCI_REG8(0x91), 0x06 },
	{ CCI_REG8(0x92), 0x06 },
	{ CCI_REG8(0x93), 0x06 },
	{ CCI_REG8(0x94), 0x06 },
	{ CCI_REG8(0x95), 0x00 },
	{ CCI_REG8(0x96), 0x40 },
	{ CCI_REG8(0x97), 0x50 },
	{ CCI_REG8(0x98), 0x70 },
	{ CCI_REG8(0xa0), 0x06 },
	{ CCI_REG8(0xa1), 0xf6 },
	{ CCI_REG8(0xa2), 0x06 },
	{ CCI_REG8(0xa3), 0xf6 },
	{ CCI_REG8(0xa4), 0x06 },
	{ CCI_REG8(0xa5), 0xf6 },
	{ CCI_REG8(0xa6), 0x01 },
	{ CCI_REG8(0xa7), 0x02 },
	{ CCI_REG8(0xa8), 0x01 },
	{ CCI_REG8(0xa9), 0x02 },
	{ CCI_REG8(0xaa), 0x01 },
	{ CCI_REG8(0xab), 0x02 },
	{ CCI_REG8(0xb0), 0x04 },
	{ CCI_REG8(0xb1), 0x04 },
	{ CCI_REG8(0xb2), 0x00 },
	{ CCI_REG8(0xb3), 0x04 },
	{ CCI_REG8(0xb4), 0x00 },
	{ CCI_REG8(0xc0), 0x00 },
	{ CCI_REG8(0xc1), 0x48 },
	{ CCI_REG8(0xc2), 0x00 },
	{ CCI_REG8(0xc3), 0x6e },
	{ CCI_REG8(0xc4), 0x00 },
	{ CCI_REG8(0xc5), 0x4d },
	{ CCI_REG8(0xc6), 0x00 },
	{ CCI_REG8(0xc7), 0x6c },
	{ CCI_REG8(0xc8), 0x00 },
	{ CCI_REG8(0xc9), 0x4f },
	{ CCI_REG8(0xca), 0x00 },
	{ CCI_REG8(0xcb), 0x6a },
	{ CCI_REG8(0xcc), 0x00 },
	{ CCI_REG8(0xcd), 0x50 },
	{ CCI_REG8(0xce), 0x00 },
	{ CCI_REG8(0xcf), 0x68 },
	{ CCI_REG8(0xd0), 0x07 },
	{ CCI_REG8(0xd1), 0x00 },
	{ CCI_REG8(0xd2), 0x03 },
	{ CCI_REG8(0xd3), 0x03 },
	{ CCI_REG8(0xe0), 0x00 },
	{ CCI_REG8(0xe1), 0x10 },
	{ CCI_REG8(0xe2), 0x67 },
	{ CCI_REG8(0xe3), 0x00 },
	{ CCI_REG8(0x03), 0x08 },
	{ CCI_REG8(0x10), 0x07 },
	{ CCI_REG8(0x20), 0x01 },
	{ CCI_REG8(0x21), 0x00 },
	{ CCI_REG8(0x22), 0x01 },
	{ CCI_REG8(0x23), 0x00 },
	{ CCI_REG8(0x24), 0x01 },
	{ CCI_REG8(0x25), 0x00 },
	{ CCI_REG8(0x26), 0x01 },
	{ CCI_REG8(0x27), 0x00 },
	{ CCI_REG8(0x28), 0x01 },
	{ CCI_REG8(0x29), 0x00 },
	{ CCI_REG8(0x2a), 0x01 },
	{ CCI_REG8(0x2b), 0x00 },
	{ CCI_REG8(0x2c), 0x01 },
	{ CCI_REG8(0x2d), 0x00 },
	{ CCI_REG8(0x2e), 0x01 },
	{ CCI_REG8(0x2f), 0x00 },
	{ CCI_REG8(0x30), 0x03 },
	{ CCI_REG8(0x31), 0xff },
	{ CCI_REG8(0x32), 0x03 },
	{ CCI_REG8(0x33), 0xff },
	{ CCI_REG8(0x34), 0x03 },
	{ CCI_REG8(0x35), 0xff },
	{ CCI_REG8(0x36), 0x03 },
	{ CCI_REG8(0x37), 0xff },
	{ CCI_REG8(0x40), 0x07 },
	{ CCI_REG8(0x50), 0x01 },
	{ CCI_REG8(0x51), 0x00 },
	{ CCI_REG8(0x52), 0x01 },
	{ CCI_REG8(0x53), 0x00 },
	{ CCI_REG8(0x54), 0x0f },
	{ CCI_REG8(0x55), 0xff },
	{ CCI_REG8(0x03), 0x10 },
	{ CCI_REG8(0x10), 0x00 },
	{ CCI_REG8(0x11), 0x00 },
	{ CCI_REG8(0x03), 0x20 },
	{ CCI_REG8(0x10), 0x00 },
	{ CCI_REG8(0x11), 0x05 },
	{ CCI_REG8(0x12), 0x03 },
	{ CCI_REG8(0x22), 0x05 },
	{ CCI_REG8(0x23), 0x15 },
	{ CCI_REG8(0x26), 0xff },
	{ CCI_REG8(0x27), 0xff },
	{ CCI_REG8(0x29), 0x00 },
	{ CCI_REG8(0x2a), 0x02 },
	{ CCI_REG8(0x2b), 0x00 },
	{ CCI_REG8(0x2c), 0x04 },
	{ CCI_REG8(0x30), 0x00 },
	{ CCI_REG8(0x31), 0x04 },
	{ CCI_REG8(0x40), 0x09 },
	{ CCI_REG8(0x41), 0x1e },
	{ CCI_REG8(0x42), 0x60 },
	{ CCI_REG8(0x52), 0x0f },
	{ CCI_REG8(0x53), 0xf3 },
	{ CCI_REG8(0x60), 0xef },
	{ CCI_REG8(0x61), 0x00 },
	{ CCI_REG8(0x64), 0x0f },
	{ CCI_REG8(0x65), 0x00 },
	{ CCI_REG8(0x03), 0x05 },
	{ CCI_REG8(0x39), 0x52 },
	{ CCI_REG8(0x4c), 0x20 },
	{ CCI_REG8(0x4d), 0x00 },
	{ CCI_REG8(0x4e), 0x40 },
	{ CCI_REG8(0x4f), 0x00 },
	{ CCI_REG8(0x11), 0x00 },
	{ CCI_REG8(0x14), 0x01 },
	{ CCI_REG8(0x16), 0x12 },
	{ CCI_REG8(0x18), 0x80 },
	{ CCI_REG8(0x19), 0x00 },
	{ CCI_REG8(0x1a), 0xf0 },
	{ CCI_REG8(0x24), 0x2b },
	{ CCI_REG8(0x32), 0x1e },
	{ CCI_REG8(0x33), 0x0f },
	{ CCI_REG8(0x34), 0x06 },
	{ CCI_REG8(0x35), 0x05 },
	{ CCI_REG8(0x36), 0x01 },
	{ CCI_REG8(0x37), 0x08 },
	{ CCI_REG8(0x1c), 0x01 },
	{ CCI_REG8(0x1d), 0x09 },
	{ CCI_REG8(0x1e), 0x0f },
	{ CCI_REG8(0x1f), 0x0b },
	{ CCI_REG8(0x30), 0x07 },
	{ CCI_REG8(0x31), 0xd0 },
	{ CCI_REG8(0x10), 0x1d },
};
/* 1600x1200 RAW10, 1 lane, 30.03 fps (line 1800, frame 1444, pixel clock 78.08 MHz), 73 writes */
static const struct reg_sequence hi259_1600x1200_regs[] = {
	{ CCI_REG8(0x03), 0x00 },
	{ CCI_REG8(0x07), 0x05 },
	{ CCI_REG8(0x08), 0x7a },
	{ CCI_REG8(0x09), 0x13 },
	{ CCI_REG8(0x07), 0x85 },
	{ CCI_REG8(0x07), 0x85 },
	{ CCI_REG8(0x07), 0x85 },
	{ CCI_REG8(0x0a), 0x80 },
	{ CCI_REG8(0x07), 0xc5 },
	{ CCI_REG8(0x03), 0x00 },
	{ CCI_REG8(0x10), 0x00 },
	{ CCI_REG8(0x20), 0x00 },
	{ CCI_REG8(0x21), 0x10 },
	{ CCI_REG8(0x22), 0x00 },
	{ CCI_REG8(0x23), 0x10 },
	{ CCI_REG8(0x24), 0x00 },
	{ CCI_REG8(0x25), 0x10 },
	{ CCI_REG8(0x26), 0x00 },
	{ CCI_REG8(0x27), 0x10 },
	{ CCI_REG8(0x28), 0x04 },
	{ CCI_REG8(0x29), 0xb0 },
	{ CCI_REG8(0x2a), 0x06 },
	{ CCI_REG8(0x2b), 0x40 },
	{ CCI_REG8(0x30), 0x00 },
	{ CCI_REG8(0x31), 0x10 },
	{ CCI_REG8(0x32), 0x00 },
	{ CCI_REG8(0x33), 0x10 },
	{ CCI_REG8(0x34), 0x00 },
	{ CCI_REG8(0x35), 0x10 },
	{ CCI_REG8(0x36), 0x00 },
	{ CCI_REG8(0x37), 0x10 },
	{ CCI_REG8(0x38), 0x04 },
	{ CCI_REG8(0x39), 0xb0 },
	{ CCI_REG8(0x3a), 0x06 },
	{ CCI_REG8(0x3b), 0x40 },
	{ CCI_REG8(0x4e), 0x05 },
	{ CCI_REG8(0x4f), 0xa4 },
	{ CCI_REG8(0x80), 0x00 },
	{ CCI_REG8(0x81), 0x00 },
	{ CCI_REG8(0x82), 0x00 },
	{ CCI_REG8(0x83), 0x00 },
	{ CCI_REG8(0x84), 0x06 },
	{ CCI_REG8(0x85), 0x60 },
	{ CCI_REG8(0x86), 0x04 },
	{ CCI_REG8(0x87), 0xcc },
	{ CCI_REG8(0x88), 0x00 },
	{ CCI_REG8(0x89), 0x00 },
	{ CCI_REG8(0x8a), 0x04 },
	{ CCI_REG8(0x8b), 0xcc },
	{ CCI_REG8(0x03), 0x02 },
	{ CCI_REG8(0x3a), 0x80 },
	{ CCI_REG8(0x03), 0x04 },
	{ CCI_REG8(0xb0), 0x04 },
	{ CCI_REG8(0xb1), 0x04 },
	{ CCI_REG8(0xb2), 0x00 },
	{ CCI_REG8(0xb3), 0x04 },
	{ CCI_REG8(0xb4), 0x00 },
	{ CCI_REG8(0x03), 0x10 },
	{ CCI_REG8(0x10), 0x00 },
	{ CCI_REG8(0x03), 0x05 },
	{ CCI_REG8(0x32), 0x1e },
	{ CCI_REG8(0x33), 0x0f },
	{ CCI_REG8(0x34), 0x06 },
	{ CCI_REG8(0x35), 0x05 },
	{ CCI_REG8(0x36), 0x01 },
	{ CCI_REG8(0x37), 0x08 },
	{ CCI_REG8(0x1a), 0xf0 },
	{ CCI_REG8(0x1c), 0x01 },
	{ CCI_REG8(0x1d), 0x09 },
	{ CCI_REG8(0x1e), 0x0f },
	{ CCI_REG8(0x1f), 0x0b },
	{ CCI_REG8(0x30), 0x07 },
	{ CCI_REG8(0x31), 0xd0 },
};
static int hi259_write_page(struct hi259 *hi259, u8 page, u32 reg, u64 val, int *err)
{
	cci_write(hi259->regmap, HI259_REG_PAGE, page, err);
	return cci_write(hi259->regmap, reg, val, err);
}

static int hi259_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hi259 *hi259 = container_of(ctrl->handler, struct hi259, ctrls);
	int ret = 0;
	u32 code;

	/* registers are only written while powered; enable_streams applies the values */
	if (!pm_runtime_get_if_active(hi259->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		hi259_write_page(hi259, 0x00, HI259_REG_GROUP_HOLD, 0x01, &ret);
		hi259_write_page(hi259, 0x20, HI259_REG_EXPOSURE_H, ctrl->val >> 8, &ret);
		cci_write(hi259->regmap, HI259_REG_EXPOSURE_L, ctrl->val & 0xff, &ret);
		hi259_write_page(hi259, 0x00, HI259_REG_GROUP_HOLD, 0x00, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		/* gain2reg(): code = (256 * 64 / gain - 17) * 2, 478 at 1x .. 30 at 8x .. 2 at 14x */
		code = (256 * 64 / ctrl->val - 17) * 2;
		hi259_write_page(hi259, 0x00, HI259_REG_GROUP_HOLD, 0x01, &ret);
		hi259_write_page(hi259, 0x20, HI259_REG_GAIN_H, code >> 1, &ret);
		cci_write(hi259->regmap, HI259_REG_GAIN_L, code & 1, &ret);
		hi259_write_page(hi259, 0x00, HI259_REG_GROUP_HOLD, 0x00, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		hi259_write_page(hi259, 0x00, HI259_REG_TEST_PATTERN, ctrl->val ? 0x04 : 0x00, &ret);
		break;
	default:
		break;
	}

	pm_runtime_put(hi259->dev);
	return ret;
}

static const struct v4l2_ctrl_ops hi259_ctrl_ops = {
	.s_ctrl = hi259_set_ctrl,
};

static int hi259_init_controls(struct hi259 *hi259)
{
	struct v4l2_ctrl_handler *hdl = &hi259->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(hdl, 10);

	ctrl = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ, 0, 0, hi259_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE, HI259_PIXEL_RATE, HI259_PIXEL_RATE, 1,
			  HI259_PIXEL_RATE);
	/* the frame length is fixed: the vendor drivers never rewrite it */
	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_VBLANK, HI259_FRAME_LENGTH - HI259_HEIGHT,
				 HI259_FRAME_LENGTH - HI259_HEIGHT, 1, HI259_FRAME_LENGTH - HI259_HEIGHT);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_HBLANK, HI259_LINE_LENGTH - HI259_WIDTH,
				 HI259_LINE_LENGTH - HI259_WIDTH, 1, HI259_LINE_LENGTH - HI259_WIDTH);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	hi259->exposure = v4l2_ctrl_new_std(hdl, &hi259_ctrl_ops, V4L2_CID_EXPOSURE, 1,
					    HI259_FRAME_LENGTH - HI259_EXPOSURE_MARGIN, 1, 1000);
	v4l2_ctrl_new_std(hdl, &hi259_ctrl_ops, V4L2_CID_ANALOGUE_GAIN, HI259_GAIN_MIN,
			  HI259_GAIN_MAX, 1, HI259_GAIN_MIN);
	v4l2_ctrl_new_std_menu_items(hdl, &hi259_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(hi259_test_pattern_menu) - 1, 0, 0,
				     hi259_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(hi259->dev, &props);
	if (ret)
		return ret;
	v4l2_ctrl_new_fwnode_properties(hdl, &hi259_ctrl_ops, &props);

	if (hdl->error)
		return hdl->error;

	hi259->sd.ctrl_handler = hdl;
	return 0;
}

static void hi259_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	memset(fmt, 0, sizeof(*fmt));
	fmt->width = HI259_WIDTH;
	fmt->height = HI259_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;	/* with the default orientation, 0x11 = 0x80 */
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int hi259_enable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				u32 pad, u64 streams_mask)
{
	struct hi259 *hi259 = to_hi259(sd);
	int ret;

	ret = pm_runtime_resume_and_get(hi259->dev);
	if (ret)
		return ret;

	ret = regmap_multi_reg_write(hi259->regmap, hi259_init_regs, ARRAY_SIZE(hi259_init_regs));
	if (!ret)
		ret = regmap_multi_reg_write(hi259->regmap, hi259_1600x1200_regs,
					     ARRAY_SIZE(hi259_1600x1200_regs));
	if (!ret)
		ret = __v4l2_ctrl_handler_setup(&hi259->ctrls);
	if (!ret)
		hi259_write_page(hi259, 0x00, HI259_REG_MODE, 0x00, &ret);
	if (ret) {
		dev_err(hi259->dev, "failed to start streaming: %d\n", ret);
		pm_runtime_put(hi259->dev);
	}
	return ret;
}

static int hi259_disable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct hi259 *hi259 = to_hi259(sd);
	int ret = 0;

	hi259_write_page(hi259, 0x00, HI259_REG_MODE, 0x01, &ret);
	pm_runtime_put(hi259->dev);
	return ret;
}

static int hi259_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int hi259_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				 struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = HI259_WIDTH;
	fse->min_height = fse->max_height = HI259_HEIGHT;
	return 0;
}

static int hi259_get_selection(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			       struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = HI259_WIDTH;
		sel->r.height = HI259_HEIGHT;
		return 0;
	}
	return -EINVAL;
}

static const struct v4l2_subdev_video_ops hi259_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops hi259_pad_ops = {
	.enum_mbus_code = hi259_enum_mbus_code,
	.enum_frame_size = hi259_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = v4l2_subdev_get_fmt,	/* one fixed mode */
	.get_selection = hi259_get_selection,
	.enable_streams = hi259_enable_streams,
	.disable_streams = hi259_disable_streams,
};

static const struct v4l2_subdev_ops hi259_ops = {
	.video = &hi259_video_ops,
	.pad = &hi259_pad_ops,
};

static int hi259_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	hi259_fill_format(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_internal_ops hi259_internal_ops = {
	.init_state = hi259_init_state,
};

/* Power-up per the CamX module: RESET=0, CHIP_EN=0, supplies, MCLK, CHIP_EN=1 (5 ms), RESET=1 (10 ms) */
static int hi259_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hi259 *hi259 = to_hi259(sd);
	int ret;

	gpiod_set_value_cansleep(hi259->reset_gpio, 1);
	gpiod_set_value_cansleep(hi259->enable_gpio, 0);
	fsleep(3000);

	ret = regulator_bulk_enable(ARRAY_SIZE(hi259->supplies), hi259->supplies);
	if (ret)
		return ret;

	ret = clk_prepare_enable(hi259->clk);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(hi259->supplies), hi259->supplies);
		return ret;
	}
	fsleep(2000);

	gpiod_set_value_cansleep(hi259->enable_gpio, 1);
	fsleep(5000);
	gpiod_set_value_cansleep(hi259->reset_gpio, 0);
	fsleep(10000);
	return 0;
}

static int hi259_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hi259 *hi259 = to_hi259(sd);

	gpiod_set_value_cansleep(hi259->reset_gpio, 1);
	fsleep(1000);
	clk_disable_unprepare(hi259->clk);
	gpiod_set_value_cansleep(hi259->enable_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(hi259->supplies), hi259->supplies);
	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(hi259_pm_ops, hi259_power_off, hi259_power_on, NULL);

static int hi259_identify(struct hi259 *hi259)
{
	u64 id = 0;
	int ret = 0;

	cci_write(hi259->regmap, HI259_REG_PAGE, 0x00, &ret);
	cci_read(hi259->regmap, HI259_REG_CHIP_ID, &id, &ret);
	if (ret)
		return dev_err_probe(hi259->dev, ret, "failed to read the chip id\n");
	if (id != HI259_CHIP_ID)
		return dev_err_probe(hi259->dev, -ENODEV, "unexpected chip id 0x%02llx (want 0x%02x)\n",
				     id, HI259_CHIP_ID);
	return 0;
}

static int hi259_check_hwcfg(struct hi259 *hi259)
{
	struct v4l2_fwnode_endpoint bus_cfg = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	unsigned long link_freq_bitmap;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(hi259->dev), 0, 0, 0);
	if (!ep)
		return dev_err_probe(hi259->dev, -EPROBE_DEFER, "waiting for the endpoint\n");
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(hi259->dev, ret, "parsing the endpoint failed\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != 1)
		ret = dev_err_probe(hi259->dev, -EINVAL, "%u data lanes, only 1 is supported\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
	if (!ret)
		ret = v4l2_link_freq_to_bitmap(hi259->dev, bus_cfg.link_frequencies,
					       bus_cfg.nr_of_link_frequencies, hi259_link_freqs,
					       ARRAY_SIZE(hi259_link_freqs), &link_freq_bitmap);
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int hi259_probe(struct i2c_client *client)
{
	struct hi259 *hi259;
	unsigned int i;
	int ret;

	hi259 = devm_kzalloc(&client->dev, sizeof(*hi259), GFP_KERNEL);
	if (!hi259)
		return -ENOMEM;
	hi259->dev = &client->dev;

	ret = hi259_check_hwcfg(hi259);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&hi259->sd, client, &hi259_ops);

	hi259->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(hi259->regmap))
		return PTR_ERR(hi259->regmap);

	hi259->clk = devm_v4l2_sensor_clk_get(hi259->dev, NULL);
	if (IS_ERR(hi259->clk))
		return dev_err_probe(hi259->dev, PTR_ERR(hi259->clk), "getting the clock\n");
	if (clk_get_rate(hi259->clk) != HI259_MCLK)
		dev_warn(hi259->dev, "clock is %lu Hz, the tables expect %u\n",
			 clk_get_rate(hi259->clk), HI259_MCLK);

	hi259->reset_gpio = devm_gpiod_get(hi259->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(hi259->reset_gpio))
		return dev_err_probe(hi259->dev, PTR_ERR(hi259->reset_gpio), "getting the reset gpio\n");
	hi259->enable_gpio = devm_gpiod_get_optional(hi259->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(hi259->enable_gpio))
		return dev_err_probe(hi259->dev, PTR_ERR(hi259->enable_gpio), "getting the enable gpio\n");

	for (i = 0; i < ARRAY_SIZE(hi259_supply_names); i++)
		hi259->supplies[i].supply = hi259_supply_names[i];
	ret = devm_regulator_bulk_get(hi259->dev, ARRAY_SIZE(hi259->supplies), hi259->supplies);
	if (ret)
		return dev_err_probe(hi259->dev, ret, "getting the supplies\n");

	ret = hi259_power_on(hi259->dev);
	if (ret)
		return ret;

	ret = hi259_identify(hi259);
	if (ret)
		goto err_power_off;

	ret = hi259_init_controls(hi259);
	if (ret)
		goto err_ctrls;

	hi259->sd.internal_ops = &hi259_internal_ops;
	hi259->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	hi259->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	hi259->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&hi259->sd.entity, 1, &hi259->pad);
	if (ret)
		goto err_ctrls;

	hi259->sd.state_lock = hi259->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&hi259->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(hi259->dev);
	pm_runtime_enable(hi259->dev);
	pm_runtime_set_autosuspend_delay(hi259->dev, 1000);
	pm_runtime_use_autosuspend(hi259->dev);

	ret = v4l2_async_register_subdev_sensor(&hi259->sd);
	if (ret)
		goto err_pm;

	pm_runtime_idle(hi259->dev);
	dev_info(hi259->dev, "Hi-259 found (chip id 0x%02x)\n", HI259_CHIP_ID);
	return 0;

err_pm:
	pm_runtime_disable(hi259->dev);
	pm_runtime_set_suspended(hi259->dev);
	v4l2_subdev_cleanup(&hi259->sd);
err_entity:
	media_entity_cleanup(&hi259->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&hi259->ctrls);
err_power_off:
	hi259_power_off(hi259->dev);
	return ret;
}

static void hi259_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct hi259 *hi259 = to_hi259(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&hi259->ctrls);
	pm_runtime_disable(hi259->dev);
	if (!pm_runtime_status_suspended(hi259->dev)) {
		hi259_power_off(hi259->dev);
		pm_runtime_set_suspended(hi259->dev);
	}
}

static const struct of_device_id hi259_of_match[] = {
	{ .compatible = "hynix,hi259" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi259_of_match);

static struct i2c_driver hi259_i2c_driver = {
	.driver = {
		.name = "hi259",
		.of_match_table = hi259_of_match,
		.pm = pm_ptr(&hi259_pm_ops),
	},
	.probe = hi259_probe,
	.remove = hi259_remove,
};
module_i2c_driver(hi259_i2c_driver);

MODULE_DESCRIPTION("SK Hynix Hi-259 camera sensor driver");
MODULE_LICENSE("GPL");
