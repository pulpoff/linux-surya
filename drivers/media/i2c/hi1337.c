// SPDX-License-Identifier: GPL-2.0
/*
 * SK Hynix Hi-1337 13MP camera sensor (the ultrawide camera of the Xiaomi POCO X3 NFC "surya").
 *
 * No public datasheet. Register tables decoded from the phone's CamX sensor modules
 * (com.qti.sensormodule.j20c_{aac,sunny}_hi1337_ultra.bin, identical settings). The control
 * registers are the Hi-847's (same family): chip id 0x0716, exposure 0x020a, frame length 0x020e,
 * line length 0x0206, analogue gain 0x0212 (1 + value/16), streaming 0x0b00, group hold 0x0208.
 * 16-bit addresses and values.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mux/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define HI1337_REG_CHIP_ID		CCI_REG16(0x0716)
#define HI1337_CHIP_ID			0x1337
#define HI1337_REG_MODE_SELECT		CCI_REG16(0x0b00)	/* 0x0100 streaming, 0 standby */
#define HI1337_REG_GROUP_HOLD		CCI_REG16(0x0208)	/* 0x0100 hold, 0 release */
#define HI1337_REG_EXPOSURE		CCI_REG16(0x020a)	/* in lines */
#define HI1337_REG_FLL			CCI_REG16(0x020e)	/* frame length in lines */
#define HI1337_REG_ANALOG_GAIN		CCI_REG16(0x0212)	/* gain = 1 + value / 16 */
#define HI1337_REG_ISP			CCI_REG16(0x0b04)
#define HI1337_ISP_TPG_EN		BIT(0)
#define HI1337_REG_TEST_PATTERN		CCI_REG16(0x0c0a)

#define HI1337_MCLK			19200000
#define HI1337_DATA_LANES		4
#define HI1337_FLL_MAX			0xffff
#define HI1337_EXPOSURE_MIN		4
#define HI1337_EXPOSURE_MARGIN		4
#define HI1337_GAIN_MIN			0
#define HI1337_GAIN_MAX			240
#define HI1337_NATIVE_WIDTH		4208
#define HI1337_NATIVE_HEIGHT		3120

/* CSIPHY1 board mux state for this sensor (CamX: CAM_SEL high) */
#define HI1337_MUX_STATE		1

static const char * const hi1337_supply_names[] = {
	"dovdd",	/* 1.8 V I/O */
	"avdd",		/* 2.8 V analog */
	"dvdd",		/* 1.1 V core */
};

static const char * const hi1337_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Colour Bars",
	"Fade To Grey Colour Bars",
	"PN9",
};

/* global init, 1457 writes (CamX module j20c_aac_hi1337_ultra) */
static const struct cci_reg_sequence hi1337_init_regs[] = {
	{ CCI_REG16(0x0790), 0x0100 },
	{ CCI_REG16(0x2000), 0x0000 },
	{ CCI_REG16(0x2002), 0x0058 },
	{ CCI_REG16(0x2006), 0x40b2 },
	{ CCI_REG16(0x2008), 0xb05c },
	{ CCI_REG16(0x200a), 0x8446 },
	{ CCI_REG16(0x200c), 0x40b2 },
	{ CCI_REG16(0x200e), 0xb082 },
	{ CCI_REG16(0x2010), 0x8450 },
	{ CCI_REG16(0x2012), 0x40b2 },
	{ CCI_REG16(0x2014), 0xb0a0 },
	{ CCI_REG16(0x2016), 0x84c6 },
	{ CCI_REG16(0x2018), 0x40b2 },
	{ CCI_REG16(0x201a), 0xb0ec },
	{ CCI_REG16(0x201c), 0x8470 },
	{ CCI_REG16(0x201e), 0x40b2 },
	{ CCI_REG16(0x2020), 0xb112 },
	{ CCI_REG16(0x2022), 0x84b4 },
	{ CCI_REG16(0x2024), 0x40b2 },
	{ CCI_REG16(0x2026), 0xb14e },
	{ CCI_REG16(0x2028), 0x84b0 },
	{ CCI_REG16(0x202a), 0x40b2 },
	{ CCI_REG16(0x202c), 0xb17c },
	{ CCI_REG16(0x202e), 0x84b8 },
	{ CCI_REG16(0x2030), 0x40b2 },
	{ CCI_REG16(0x2032), 0xb1b2 },
	{ CCI_REG16(0x2034), 0x847c },
	{ CCI_REG16(0x2036), 0x40b2 },
	{ CCI_REG16(0x2038), 0xb420 },
	{ CCI_REG16(0x203a), 0x8478 },
	{ CCI_REG16(0x203c), 0x40b2 },
	{ CCI_REG16(0x203e), 0xb4b4 },
	{ CCI_REG16(0x2040), 0x8476 },
	{ CCI_REG16(0x2042), 0x40b2 },
	{ CCI_REG16(0x2044), 0xb530 },
	{ CCI_REG16(0x2046), 0x847e },
	{ CCI_REG16(0x2048), 0x40b2 },
	{ CCI_REG16(0x204a), 0xb640 },
	{ CCI_REG16(0x204c), 0x843a },
	{ CCI_REG16(0x204e), 0x40b2 },
	{ CCI_REG16(0x2050), 0xb822 },
	{ CCI_REG16(0x2052), 0x845c },
	{ CCI_REG16(0x2054), 0x40b2 },
	{ CCI_REG16(0x2056), 0xb852 },
	{ CCI_REG16(0x2058), 0x845e },
	{ CCI_REG16(0x205a), 0x4130 },
	{ CCI_REG16(0x205c), 0x1292 },
	{ CCI_REG16(0x205e), 0xd016 },
	{ CCI_REG16(0x2060), 0xb3d2 },
	{ CCI_REG16(0x2062), 0x0b00 },
	{ CCI_REG16(0x2064), 0x2002 },
	{ CCI_REG16(0x2066), 0xd2e2 },
	{ CCI_REG16(0x2068), 0x0381 },
	{ CCI_REG16(0x206a), 0x93c2 },
	{ CCI_REG16(0x206c), 0x0263 },
	{ CCI_REG16(0x206e), 0x2001 },
	{ CCI_REG16(0x2070), 0x4130 },
	{ CCI_REG16(0x2072), 0x422d },
	{ CCI_REG16(0x2074), 0x403e },
	{ CCI_REG16(0x2076), 0x879e },
	{ CCI_REG16(0x2078), 0x403f },
	{ CCI_REG16(0x207a), 0x192a },
	{ CCI_REG16(0x207c), 0x1292 },
	{ CCI_REG16(0x207e), 0x843e },
	{ CCI_REG16(0x2080), 0x3ff7 },
	{ CCI_REG16(0x2082), 0xb3d2 },
	{ CCI_REG16(0x2084), 0x0267 },
	{ CCI_REG16(0x2086), 0x2403 },
	{ CCI_REG16(0x2088), 0xd0f2 },
	{ CCI_REG16(0x208a), 0x0040 },
	{ CCI_REG16(0x208c), 0x0381 },
	{ CCI_REG16(0x208e), 0x90f2 },
	{ CCI_REG16(0x2090), 0x0010 },
	{ CCI_REG16(0x2092), 0x0260 },
	{ CCI_REG16(0x2094), 0x2002 },
	{ CCI_REG16(0x2096), 0x1292 },
	{ CCI_REG16(0x2098), 0x84bc },
	{ CCI_REG16(0x209a), 0x1292 },
	{ CCI_REG16(0x209c), 0xd020 },
	{ CCI_REG16(0x209e), 0x4130 },
	{ CCI_REG16(0x20a0), 0x1292 },
	{ CCI_REG16(0x20a2), 0x8470 },
	{ CCI_REG16(0x20a4), 0x1292 },
	{ CCI_REG16(0x20a6), 0x8452 },
	{ CCI_REG16(0x20a8), 0x0900 },
	{ CCI_REG16(0x20aa), 0x7118 },
	{ CCI_REG16(0x20ac), 0x1292 },
	{ CCI_REG16(0x20ae), 0x848e },
	{ CCI_REG16(0x20b0), 0x0900 },
	{ CCI_REG16(0x20b2), 0x7112 },
	{ CCI_REG16(0x20b4), 0x0800 },
	{ CCI_REG16(0x20b6), 0x7a20 },
	{ CCI_REG16(0x20b8), 0x4292 },
	{ CCI_REG16(0x20ba), 0x86ee },
	{ CCI_REG16(0x20bc), 0x7334 },
	{ CCI_REG16(0x20be), 0x0f00 },
	{ CCI_REG16(0x20c0), 0x7304 },
	{ CCI_REG16(0x20c2), 0x421f },
	{ CCI_REG16(0x20c4), 0x8620 },
	{ CCI_REG16(0x20c6), 0x1292 },
	{ CCI_REG16(0x20c8), 0x846e },
	{ CCI_REG16(0x20ca), 0x1292 },
	{ CCI_REG16(0x20cc), 0x8488 },
	{ CCI_REG16(0x20ce), 0x0b00 },
	{ CCI_REG16(0x20d0), 0x7114 },
	{ CCI_REG16(0x20d2), 0x0002 },
	{ CCI_REG16(0x20d4), 0x1292 },
	{ CCI_REG16(0x20d6), 0x848c },
	{ CCI_REG16(0x20d8), 0x1292 },
	{ CCI_REG16(0x20da), 0x8454 },
	{ CCI_REG16(0x20dc), 0x43c2 },
	{ CCI_REG16(0x20de), 0x85f6 },
	{ CCI_REG16(0x20e0), 0x4292 },
	{ CCI_REG16(0x20e2), 0x0c34 },
	{ CCI_REG16(0x20e4), 0x0202 },
	{ CCI_REG16(0x20e6), 0x1292 },
	{ CCI_REG16(0x20e8), 0x8444 },
	{ CCI_REG16(0x20ea), 0x4130 },
	{ CCI_REG16(0x20ec), 0x4392 },
	{ CCI_REG16(0x20ee), 0x7360 },
	{ CCI_REG16(0x20f0), 0xb3d2 },
	{ CCI_REG16(0x20f2), 0x0b00 },
	{ CCI_REG16(0x20f4), 0x2402 },
	{ CCI_REG16(0x20f6), 0xc2e2 },
	{ CCI_REG16(0x20f8), 0x0381 },
	{ CCI_REG16(0x20fa), 0x0900 },
	{ CCI_REG16(0x20fc), 0x732c },
	{ CCI_REG16(0x20fe), 0x4382 },
	{ CCI_REG16(0x2100), 0x7360 },
	{ CCI_REG16(0x2102), 0x422d },
	{ CCI_REG16(0x2104), 0x403e },
	{ CCI_REG16(0x2106), 0x8700 },
	{ CCI_REG16(0x2108), 0x403f },
	{ CCI_REG16(0x210a), 0x86f8 },
	{ CCI_REG16(0x210c), 0x1292 },
	{ CCI_REG16(0x210e), 0x843e },
	{ CCI_REG16(0x2110), 0x4130 },
	{ CCI_REG16(0x2112), 0x4f0c },
	{ CCI_REG16(0x2114), 0x403f },
	{ CCI_REG16(0x2116), 0x0267 },
	{ CCI_REG16(0x2118), 0xf0ff },
	{ CCI_REG16(0x211a), 0xffdf },
	{ CCI_REG16(0x211c), 0x0000 },
	{ CCI_REG16(0x211e), 0xf0ff },
	{ CCI_REG16(0x2120), 0xffef },
	{ CCI_REG16(0x2122), 0x0000 },
	{ CCI_REG16(0x2124), 0x421d },
	{ CCI_REG16(0x2126), 0x84b0 },
	{ CCI_REG16(0x2128), 0x403e },
	{ CCI_REG16(0x212a), 0x06f9 },
	{ CCI_REG16(0x212c), 0x4c0f },
	{ CCI_REG16(0x212e), 0x1292 },
	{ CCI_REG16(0x2130), 0x84ac },
	{ CCI_REG16(0x2132), 0x4f4e },
	{ CCI_REG16(0x2134), 0xb31e },
	{ CCI_REG16(0x2136), 0x2403 },
	{ CCI_REG16(0x2138), 0xd0f2 },
	{ CCI_REG16(0x213a), 0x0020 },
	{ CCI_REG16(0x213c), 0x0267 },
	{ CCI_REG16(0x213e), 0xb32e },
	{ CCI_REG16(0x2140), 0x2403 },
	{ CCI_REG16(0x2142), 0xd0f2 },
	{ CCI_REG16(0x2144), 0x0010 },
	{ CCI_REG16(0x2146), 0x0267 },
	{ CCI_REG16(0x2148), 0xc3e2 },
	{ CCI_REG16(0x214a), 0x0267 },
	{ CCI_REG16(0x214c), 0x4130 },
	{ CCI_REG16(0x214e), 0x120b },
	{ CCI_REG16(0x2150), 0x120a },
	{ CCI_REG16(0x2152), 0x403a },
	{ CCI_REG16(0x2154), 0x1140 },
	{ CCI_REG16(0x2156), 0x1292 },
	{ CCI_REG16(0x2158), 0xd080 },
	{ CCI_REG16(0x215a), 0x430b },
	{ CCI_REG16(0x215c), 0x4a0f },
	{ CCI_REG16(0x215e), 0x532a },
	{ CCI_REG16(0x2160), 0x1292 },
	{ CCI_REG16(0x2162), 0x84a4 },
	{ CCI_REG16(0x2164), 0x4f0e },
	{ CCI_REG16(0x2166), 0x430f },
	{ CCI_REG16(0x2168), 0x5e82 },
	{ CCI_REG16(0x216a), 0x870c },
	{ CCI_REG16(0x216c), 0x6f82 },
	{ CCI_REG16(0x216e), 0x870e },
	{ CCI_REG16(0x2170), 0x531b },
	{ CCI_REG16(0x2172), 0x923b },
	{ CCI_REG16(0x2174), 0x2bf3 },
	{ CCI_REG16(0x2176), 0x413a },
	{ CCI_REG16(0x2178), 0x413b },
	{ CCI_REG16(0x217a), 0x4130 },
	{ CCI_REG16(0x217c), 0xf0f2 },
	{ CCI_REG16(0x217e), 0x007f },
	{ CCI_REG16(0x2180), 0x0267 },
	{ CCI_REG16(0x2182), 0x421d },
	{ CCI_REG16(0x2184), 0x84b6 },
	{ CCI_REG16(0x2186), 0x403e },
	{ CCI_REG16(0x2188), 0x01f9 },
	{ CCI_REG16(0x218a), 0x1292 },
	{ CCI_REG16(0x218c), 0x84ac },
	{ CCI_REG16(0x218e), 0x4f4e },
	{ CCI_REG16(0x2190), 0xf35f },
	{ CCI_REG16(0x2192), 0x2403 },
	{ CCI_REG16(0x2194), 0xd0f2 },
	{ CCI_REG16(0x2196), 0xff80 },
	{ CCI_REG16(0x2198), 0x0267 },
	{ CCI_REG16(0x219a), 0xb36e },
	{ CCI_REG16(0x219c), 0x2404 },
	{ CCI_REG16(0x219e), 0xd0f2 },
	{ CCI_REG16(0x21a0), 0x0040 },
	{ CCI_REG16(0x21a2), 0x0267 },
	{ CCI_REG16(0x21a4), 0x3c03 },
	{ CCI_REG16(0x21a6), 0xf0f2 },
	{ CCI_REG16(0x21a8), 0xffbf },
	{ CCI_REG16(0x21aa), 0x0267 },
	{ CCI_REG16(0x21ac), 0xc2e2 },
	{ CCI_REG16(0x21ae), 0x0267 },
	{ CCI_REG16(0x21b0), 0x4130 },
	{ CCI_REG16(0x21b2), 0x120b },
	{ CCI_REG16(0x21b4), 0x120a },
	{ CCI_REG16(0x21b6), 0x8231 },
	{ CCI_REG16(0x21b8), 0x430b },
	{ CCI_REG16(0x21ba), 0x93c2 },
	{ CCI_REG16(0x21bc), 0x0c0a },
	{ CCI_REG16(0x21be), 0x2404 },
	{ CCI_REG16(0x21c0), 0xb3d2 },
	{ CCI_REG16(0x21c2), 0x0b05 },
	{ CCI_REG16(0x21c4), 0x2401 },
	{ CCI_REG16(0x21c6), 0x431b },
	{ CCI_REG16(0x21c8), 0x422d },
	{ CCI_REG16(0x21ca), 0x403e },
	{ CCI_REG16(0x21cc), 0x192a },
	{ CCI_REG16(0x21ce), 0x403f },
	{ CCI_REG16(0x21d0), 0x879e },
	{ CCI_REG16(0x21d2), 0x1292 },
	{ CCI_REG16(0x21d4), 0x843e },
	{ CCI_REG16(0x21d6), 0x930b },
	{ CCI_REG16(0x21d8), 0x20f4 },
	{ CCI_REG16(0x21da), 0x93e2 },
	{ CCI_REG16(0x21dc), 0x0241 },
	{ CCI_REG16(0x21de), 0x24eb },
	{ CCI_REG16(0x21e0), 0x403a },
	{ CCI_REG16(0x21e2), 0x0292 },
	{ CCI_REG16(0x21e4), 0x4aa2 },
	{ CCI_REG16(0x21e6), 0x0a00 },
	{ CCI_REG16(0x21e8), 0xb2e2 },
	{ CCI_REG16(0x21ea), 0x0361 },
	{ CCI_REG16(0x21ec), 0x2405 },
	{ CCI_REG16(0x21ee), 0x4a2f },
	{ CCI_REG16(0x21f0), 0x1292 },
	{ CCI_REG16(0x21f2), 0x8474 },
	{ CCI_REG16(0x21f4), 0x4f82 },
	{ CCI_REG16(0x21f6), 0x0a1c },
	{ CCI_REG16(0x21f8), 0x93c2 },
	{ CCI_REG16(0x21fa), 0x0360 },
	{ CCI_REG16(0x21fc), 0x34cd },
	{ CCI_REG16(0x21fe), 0x430c },
	{ CCI_REG16(0x2200), 0x4c0f },
	{ CCI_REG16(0x2202), 0x5f0f },
	{ CCI_REG16(0x2204), 0x4f0d },
	{ CCI_REG16(0x2206), 0x510d },
	{ CCI_REG16(0x2208), 0x4f0e },
	{ CCI_REG16(0x220a), 0x5a0e },
	{ CCI_REG16(0x220c), 0x4e1e },
	{ CCI_REG16(0x220e), 0x0002 },
	{ CCI_REG16(0x2210), 0x4f1f },
	{ CCI_REG16(0x2212), 0x192a },
	{ CCI_REG16(0x2214), 0x1202 },
	{ CCI_REG16(0x2216), 0xc232 },
	{ CCI_REG16(0x2218), 0x4303 },
	{ CCI_REG16(0x221a), 0x4e82 },
	{ CCI_REG16(0x221c), 0x0130 },
	{ CCI_REG16(0x221e), 0x4f82 },
	{ CCI_REG16(0x2220), 0x0138 },
	{ CCI_REG16(0x2222), 0x421e },
	{ CCI_REG16(0x2224), 0x013a },
	{ CCI_REG16(0x2226), 0x421f },
	{ CCI_REG16(0x2228), 0x013c },
	{ CCI_REG16(0x222a), 0x4132 },
	{ CCI_REG16(0x222c), 0x108e },
	{ CCI_REG16(0x222e), 0x108f },
	{ CCI_REG16(0x2230), 0xef4e },
	{ CCI_REG16(0x2232), 0xef0e },
	{ CCI_REG16(0x2234), 0xf37f },
	{ CCI_REG16(0x2236), 0xc312 },
	{ CCI_REG16(0x2238), 0x100f },
	{ CCI_REG16(0x223a), 0x100e },
	{ CCI_REG16(0x223c), 0x4e8d },
	{ CCI_REG16(0x223e), 0x0000 },
	{ CCI_REG16(0x2240), 0x531c },
	{ CCI_REG16(0x2242), 0x922c },
	{ CCI_REG16(0x2244), 0x2bdd },
	{ CCI_REG16(0x2246), 0xb3d2 },
	{ CCI_REG16(0x2248), 0x1921 },
	{ CCI_REG16(0x224a), 0x2403 },
	{ CCI_REG16(0x224c), 0x410f },
	{ CCI_REG16(0x224e), 0x1292 },
	{ CCI_REG16(0x2250), 0x847e },
	{ CCI_REG16(0x2252), 0x403b },
	{ CCI_REG16(0x2254), 0x843e },
	{ CCI_REG16(0x2256), 0x422d },
	{ CCI_REG16(0x2258), 0x410e },
	{ CCI_REG16(0x225a), 0x403f },
	{ CCI_REG16(0x225c), 0x1908 },
	{ CCI_REG16(0x225e), 0x12ab },
	{ CCI_REG16(0x2260), 0x403d },
	{ CCI_REG16(0x2262), 0x0005 },
	{ CCI_REG16(0x2264), 0x403e },
	{ CCI_REG16(0x2266), 0x0292 },
	{ CCI_REG16(0x2268), 0x403f },
	{ CCI_REG16(0x226a), 0x85ec },
	{ CCI_REG16(0x226c), 0x12ab },
	{ CCI_REG16(0x226e), 0x421f },
	{ CCI_REG16(0x2270), 0x060e },
	{ CCI_REG16(0x2272), 0x9f82 },
	{ CCI_REG16(0x2274), 0x8628 },
	{ CCI_REG16(0x2276), 0x288d },
	{ CCI_REG16(0x2278), 0x9382 },
	{ CCI_REG16(0x227a), 0x060e },
	{ CCI_REG16(0x227c), 0x248a },
	{ CCI_REG16(0x227e), 0x90ba },
	{ CCI_REG16(0x2280), 0x0010 },
	{ CCI_REG16(0x2282), 0x0000 },
	{ CCI_REG16(0x2284), 0x2c0b },
	{ CCI_REG16(0x2286), 0x93c2 },
	{ CCI_REG16(0x2288), 0x85f6 },
	{ CCI_REG16(0x228a), 0x2008 },
	{ CCI_REG16(0x228c), 0x403f },
	{ CCI_REG16(0x228e), 0x06a7 },
	{ CCI_REG16(0x2290), 0xd0ff },
	{ CCI_REG16(0x2292), 0x0007 },
	{ CCI_REG16(0x2294), 0x0000 },
	{ CCI_REG16(0x2296), 0xf0ff },
	{ CCI_REG16(0x2298), 0xfff8 },
	{ CCI_REG16(0x229a), 0x0000 },
	{ CCI_REG16(0x229c), 0x4392 },
	{ CCI_REG16(0x229e), 0x8628 },
	{ CCI_REG16(0x22a0), 0x403f },
	{ CCI_REG16(0x22a2), 0x06a7 },
	{ CCI_REG16(0x22a4), 0xd2ef },
	{ CCI_REG16(0x22a6), 0x0000 },
	{ CCI_REG16(0x22a8), 0xc2ef },
	{ CCI_REG16(0x22aa), 0x0000 },
	{ CCI_REG16(0x22ac), 0x93c2 },
	{ CCI_REG16(0x22ae), 0x86e3 },
	{ CCI_REG16(0x22b0), 0x2068 },
	{ CCI_REG16(0x22b2), 0xb0f2 },
	{ CCI_REG16(0x22b4), 0x0040 },
	{ CCI_REG16(0x22b6), 0x0b05 },
	{ CCI_REG16(0x22b8), 0x2461 },
	{ CCI_REG16(0x22ba), 0xd3d2 },
	{ CCI_REG16(0x22bc), 0x0410 },
	{ CCI_REG16(0x22be), 0xb3e2 },
	{ CCI_REG16(0x22c0), 0x0381 },
	{ CCI_REG16(0x22c2), 0x2089 },
	{ CCI_REG16(0x22c4), 0x90b2 },
	{ CCI_REG16(0x22c6), 0x0030 },
	{ CCI_REG16(0x22c8), 0x0a00 },
	{ CCI_REG16(0x22ca), 0x2c52 },
	{ CCI_REG16(0x22cc), 0x93c2 },
	{ CCI_REG16(0x22ce), 0x85f6 },
	{ CCI_REG16(0x22d0), 0x204f },
	{ CCI_REG16(0x22d2), 0x430e },
	{ CCI_REG16(0x22d4), 0x430c },
	{ CCI_REG16(0x22d6), 0x4c0f },
	{ CCI_REG16(0x22d8), 0x5f0f },
	{ CCI_REG16(0x22da), 0x5f0f },
	{ CCI_REG16(0x22dc), 0x5f0f },
	{ CCI_REG16(0x22de), 0x4f1f },
	{ CCI_REG16(0x22e0), 0x8570 },
	{ CCI_REG16(0x22e2), 0xf03f },
	{ CCI_REG16(0x22e4), 0x07ff },
	{ CCI_REG16(0x22e6), 0x903f },
	{ CCI_REG16(0x22e8), 0x0400 },
	{ CCI_REG16(0x22ea), 0x343e },
	{ CCI_REG16(0x22ec), 0x5f0e },
	{ CCI_REG16(0x22ee), 0x531c },
	{ CCI_REG16(0x22f0), 0x923c },
	{ CCI_REG16(0x22f2), 0x2bf1 },
	{ CCI_REG16(0x22f4), 0x4e0f },
	{ CCI_REG16(0x22f6), 0x930e },
	{ CCI_REG16(0x22f8), 0x3834 },
	{ CCI_REG16(0x22fa), 0x110f },
	{ CCI_REG16(0x22fc), 0x110f },
	{ CCI_REG16(0x22fe), 0x110f },
	{ CCI_REG16(0x2300), 0x9382 },
	{ CCI_REG16(0x2302), 0x85f6 },
	{ CCI_REG16(0x2304), 0x2023 },
	{ CCI_REG16(0x2306), 0x5f82 },
	{ CCI_REG16(0x2308), 0x86e6 },
	{ CCI_REG16(0x230a), 0x403b },
	{ CCI_REG16(0x230c), 0x86e6 },
	{ CCI_REG16(0x230e), 0x4b2f },
	{ CCI_REG16(0x2310), 0x12b0 },
	{ CCI_REG16(0x2312), 0xb3ec },
	{ CCI_REG16(0x2314), 0x4f8b },
	{ CCI_REG16(0x2316), 0x0000 },
	{ CCI_REG16(0x2318), 0x430c },
	{ CCI_REG16(0x231a), 0x4c0d },
	{ CCI_REG16(0x231c), 0x5d0d },
	{ CCI_REG16(0x231e), 0x5d0d },
	{ CCI_REG16(0x2320), 0x5d0d },
	{ CCI_REG16(0x2322), 0x403a },
	{ CCI_REG16(0x2324), 0x86e8 },
	{ CCI_REG16(0x2326), 0x421b },
	{ CCI_REG16(0x2328), 0x86e6 },
	{ CCI_REG16(0x232a), 0x4b0f },
	{ CCI_REG16(0x232c), 0x8a2f },
	{ CCI_REG16(0x232e), 0x4f0e },
	{ CCI_REG16(0x2330), 0x4e0f },
	{ CCI_REG16(0x2332), 0x5f0f },
	{ CCI_REG16(0x2334), 0x7f0f },
	{ CCI_REG16(0x2336), 0xe33f },
	{ CCI_REG16(0x2338), 0x8e8d },
	{ CCI_REG16(0x233a), 0x8570 },
	{ CCI_REG16(0x233c), 0x7f8d },
	{ CCI_REG16(0x233e), 0x8572 },
	{ CCI_REG16(0x2340), 0x531c },
	{ CCI_REG16(0x2342), 0x923c },
	{ CCI_REG16(0x2344), 0x2bea },
	{ CCI_REG16(0x2346), 0x4b8a },
	{ CCI_REG16(0x2348), 0x0000 },
	{ CCI_REG16(0x234a), 0x3c45 },
	{ CCI_REG16(0x234c), 0x9382 },
	{ CCI_REG16(0x234e), 0x85f8 },
	{ CCI_REG16(0x2350), 0x2005 },
	{ CCI_REG16(0x2352), 0x4382 },
	{ CCI_REG16(0x2354), 0x86e6 },
	{ CCI_REG16(0x2356), 0x4382 },
	{ CCI_REG16(0x2358), 0x86e8 },
	{ CCI_REG16(0x235a), 0x3fd7 },
	{ CCI_REG16(0x235c), 0x4f82 },
	{ CCI_REG16(0x235e), 0x86e6 },
	{ CCI_REG16(0x2360), 0x3fd4 },
	{ CCI_REG16(0x2362), 0x503f },
	{ CCI_REG16(0x2364), 0x0007 },
	{ CCI_REG16(0x2366), 0x3fc9 },
	{ CCI_REG16(0x2368), 0x5f0e },
	{ CCI_REG16(0x236a), 0x503e },
	{ CCI_REG16(0x236c), 0xf800 },
	{ CCI_REG16(0x236e), 0x3fbf },
	{ CCI_REG16(0x2370), 0x430f },
	{ CCI_REG16(0x2372), 0x12b0 },
	{ CCI_REG16(0x2374), 0xb3ec },
	{ CCI_REG16(0x2376), 0x4382 },
	{ CCI_REG16(0x2378), 0x86e6 },
	{ CCI_REG16(0x237a), 0x3c2d },
	{ CCI_REG16(0x237c), 0xc3d2 },
	{ CCI_REG16(0x237e), 0x0410 },
	{ CCI_REG16(0x2380), 0x3f9e },
	{ CCI_REG16(0x2382), 0x430d },
	{ CCI_REG16(0x2384), 0x403e },
	{ CCI_REG16(0x2386), 0x0050 },
	{ CCI_REG16(0x2388), 0x403f },
	{ CCI_REG16(0x238a), 0x84d0 },
	{ CCI_REG16(0x238c), 0x1292 },
	{ CCI_REG16(0x238e), 0x844e },
	{ CCI_REG16(0x2390), 0x3f90 },
	{ CCI_REG16(0x2392), 0x5392 },
	{ CCI_REG16(0x2394), 0x8628 },
	{ CCI_REG16(0x2396), 0x3f84 },
	{ CCI_REG16(0x2398), 0x403b },
	{ CCI_REG16(0x239a), 0x843e },
	{ CCI_REG16(0x239c), 0x4a0f },
	{ CCI_REG16(0x239e), 0x532f },
	{ CCI_REG16(0x23a0), 0x422d },
	{ CCI_REG16(0x23a2), 0x4f0e },
	{ CCI_REG16(0x23a4), 0x403f },
	{ CCI_REG16(0x23a6), 0x0e08 },
	{ CCI_REG16(0x23a8), 0x12ab },
	{ CCI_REG16(0x23aa), 0x422d },
	{ CCI_REG16(0x23ac), 0x403e },
	{ CCI_REG16(0x23ae), 0x192a },
	{ CCI_REG16(0x23b0), 0x410f },
	{ CCI_REG16(0x23b2), 0x12ab },
	{ CCI_REG16(0x23b4), 0x3f48 },
	{ CCI_REG16(0x23b6), 0x93c2 },
	{ CCI_REG16(0x23b8), 0x85f6 },
	{ CCI_REG16(0x23ba), 0x2312 },
	{ CCI_REG16(0x23bc), 0x403a },
	{ CCI_REG16(0x23be), 0x85ec },
	{ CCI_REG16(0x23c0), 0x3f11 },
	{ CCI_REG16(0x23c2), 0x403d },
	{ CCI_REG16(0x23c4), 0x0200 },
	{ CCI_REG16(0x23c6), 0x422e },
	{ CCI_REG16(0x23c8), 0x403f },
	{ CCI_REG16(0x23ca), 0x192a },
	{ CCI_REG16(0x23cc), 0x1292 },
	{ CCI_REG16(0x23ce), 0x844e },
	{ CCI_REG16(0x23d0), 0xc3d2 },
	{ CCI_REG16(0x23d2), 0x1921 },
	{ CCI_REG16(0x23d4), 0x3f02 },
	{ CCI_REG16(0x23d6), 0x422d },
	{ CCI_REG16(0x23d8), 0x403e },
	{ CCI_REG16(0x23da), 0x879e },
	{ CCI_REG16(0x23dc), 0x403f },
	{ CCI_REG16(0x23de), 0x192a },
	{ CCI_REG16(0x23e0), 0x1292 },
	{ CCI_REG16(0x23e2), 0x843e },
	{ CCI_REG16(0x23e4), 0x5231 },
	{ CCI_REG16(0x23e6), 0x413a },
	{ CCI_REG16(0x23e8), 0x413b },
	{ CCI_REG16(0x23ea), 0x4130 },
	{ CCI_REG16(0x23ec), 0x4382 },
	{ CCI_REG16(0x23ee), 0x052c },
	{ CCI_REG16(0x23f0), 0x4f0d },
	{ CCI_REG16(0x23f2), 0x930d },
	{ CCI_REG16(0x23f4), 0x3402 },
	{ CCI_REG16(0x23f6), 0xe33d },
	{ CCI_REG16(0x23f8), 0x531d },
	{ CCI_REG16(0x23fa), 0xf03d },
	{ CCI_REG16(0x23fc), 0x07f0 },
	{ CCI_REG16(0x23fe), 0x4d0e },
	{ CCI_REG16(0x2400), 0xc312 },
	{ CCI_REG16(0x2402), 0x100e },
	{ CCI_REG16(0x2404), 0x110e },
	{ CCI_REG16(0x2406), 0x110e },
	{ CCI_REG16(0x2408), 0x110e },
	{ CCI_REG16(0x240a), 0x930f },
	{ CCI_REG16(0x240c), 0x3803 },
	{ CCI_REG16(0x240e), 0x4ec2 },
	{ CCI_REG16(0x2410), 0x052c },
	{ CCI_REG16(0x2412), 0x3c04 },
	{ CCI_REG16(0x2414), 0x4ec2 },
	{ CCI_REG16(0x2416), 0x052d },
	{ CCI_REG16(0x2418), 0xe33d },
	{ CCI_REG16(0x241a), 0x531d },
	{ CCI_REG16(0x241c), 0x4d0f },
	{ CCI_REG16(0x241e), 0x4130 },
	{ CCI_REG16(0x2420), 0x120b },
	{ CCI_REG16(0x2422), 0x120a },
	{ CCI_REG16(0x2424), 0x93c2 },
	{ CCI_REG16(0x2426), 0x85f6 },
	{ CCI_REG16(0x2428), 0x2003 },
	{ CCI_REG16(0x242a), 0xb3d2 },
	{ CCI_REG16(0x242c), 0x0360 },
	{ CCI_REG16(0x242e), 0x2402 },
	{ CCI_REG16(0x2430), 0x1292 },
	{ CCI_REG16(0x2432), 0x847a },
	{ CCI_REG16(0x2434), 0x1292 },
	{ CCI_REG16(0x2436), 0x847c },
	{ CCI_REG16(0x2438), 0x93c2 },
	{ CCI_REG16(0x243a), 0x0600 },
	{ CCI_REG16(0x243c), 0x3803 },
	{ CCI_REG16(0x243e), 0x93c2 },
	{ CCI_REG16(0x2440), 0x0604 },
	{ CCI_REG16(0x2442), 0x3832 },
	{ CCI_REG16(0x2444), 0xd2f2 },
	{ CCI_REG16(0x2446), 0x0f01 },
	{ CCI_REG16(0x2448), 0xb3d2 },
	{ CCI_REG16(0x244a), 0x0363 },
	{ CCI_REG16(0x244c), 0x2418 },
	{ CCI_REG16(0x244e), 0x421f },
	{ CCI_REG16(0x2450), 0x1246 },
	{ CCI_REG16(0x2452), 0x4f0e },
	{ CCI_REG16(0x2454), 0x430f },
	{ CCI_REG16(0x2456), 0x421b },
	{ CCI_REG16(0x2458), 0x1244 },
	{ CCI_REG16(0x245a), 0x430a },
	{ CCI_REG16(0x245c), 0xda0e },
	{ CCI_REG16(0x245e), 0xdb0f },
	{ CCI_REG16(0x2460), 0x821e },
	{ CCI_REG16(0x2462), 0x86f4 },
	{ CCI_REG16(0x2464), 0x721f },
	{ CCI_REG16(0x2466), 0x86f6 },
	{ CCI_REG16(0x2468), 0x2c1b },
	{ CCI_REG16(0x246a), 0x421f },
	{ CCI_REG16(0x246c), 0x1240 },
	{ CCI_REG16(0x246e), 0xf03f },
	{ CCI_REG16(0x2470), 0x01ff },
	{ CCI_REG16(0x2472), 0x9f82 },
	{ CCI_REG16(0x2474), 0x0a00 },
	{ CCI_REG16(0x2476), 0x2814 },
	{ CCI_REG16(0x2478), 0xd0f2 },
	{ CCI_REG16(0x247a), 0xff80 },
	{ CCI_REG16(0x247c), 0x1240 },
	{ CCI_REG16(0x247e), 0x93c2 },
	{ CCI_REG16(0x2480), 0x85f6 },
	{ CCI_REG16(0x2482), 0x2015 },
	{ CCI_REG16(0x2484), 0xb0f2 },
	{ CCI_REG16(0x2486), 0x0020 },
	{ CCI_REG16(0x2488), 0x0381 },
	{ CCI_REG16(0x248a), 0x2407 },
	{ CCI_REG16(0x248c), 0x9292 },
	{ CCI_REG16(0x248e), 0x862a },
	{ CCI_REG16(0x2490), 0x0384 },
	{ CCI_REG16(0x2492), 0x2c03 },
	{ CCI_REG16(0x2494), 0xd3d2 },
	{ CCI_REG16(0x2496), 0x0649 },
	{ CCI_REG16(0x2498), 0x3c0a },
	{ CCI_REG16(0x249a), 0xc3d2 },
	{ CCI_REG16(0x249c), 0x0649 },
	{ CCI_REG16(0x249e), 0x3c07 },
	{ CCI_REG16(0x24a0), 0xf0f2 },
	{ CCI_REG16(0x24a2), 0x007f },
	{ CCI_REG16(0x24a4), 0x1240 },
	{ CCI_REG16(0x24a6), 0x3feb },
	{ CCI_REG16(0x24a8), 0xc2f2 },
	{ CCI_REG16(0x24aa), 0x0f01 },
	{ CCI_REG16(0x24ac), 0x3fcd },
	{ CCI_REG16(0x24ae), 0x413a },
	{ CCI_REG16(0x24b0), 0x413b },
	{ CCI_REG16(0x24b2), 0x4130 },
	{ CCI_REG16(0x24b4), 0x425f },
	{ CCI_REG16(0x24b6), 0x86e2 },
	{ CCI_REG16(0x24b8), 0xd25f },
	{ CCI_REG16(0x24ba), 0x86e1 },
	{ CCI_REG16(0x24bc), 0x4f4e },
	{ CCI_REG16(0x24be), 0x5e0e },
	{ CCI_REG16(0x24c0), 0x425f },
	{ CCI_REG16(0x24c2), 0x0204 },
	{ CCI_REG16(0x24c4), 0xf07f },
	{ CCI_REG16(0x24c6), 0x0003 },
	{ CCI_REG16(0x24c8), 0xf37f },
	{ CCI_REG16(0x24ca), 0xdf0e },
	{ CCI_REG16(0x24cc), 0x40b2 },
	{ CCI_REG16(0x24ce), 0x8030 },
	{ CCI_REG16(0x24d0), 0x7a00 },
	{ CCI_REG16(0x24d2), 0x40b2 },
	{ CCI_REG16(0x24d4), 0x0100 },
	{ CCI_REG16(0x24d6), 0x7a02 },
	{ CCI_REG16(0x24d8), 0x40b2 },
	{ CCI_REG16(0x24da), 0x0d04 },
	{ CCI_REG16(0x24dc), 0x7a0c },
	{ CCI_REG16(0x24de), 0x40b2 },
	{ CCI_REG16(0x24e0), 0xfff0 },
	{ CCI_REG16(0x24e2), 0x7a04 },
	{ CCI_REG16(0x24e4), 0x93c2 },
	{ CCI_REG16(0x24e6), 0x86e0 },
	{ CCI_REG16(0x24e8), 0x240a },
	{ CCI_REG16(0x24ea), 0x40b2 },
	{ CCI_REG16(0x24ec), 0xfff1 },
	{ CCI_REG16(0x24ee), 0x7a06 },
	{ CCI_REG16(0x24f0), 0x40b2 },
	{ CCI_REG16(0x24f2), 0xfff4 },
	{ CCI_REG16(0x24f4), 0x7a08 },
	{ CCI_REG16(0x24f6), 0x40b2 },
	{ CCI_REG16(0x24f8), 0xfff5 },
	{ CCI_REG16(0x24fa), 0x7a0a },
	{ CCI_REG16(0x24fc), 0x3c09 },
	{ CCI_REG16(0x24fe), 0x40b2 },
	{ CCI_REG16(0x2500), 0xfff2 },
	{ CCI_REG16(0x2502), 0x7a06 },
	{ CCI_REG16(0x2504), 0x40b2 },
	{ CCI_REG16(0x2506), 0xfff4 },
	{ CCI_REG16(0x2508), 0x7a08 },
	{ CCI_REG16(0x250a), 0x40b2 },
	{ CCI_REG16(0x250c), 0xfff6 },
	{ CCI_REG16(0x250e), 0x7a0a },
	{ CCI_REG16(0x2510), 0xf03e },
	{ CCI_REG16(0x2512), 0x0003 },
	{ CCI_REG16(0x2514), 0x5e0e },
	{ CCI_REG16(0x2516), 0x425f },
	{ CCI_REG16(0x2518), 0x86e2 },
	{ CCI_REG16(0x251a), 0xd25f },
	{ CCI_REG16(0x251c), 0x86e1 },
	{ CCI_REG16(0x251e), 0xf31f },
	{ CCI_REG16(0x2520), 0x5f0f },
	{ CCI_REG16(0x2522), 0x5f0f },
	{ CCI_REG16(0x2524), 0x5f0f },
	{ CCI_REG16(0x2526), 0xd31e },
	{ CCI_REG16(0x2528), 0xdf0e },
	{ CCI_REG16(0x252a), 0x4e82 },
	{ CCI_REG16(0x252c), 0x7a12 },
	{ CCI_REG16(0x252e), 0x4130 },
	{ CCI_REG16(0x2530), 0x120b },
	{ CCI_REG16(0x2532), 0x120a },
	{ CCI_REG16(0x2534), 0x1209 },
	{ CCI_REG16(0x2536), 0x1208 },
	{ CCI_REG16(0x2538), 0x1207 },
	{ CCI_REG16(0x253a), 0x1206 },
	{ CCI_REG16(0x253c), 0x1205 },
	{ CCI_REG16(0x253e), 0x1204 },
	{ CCI_REG16(0x2540), 0x8231 },
	{ CCI_REG16(0x2542), 0x4f81 },
	{ CCI_REG16(0x2544), 0x0000 },
	{ CCI_REG16(0x2546), 0x4381 },
	{ CCI_REG16(0x2548), 0x0002 },
	{ CCI_REG16(0x254a), 0x4304 },
	{ CCI_REG16(0x254c), 0x411c },
	{ CCI_REG16(0x254e), 0x0002 },
	{ CCI_REG16(0x2550), 0x5c0c },
	{ CCI_REG16(0x2552), 0x4c0f },
	{ CCI_REG16(0x2554), 0x5f0f },
	{ CCI_REG16(0x2556), 0x5f0f },
	{ CCI_REG16(0x2558), 0x5f0f },
	{ CCI_REG16(0x255a), 0x5f0f },
	{ CCI_REG16(0x255c), 0x5f0f },
	{ CCI_REG16(0x255e), 0x503f },
	{ CCI_REG16(0x2560), 0x1980 },
	{ CCI_REG16(0x2562), 0x440d },
	{ CCI_REG16(0x2564), 0x5d0d },
	{ CCI_REG16(0x2566), 0x4d0e },
	{ CCI_REG16(0x2568), 0x5f0e },
	{ CCI_REG16(0x256a), 0x4e2e },
	{ CCI_REG16(0x256c), 0x4d05 },
	{ CCI_REG16(0x256e), 0x5505 },
	{ CCI_REG16(0x2570), 0x5f05 },
	{ CCI_REG16(0x2572), 0x4516 },
	{ CCI_REG16(0x2574), 0x0008 },
	{ CCI_REG16(0x2576), 0x4517 },
	{ CCI_REG16(0x2578), 0x000a },
	{ CCI_REG16(0x257a), 0x460a },
	{ CCI_REG16(0x257c), 0x470b },
	{ CCI_REG16(0x257e), 0xf30a },
	{ CCI_REG16(0x2580), 0xf32b },
	{ CCI_REG16(0x2582), 0x4a81 },
	{ CCI_REG16(0x2584), 0x0004 },
	{ CCI_REG16(0x2586), 0x4b81 },
	{ CCI_REG16(0x2588), 0x0006 },
	{ CCI_REG16(0x258a), 0xb03e },
	{ CCI_REG16(0x258c), 0x2000 },
	{ CCI_REG16(0x258e), 0x2404 },
	{ CCI_REG16(0x2590), 0xf03e },
	{ CCI_REG16(0x2592), 0x1fff },
	{ CCI_REG16(0x2594), 0xe33e },
	{ CCI_REG16(0x2596), 0x531e },
	{ CCI_REG16(0x2598), 0xf317 },
	{ CCI_REG16(0x259a), 0x503e },
	{ CCI_REG16(0x259c), 0x2000 },
	{ CCI_REG16(0x259e), 0x4e0f },
	{ CCI_REG16(0x25a0), 0x5f0f },
	{ CCI_REG16(0x25a2), 0x7f0f },
	{ CCI_REG16(0x25a4), 0xe33f },
	{ CCI_REG16(0x25a6), 0x512c },
	{ CCI_REG16(0x25a8), 0x4c28 },
	{ CCI_REG16(0x25aa), 0x4309 },
	{ CCI_REG16(0x25ac), 0x4e0a },
	{ CCI_REG16(0x25ae), 0x4f0b },
	{ CCI_REG16(0x25b0), 0x480c },
	{ CCI_REG16(0x25b2), 0x490d },
	{ CCI_REG16(0x25b4), 0x1202 },
	{ CCI_REG16(0x25b6), 0xc232 },
	{ CCI_REG16(0x25b8), 0x12b0 },
	{ CCI_REG16(0x25ba), 0xffc0 },
	{ CCI_REG16(0x25bc), 0x4132 },
	{ CCI_REG16(0x25be), 0x108e },
	{ CCI_REG16(0x25c0), 0x108f },
	{ CCI_REG16(0x25c2), 0xef4e },
	{ CCI_REG16(0x25c4), 0xef0e },
	{ CCI_REG16(0x25c6), 0xf37f },
	{ CCI_REG16(0x25c8), 0xc312 },
	{ CCI_REG16(0x25ca), 0x100f },
	{ CCI_REG16(0x25cc), 0x100e },
	{ CCI_REG16(0x25ce), 0x4e85 },
	{ CCI_REG16(0x25d0), 0x0018 },
	{ CCI_REG16(0x25d2), 0x4f85 },
	{ CCI_REG16(0x25d4), 0x001a },
	{ CCI_REG16(0x25d6), 0x480a },
	{ CCI_REG16(0x25d8), 0x490b },
	{ CCI_REG16(0x25da), 0x460c },
	{ CCI_REG16(0x25dc), 0x470d },
	{ CCI_REG16(0x25de), 0x1202 },
	{ CCI_REG16(0x25e0), 0xc232 },
	{ CCI_REG16(0x25e2), 0x12b0 },
	{ CCI_REG16(0x25e4), 0xffc0 },
	{ CCI_REG16(0x25e6), 0x4132 },
	{ CCI_REG16(0x25e8), 0x4e0c },
	{ CCI_REG16(0x25ea), 0x4f0d },
	{ CCI_REG16(0x25ec), 0x108c },
	{ CCI_REG16(0x25ee), 0x108d },
	{ CCI_REG16(0x25f0), 0xed4c },
	{ CCI_REG16(0x25f2), 0xed0c },
	{ CCI_REG16(0x25f4), 0xf37d },
	{ CCI_REG16(0x25f6), 0xc312 },
	{ CCI_REG16(0x25f8), 0x100d },
	{ CCI_REG16(0x25fa), 0x100c },
	{ CCI_REG16(0x25fc), 0x411e },
	{ CCI_REG16(0x25fe), 0x0004 },
	{ CCI_REG16(0x2600), 0x411f },
	{ CCI_REG16(0x2602), 0x0006 },
	{ CCI_REG16(0x2604), 0x5e0e },
	{ CCI_REG16(0x2606), 0x6f0f },
	{ CCI_REG16(0x2608), 0x5e0e },
	{ CCI_REG16(0x260a), 0x6f0f },
	{ CCI_REG16(0x260c), 0x5e0e },
	{ CCI_REG16(0x260e), 0x6f0f },
	{ CCI_REG16(0x2610), 0xde0c },
	{ CCI_REG16(0x2612), 0xdf0d },
	{ CCI_REG16(0x2614), 0x4c85 },
	{ CCI_REG16(0x2616), 0x002c },
	{ CCI_REG16(0x2618), 0x4d85 },
	{ CCI_REG16(0x261a), 0x002e },
	{ CCI_REG16(0x261c), 0x5314 },
	{ CCI_REG16(0x261e), 0x9224 },
	{ CCI_REG16(0x2620), 0x2b95 },
	{ CCI_REG16(0x2622), 0x5391 },
	{ CCI_REG16(0x2624), 0x0002 },
	{ CCI_REG16(0x2626), 0x92a1 },
	{ CCI_REG16(0x2628), 0x0002 },
	{ CCI_REG16(0x262a), 0x2b8f },
	{ CCI_REG16(0x262c), 0x5231 },
	{ CCI_REG16(0x262e), 0x4134 },
	{ CCI_REG16(0x2630), 0x4135 },
	{ CCI_REG16(0x2632), 0x4136 },
	{ CCI_REG16(0x2634), 0x4137 },
	{ CCI_REG16(0x2636), 0x4138 },
	{ CCI_REG16(0x2638), 0x4139 },
	{ CCI_REG16(0x263a), 0x413a },
	{ CCI_REG16(0x263c), 0x413b },
	{ CCI_REG16(0x263e), 0x4130 },
	{ CCI_REG16(0x2640), 0x120b },
	{ CCI_REG16(0x2642), 0x120a },
	{ CCI_REG16(0x2644), 0x1209 },
	{ CCI_REG16(0x2646), 0x8031 },
	{ CCI_REG16(0x2648), 0x000c },
	{ CCI_REG16(0x264a), 0x425f },
	{ CCI_REG16(0x264c), 0x0205 },
	{ CCI_REG16(0x264e), 0xc312 },
	{ CCI_REG16(0x2650), 0x104f },
	{ CCI_REG16(0x2652), 0x114f },
	{ CCI_REG16(0x2654), 0x114f },
	{ CCI_REG16(0x2656), 0x114f },
	{ CCI_REG16(0x2658), 0x114f },
	{ CCI_REG16(0x265a), 0x114f },
	{ CCI_REG16(0x265c), 0xf37f },
	{ CCI_REG16(0x265e), 0x4f0b },
	{ CCI_REG16(0x2660), 0xf31b },
	{ CCI_REG16(0x2662), 0x5b0b },
	{ CCI_REG16(0x2664), 0x5b0b },
	{ CCI_REG16(0x2666), 0x5b0b },
	{ CCI_REG16(0x2668), 0x503b },
	{ CCI_REG16(0x266a), 0xd196 },
	{ CCI_REG16(0x266c), 0x4219 },
	{ CCI_REG16(0x266e), 0x0508 },
	{ CCI_REG16(0x2670), 0xf039 },
	{ CCI_REG16(0x2672), 0x2000 },
	{ CCI_REG16(0x2674), 0x4f0a },
	{ CCI_REG16(0x2676), 0xc312 },
	{ CCI_REG16(0x2678), 0x100a },
	{ CCI_REG16(0x267a), 0xe31a },
	{ CCI_REG16(0x267c), 0x421f },
	{ CCI_REG16(0x267e), 0x86ee },
	{ CCI_REG16(0x2680), 0x503f },
	{ CCI_REG16(0x2682), 0xff60 },
	{ CCI_REG16(0x2684), 0x903f },
	{ CCI_REG16(0x2686), 0x00c8 },
	{ CCI_REG16(0x2688), 0x2c02 },
	{ CCI_REG16(0x268a), 0x403f },
	{ CCI_REG16(0x268c), 0x00c8 },
	{ CCI_REG16(0x268e), 0x4f82 },
	{ CCI_REG16(0x2690), 0x7322 },
	{ CCI_REG16(0x2692), 0xb3d2 },
	{ CCI_REG16(0x2694), 0x0381 },
	{ CCI_REG16(0x2696), 0x2009 },
	{ CCI_REG16(0x2698), 0x421f },
	{ CCI_REG16(0x269a), 0x85f8 },
	{ CCI_REG16(0x269c), 0xd21f },
	{ CCI_REG16(0x269e), 0x85f6 },
	{ CCI_REG16(0x26a0), 0x930f },
	{ CCI_REG16(0x26a2), 0x24b1 },
	{ CCI_REG16(0x26a4), 0x40f2 },
	{ CCI_REG16(0x26a6), 0xff80 },
	{ CCI_REG16(0x26a8), 0x0619 },
	{ CCI_REG16(0x26aa), 0x1292 },
	{ CCI_REG16(0x26ac), 0xd00a },
	{ CCI_REG16(0x26ae), 0x430d },
	{ CCI_REG16(0x26b0), 0x93c2 },
	{ CCI_REG16(0x26b2), 0x86e0 },
	{ CCI_REG16(0x26b4), 0x2003 },
	{ CCI_REG16(0x26b6), 0xb2f2 },
	{ CCI_REG16(0x26b8), 0x0360 },
	{ CCI_REG16(0x26ba), 0x2001 },
	{ CCI_REG16(0x26bc), 0x431d },
	{ CCI_REG16(0x26be), 0x425f },
	{ CCI_REG16(0x26c0), 0x86e3 },
	{ CCI_REG16(0x26c2), 0xd25f },
	{ CCI_REG16(0x26c4), 0x86e2 },
	{ CCI_REG16(0x26c6), 0xf37f },
	{ CCI_REG16(0x26c8), 0x5f0f },
	{ CCI_REG16(0x26ca), 0x425e },
	{ CCI_REG16(0x26cc), 0x86dd },
	{ CCI_REG16(0x26ce), 0xde0f },
	{ CCI_REG16(0x26d0), 0x5f0f },
	{ CCI_REG16(0x26d2), 0x5b0f },
	{ CCI_REG16(0x26d4), 0x4fa2 },
	{ CCI_REG16(0x26d6), 0x0402 },
	{ CCI_REG16(0x26d8), 0x930d },
	{ CCI_REG16(0x26da), 0x2007 },
	{ CCI_REG16(0x26dc), 0x930a },
	{ CCI_REG16(0x26de), 0x248e },
	{ CCI_REG16(0x26e0), 0x4f5f },
	{ CCI_REG16(0x26e2), 0x0001 },
	{ CCI_REG16(0x26e4), 0xf37f },
	{ CCI_REG16(0x26e6), 0x4fc2 },
	{ CCI_REG16(0x26e8), 0x0403 },
	{ CCI_REG16(0x26ea), 0x93c2 },
	{ CCI_REG16(0x26ec), 0x86dd },
	{ CCI_REG16(0x26ee), 0x2483 },
	{ CCI_REG16(0x26f0), 0xc2f2 },
	{ CCI_REG16(0x26f2), 0x0400 },
	{ CCI_REG16(0x26f4), 0xb2e2 },
	{ CCI_REG16(0x26f6), 0x0265 },
	{ CCI_REG16(0x26f8), 0x2407 },
	{ CCI_REG16(0x26fa), 0x421f },
	{ CCI_REG16(0x26fc), 0x0508 },
	{ CCI_REG16(0x26fe), 0xf03f },
	{ CCI_REG16(0x2700), 0xffdf },
	{ CCI_REG16(0x2702), 0xd90f },
	{ CCI_REG16(0x2704), 0x4f82 },
	{ CCI_REG16(0x2706), 0x0508 },
	{ CCI_REG16(0x2708), 0xb3d2 },
	{ CCI_REG16(0x270a), 0x0383 },
	{ CCI_REG16(0x270c), 0x2484 },
	{ CCI_REG16(0x270e), 0x403f },
	{ CCI_REG16(0x2710), 0x0508 },
	{ CCI_REG16(0x2712), 0x4fb1 },
	{ CCI_REG16(0x2714), 0x0000 },
	{ CCI_REG16(0x2716), 0x4fb1 },
	{ CCI_REG16(0x2718), 0x0002 },
	{ CCI_REG16(0x271a), 0x4fb1 },
	{ CCI_REG16(0x271c), 0x0004 },
	{ CCI_REG16(0x271e), 0x403f },
	{ CCI_REG16(0x2720), 0x0500 },
	{ CCI_REG16(0x2722), 0x4fb1 },
	{ CCI_REG16(0x2724), 0x0006 },
	{ CCI_REG16(0x2726), 0x4fb1 },
	{ CCI_REG16(0x2728), 0x0008 },
	{ CCI_REG16(0x272a), 0x4fb1 },
	{ CCI_REG16(0x272c), 0x000a },
	{ CCI_REG16(0x272e), 0xb3e2 },
	{ CCI_REG16(0x2730), 0x0383 },
	{ CCI_REG16(0x2732), 0x2412 },
	{ CCI_REG16(0x2734), 0xc2e1 },
	{ CCI_REG16(0x2736), 0x0002 },
	{ CCI_REG16(0x2738), 0xb2e2 },
	{ CCI_REG16(0x273a), 0x0383 },
	{ CCI_REG16(0x273c), 0x434f },
	{ CCI_REG16(0x273e), 0x634f },
	{ CCI_REG16(0x2740), 0xf37f },
	{ CCI_REG16(0x2742), 0x4f4e },
	{ CCI_REG16(0x2744), 0x114e },
	{ CCI_REG16(0x2746), 0x434e },
	{ CCI_REG16(0x2748), 0x104e },
	{ CCI_REG16(0x274a), 0x415f },
	{ CCI_REG16(0x274c), 0x0007 },
	{ CCI_REG16(0x274e), 0xf07f },
	{ CCI_REG16(0x2750), 0x007f },
	{ CCI_REG16(0x2752), 0xde4f },
	{ CCI_REG16(0x2754), 0x4fc1 },
	{ CCI_REG16(0x2756), 0x0007 },
	{ CCI_REG16(0x2758), 0xb2f2 },
	{ CCI_REG16(0x275a), 0x0383 },
	{ CCI_REG16(0x275c), 0x2415 },
	{ CCI_REG16(0x275e), 0xf0f1 },
	{ CCI_REG16(0x2760), 0xffbf },
	{ CCI_REG16(0x2762), 0x0000 },
	{ CCI_REG16(0x2764), 0xb0f2 },
	{ CCI_REG16(0x2766), 0x0010 },
	{ CCI_REG16(0x2768), 0x0383 },
	{ CCI_REG16(0x276a), 0x434e },
	{ CCI_REG16(0x276c), 0x634e },
	{ CCI_REG16(0x276e), 0x5e4e },
	{ CCI_REG16(0x2770), 0x5e4e },
	{ CCI_REG16(0x2772), 0x5e4e },
	{ CCI_REG16(0x2774), 0x5e4e },
	{ CCI_REG16(0x2776), 0x5e4e },
	{ CCI_REG16(0x2778), 0x5e4e },
	{ CCI_REG16(0x277a), 0x415f },
	{ CCI_REG16(0x277c), 0x0006 },
	{ CCI_REG16(0x277e), 0xf07f },
	{ CCI_REG16(0x2780), 0xffbf },
	{ CCI_REG16(0x2782), 0xde4f },
	{ CCI_REG16(0x2784), 0x4fc1 },
	{ CCI_REG16(0x2786), 0x0006 },
	{ CCI_REG16(0x2788), 0xb0f2 },
	{ CCI_REG16(0x278a), 0x0020 },
	{ CCI_REG16(0x278c), 0x0383 },
	{ CCI_REG16(0x278e), 0x2410 },
	{ CCI_REG16(0x2790), 0xf0f1 },
	{ CCI_REG16(0x2792), 0xffdf },
	{ CCI_REG16(0x2794), 0x0002 },
	{ CCI_REG16(0x2796), 0xb0f2 },
	{ CCI_REG16(0x2798), 0x0040 },
	{ CCI_REG16(0x279a), 0x0383 },
	{ CCI_REG16(0x279c), 0x434e },
	{ CCI_REG16(0x279e), 0x634e },
	{ CCI_REG16(0x27a0), 0x5e4e },
	{ CCI_REG16(0x27a2), 0x5e4e },
	{ CCI_REG16(0x27a4), 0x415f },
	{ CCI_REG16(0x27a6), 0x0008 },
	{ CCI_REG16(0x27a8), 0xc26f },
	{ CCI_REG16(0x27aa), 0xde4f },
	{ CCI_REG16(0x27ac), 0x4fc1 },
	{ CCI_REG16(0x27ae), 0x0008 },
	{ CCI_REG16(0x27b0), 0x93c2 },
	{ CCI_REG16(0x27b2), 0x0383 },
	{ CCI_REG16(0x27b4), 0x3412 },
	{ CCI_REG16(0x27b6), 0xf0f1 },
	{ CCI_REG16(0x27b8), 0xffdf },
	{ CCI_REG16(0x27ba), 0x0000 },
	{ CCI_REG16(0x27bc), 0x425e },
	{ CCI_REG16(0x27be), 0x0382 },
	{ CCI_REG16(0x27c0), 0xf35e },
	{ CCI_REG16(0x27c2), 0x5e4e },
	{ CCI_REG16(0x27c4), 0x5e4e },
	{ CCI_REG16(0x27c6), 0x5e4e },
	{ CCI_REG16(0x27c8), 0x5e4e },
	{ CCI_REG16(0x27ca), 0x5e4e },
	{ CCI_REG16(0x27cc), 0x415f },
	{ CCI_REG16(0x27ce), 0x0006 },
	{ CCI_REG16(0x27d0), 0xf07f },
	{ CCI_REG16(0x27d2), 0xffdf },
	{ CCI_REG16(0x27d4), 0xde4f },
	{ CCI_REG16(0x27d6), 0x4fc1 },
	{ CCI_REG16(0x27d8), 0x0006 },
	{ CCI_REG16(0x27da), 0x410f },
	{ CCI_REG16(0x27dc), 0x4fb2 },
	{ CCI_REG16(0x27de), 0x0508 },
	{ CCI_REG16(0x27e0), 0x4fb2 },
	{ CCI_REG16(0x27e2), 0x050a },
	{ CCI_REG16(0x27e4), 0x4fb2 },
	{ CCI_REG16(0x27e6), 0x050c },
	{ CCI_REG16(0x27e8), 0x4fb2 },
	{ CCI_REG16(0x27ea), 0x0500 },
	{ CCI_REG16(0x27ec), 0x4fb2 },
	{ CCI_REG16(0x27ee), 0x0502 },
	{ CCI_REG16(0x27f0), 0x4fb2 },
	{ CCI_REG16(0x27f2), 0x0504 },
	{ CCI_REG16(0x27f4), 0x3c10 },
	{ CCI_REG16(0x27f6), 0xd2f2 },
	{ CCI_REG16(0x27f8), 0x0400 },
	{ CCI_REG16(0x27fa), 0x3f7c },
	{ CCI_REG16(0x27fc), 0x4f6f },
	{ CCI_REG16(0x27fe), 0xf37f },
	{ CCI_REG16(0x2800), 0x4fc2 },
	{ CCI_REG16(0x2802), 0x0402 },
	{ CCI_REG16(0x2804), 0x3f72 },
	{ CCI_REG16(0x2806), 0x90f2 },
	{ CCI_REG16(0x2808), 0x0011 },
	{ CCI_REG16(0x280a), 0x0619 },
	{ CCI_REG16(0x280c), 0x2b4e },
	{ CCI_REG16(0x280e), 0x50f2 },
	{ CCI_REG16(0x2810), 0xfff0 },
	{ CCI_REG16(0x2812), 0x0619 },
	{ CCI_REG16(0x2814), 0x3f4a },
	{ CCI_REG16(0x2816), 0x5031 },
	{ CCI_REG16(0x2818), 0x000c },
	{ CCI_REG16(0x281a), 0x4139 },
	{ CCI_REG16(0x281c), 0x413a },
	{ CCI_REG16(0x281e), 0x413b },
	{ CCI_REG16(0x2820), 0x4130 },
	{ CCI_REG16(0x2822), 0x0900 },
	{ CCI_REG16(0x2824), 0x7312 },
	{ CCI_REG16(0x2826), 0x421f },
	{ CCI_REG16(0x2828), 0x0a08 },
	{ CCI_REG16(0x282a), 0xf03f },
	{ CCI_REG16(0x282c), 0xf7ff },
	{ CCI_REG16(0x282e), 0x4f82 },
	{ CCI_REG16(0x2830), 0x0a88 },
	{ CCI_REG16(0x2832), 0x0900 },
	{ CCI_REG16(0x2834), 0x7312 },
	{ CCI_REG16(0x2836), 0x421f },
	{ CCI_REG16(0x2838), 0x0a0e },
	{ CCI_REG16(0x283a), 0xf03f },
	{ CCI_REG16(0x283c), 0x7fff },
	{ CCI_REG16(0x283e), 0x4f82 },
	{ CCI_REG16(0x2840), 0x0a8e },
	{ CCI_REG16(0x2842), 0x0900 },
	{ CCI_REG16(0x2844), 0x7312 },
	{ CCI_REG16(0x2846), 0x421f },
	{ CCI_REG16(0x2848), 0x0a1e },
	{ CCI_REG16(0x284a), 0xc31f },
	{ CCI_REG16(0x284c), 0x4f82 },
	{ CCI_REG16(0x284e), 0x0a9e },
	{ CCI_REG16(0x2850), 0x4130 },
	{ CCI_REG16(0x2852), 0x4292 },
	{ CCI_REG16(0x2854), 0x0a08 },
	{ CCI_REG16(0x2856), 0x0a88 },
	{ CCI_REG16(0x2858), 0x0900 },
	{ CCI_REG16(0x285a), 0x7312 },
	{ CCI_REG16(0x285c), 0x4292 },
	{ CCI_REG16(0x285e), 0x0a0e },
	{ CCI_REG16(0x2860), 0x0a8e },
	{ CCI_REG16(0x2862), 0x0900 },
	{ CCI_REG16(0x2864), 0x7312 },
	{ CCI_REG16(0x2866), 0x4292 },
	{ CCI_REG16(0x2868), 0x0a1e },
	{ CCI_REG16(0x286a), 0x0a9e },
	{ CCI_REG16(0x286c), 0x4130 },
	{ CCI_REG16(0x286e), 0x7400 },
	{ CCI_REG16(0x2870), 0x8058 },
	{ CCI_REG16(0x2872), 0x1807 },
	{ CCI_REG16(0x2874), 0x00e0 },
	{ CCI_REG16(0x2876), 0x7002 },
	{ CCI_REG16(0x2878), 0x17c7 },
	{ CCI_REG16(0x287a), 0x0045 },
	{ CCI_REG16(0x287c), 0x0006 },
	{ CCI_REG16(0x287e), 0x17cc },
	{ CCI_REG16(0x2880), 0x0015 },
	{ CCI_REG16(0x2882), 0x1512 },
	{ CCI_REG16(0x2884), 0x216f },
	{ CCI_REG16(0x2886), 0x005b },
	{ CCI_REG16(0x2888), 0x005d },
	{ CCI_REG16(0x288a), 0x00de },
	{ CCI_REG16(0x288c), 0x00dd },
	{ CCI_REG16(0x288e), 0x5023 },
	{ CCI_REG16(0x2890), 0x00de },
	{ CCI_REG16(0x2892), 0x005b },
	{ CCI_REG16(0x2894), 0x0410 },
	{ CCI_REG16(0x2896), 0x0091 },
	{ CCI_REG16(0x2898), 0x0015 },
	{ CCI_REG16(0x289a), 0x0040 },
	{ CCI_REG16(0x289c), 0x7023 },
	{ CCI_REG16(0x289e), 0x1653 },
	{ CCI_REG16(0x28a0), 0x0156 },
	{ CCI_REG16(0x28a2), 0x0001 },
	{ CCI_REG16(0x28a4), 0x2081 },
	{ CCI_REG16(0x28a6), 0x700e },
	{ CCI_REG16(0x28a8), 0x2f99 },
	{ CCI_REG16(0x28aa), 0x005c },
	{ CCI_REG16(0x28ac), 0x0000 },
	{ CCI_REG16(0x28ae), 0x5040 },
	{ CCI_REG16(0x28b0), 0x0045 },
	{ CCI_REG16(0x28b2), 0x213a },
	{ CCI_REG16(0x28b4), 0x0303 },
	{ CCI_REG16(0x28b6), 0x0148 },
	{ CCI_REG16(0x28b8), 0x0049 },
	{ CCI_REG16(0x28ba), 0x0045 },
	{ CCI_REG16(0x28bc), 0x0046 },
	{ CCI_REG16(0x28be), 0x081d },
	{ CCI_REG16(0x28c0), 0x00de },
	{ CCI_REG16(0x28c2), 0x00dd },
	{ CCI_REG16(0x28c4), 0x00dc },
	{ CCI_REG16(0x28c6), 0x00de },
	{ CCI_REG16(0x28c8), 0x04d6 },
	{ CCI_REG16(0x28ca), 0x2014 },
	{ CCI_REG16(0x28cc), 0x2081 },
	{ CCI_REG16(0x28ce), 0x704e },
	{ CCI_REG16(0x28d0), 0x2f99 },
	{ CCI_REG16(0x28d2), 0x005c },
	{ CCI_REG16(0x28d4), 0x0002 },
	{ CCI_REG16(0x28d6), 0x5060 },
	{ CCI_REG16(0x28d8), 0x31c0 },
	{ CCI_REG16(0x28da), 0x2122 },
	{ CCI_REG16(0x28dc), 0x7800 },
	{ CCI_REG16(0x28de), 0xc08c },
	{ CCI_REG16(0x28e0), 0x0001 },
	{ CCI_REG16(0x28e2), 0x9038 },
	{ CCI_REG16(0x28e4), 0x59f7 },
	{ CCI_REG16(0x28e6), 0x907a },
	{ CCI_REG16(0x28e8), 0x03d8 },
	{ CCI_REG16(0x28ea), 0x8d90 },
	{ CCI_REG16(0x28ec), 0x01c0 },
	{ CCI_REG16(0x28ee), 0x7400 },
	{ CCI_REG16(0x28f0), 0x2002 },
	{ CCI_REG16(0x28f2), 0x70df },
	{ CCI_REG16(0x28f4), 0x3f40 },
	{ CCI_REG16(0x28f6), 0x0240 },
	{ CCI_REG16(0x28f8), 0x7800 },
	{ CCI_REG16(0x28fa), 0x0021 },
	{ CCI_REG16(0x28fc), 0x7400 },
	{ CCI_REG16(0x28fe), 0x0001 },
	{ CCI_REG16(0x2900), 0x70df },
	{ CCI_REG16(0x2902), 0x3f5f },
	{ CCI_REG16(0x2904), 0x7012 },
	{ CCI_REG16(0x2906), 0x2f01 },
	{ CCI_REG16(0x2908), 0x7800 },
	{ CCI_REG16(0x290a), 0x7400 },
	{ CCI_REG16(0x290c), 0x2004 },
	{ CCI_REG16(0x290e), 0x70df },
	{ CCI_REG16(0x2910), 0x3f20 },
	{ CCI_REG16(0x2912), 0x0240 },
	{ CCI_REG16(0x2914), 0x7800 },
	{ CCI_REG16(0x2916), 0x0041 },
	{ CCI_REG16(0x2918), 0x7400 },
	{ CCI_REG16(0x291a), 0x2008 },
	{ CCI_REG16(0x291c), 0x70df },
	{ CCI_REG16(0x291e), 0x3f20 },
	{ CCI_REG16(0x2920), 0x0240 },
	{ CCI_REG16(0x2922), 0x7800 },
	{ CCI_REG16(0x2924), 0x0041 },
	{ CCI_REG16(0x2926), 0x7400 },
	{ CCI_REG16(0x2928), 0x0004 },
	{ CCI_REG16(0x292a), 0x70df },
	{ CCI_REG16(0x292c), 0x3f5f },
	{ CCI_REG16(0x292e), 0x7012 },
	{ CCI_REG16(0x2930), 0x2f01 },
	{ CCI_REG16(0x2932), 0x7800 },
	{ CCI_REG16(0x2934), 0x7400 },
	{ CCI_REG16(0x2936), 0x2010 },
	{ CCI_REG16(0x2938), 0x70df },
	{ CCI_REG16(0x293a), 0x3f40 },
	{ CCI_REG16(0x293c), 0x0240 },
	{ CCI_REG16(0x293e), 0x7800 },
	{ CCI_REG16(0x2940), 0x0000 },
	{ CCI_REG16(0x2942), 0xb86e },
	{ CCI_REG16(0x2944), 0x0000 },
	{ CCI_REG16(0x2946), 0xb86e },
	{ CCI_REG16(0x2948), 0xb8de },
	{ CCI_REG16(0x294a), 0x0002 },
	{ CCI_REG16(0x294c), 0x0063 },
	{ CCI_REG16(0x294e), 0xb90a },
	{ CCI_REG16(0x2950), 0x0063 },
	{ CCI_REG16(0x2952), 0xb8ee },
	{ CCI_REG16(0x2954), 0x0063 },
	{ CCI_REG16(0x2956), 0xb918 },
	{ CCI_REG16(0x2958), 0x0063 },
	{ CCI_REG16(0x295a), 0xb926 },
	{ CCI_REG16(0x295c), 0xb8fa },
	{ CCI_REG16(0x295e), 0x0004 },
	{ CCI_REG16(0x2960), 0x0063 },
	{ CCI_REG16(0x2962), 0xb918 },
	{ CCI_REG16(0x2964), 0x0063 },
	{ CCI_REG16(0x2966), 0xb934 },
	{ CCI_REG16(0x2968), 0x0063 },
	{ CCI_REG16(0x296a), 0xb90a },
	{ CCI_REG16(0x296c), 0x0063 },
	{ CCI_REG16(0x296e), 0xb8fc },
	{ CCI_REG16(0x2970), 0xb8fa },
	{ CCI_REG16(0x2972), 0x0004 },
	{ CCI_REG16(0x2974), 0x0066 },
	{ CCI_REG16(0x2976), 0x0067 },
	{ CCI_REG16(0x2978), 0x00af },
	{ CCI_REG16(0x297a), 0x01cf },
	{ CCI_REG16(0x297c), 0x0087 },
	{ CCI_REG16(0x297e), 0x0083 },
	{ CCI_REG16(0x2980), 0x011b },
	{ CCI_REG16(0x2982), 0x035a },
	{ CCI_REG16(0x2984), 0x00fa },
	{ CCI_REG16(0x2986), 0x00f2 },
	{ CCI_REG16(0x2988), 0x00a6 },
	{ CCI_REG16(0x298a), 0x00a4 },
	{ CCI_REG16(0x298c), 0xffff },
	{ CCI_REG16(0x298e), 0x002d },
	{ CCI_REG16(0x2990), 0x005a },
	{ CCI_REG16(0x2992), 0x0000 },
	{ CCI_REG16(0x2994), 0x0000 },
	{ CCI_REG16(0x2996), 0xb974 },
	{ CCI_REG16(0x2998), 0xb940 },
	{ CCI_REG16(0x299a), 0xb98e },
	{ CCI_REG16(0x299c), 0xb94c },
	{ CCI_REG16(0x299e), 0xb960 },
	{ CCI_REG16(0x29a0), 0xb94c },
	{ CCI_REG16(0x29a2), 0xb960 },
	{ CCI_REG16(0x29a4), 0xb94c },
	{ CCI_REG16(0x29a6), 0xb960 },
	{ CCI_REG16(0x29a8), 0xb94c },
	{ CCI_REG16(0x29aa), 0xb960 },
	{ CCI_REG16(0x29ac), 0xb94c },
	{ CCI_REG16(0x29ae), 0xb960 },
	{ CCI_REG16(0x29b0), 0xb94c },
	{ CCI_REG16(0x29b2), 0xb960 },
	{ CCI_REG16(0x29b4), 0xb94c },
	{ CCI_REG16(0x29b6), 0xb960 },
	{ CCI_REG16(0x29b8), 0xb94c },
	{ CCI_REG16(0x29ba), 0xb960 },
	{ CCI_REG16(0x29bc), 0xb94c },
	{ CCI_REG16(0x29be), 0xb960 },
	{ CCI_REG16(0x29c0), 0xb94c },
	{ CCI_REG16(0x29c2), 0xb960 },
	{ CCI_REG16(0x29c4), 0xb94c },
	{ CCI_REG16(0x29c6), 0xb960 },
	{ CCI_REG16(0x29c8), 0xb94c },
	{ CCI_REG16(0x29ca), 0xb960 },
	{ CCI_REG16(0x29cc), 0xb94c },
	{ CCI_REG16(0x29ce), 0xb960 },
	{ CCI_REG16(0x29d0), 0xb94c },
	{ CCI_REG16(0x29d2), 0xb960 },
	{ CCI_REG16(0x29d4), 0xb94c },
	{ CCI_REG16(0x29d6), 0xb960 },
	{ CCI_REG16(0x29d8), 0xb94c },
	{ CCI_REG16(0x29da), 0xb960 },
	{ CCI_REG16(0x3710), 0x871e },
	{ CCI_REG16(0x3712), 0xb9bc },
	{ CCI_REG16(0x3714), 0xb99a },
	{ CCI_REG16(0x3716), 0xd140 },
	{ CCI_REG16(0x3718), 0xb99c },
	{ CCI_REG16(0x371a), 0xb998 },
	{ CCI_REG16(0x371c), 0x0000 },
	{ CCI_REG16(0x371e), 0x0040 },
	{ CCI_REG16(0x3720), 0x0040 },
	{ CCI_REG16(0x3722), 0x0040 },
	{ CCI_REG16(0x3724), 0x0040 },
	{ CCI_REG16(0x3726), 0x0044 },
	{ CCI_REG16(0x3728), 0x0049 },
	{ CCI_REG16(0x372a), 0x004d },
	{ CCI_REG16(0x372c), 0x0052 },
	{ CCI_REG16(0x372e), 0x0057 },
	{ CCI_REG16(0x3730), 0x005c },
	{ CCI_REG16(0x3732), 0x0062 },
	{ CCI_REG16(0x3734), 0x0068 },
	{ CCI_REG16(0x3736), 0x006e },
	{ CCI_REG16(0x3738), 0x0074 },
	{ CCI_REG16(0x373a), 0x007a },
	{ CCI_REG16(0x373c), 0x0080 },
	{ CCI_REG16(0x373e), 0x0087 },
	{ CCI_REG16(0x3740), 0x008e },
	{ CCI_REG16(0x3742), 0x0095 },
	{ CCI_REG16(0x3744), 0x009c },
	{ CCI_REG16(0x3746), 0x00a4 },
	{ CCI_REG16(0x3748), 0x00ab },
	{ CCI_REG16(0x374a), 0x00b2 },
	{ CCI_REG16(0x374c), 0x00ba },
	{ CCI_REG16(0x374e), 0x00c1 },
	{ CCI_REG16(0x3750), 0x00c7 },
	{ CCI_REG16(0x3752), 0x00cd },
	{ CCI_REG16(0x3754), 0x00d4 },
	{ CCI_REG16(0x3756), 0x00da },
	{ CCI_REG16(0x3758), 0x00e0 },
	{ CCI_REG16(0x375a), 0x00e6 },
	{ CCI_REG16(0x375c), 0x00e6 },
	{ CCI_REG16(0x375e), 0x0000 },
	{ CCI_REG16(0x3760), 0x0000 },
	{ CCI_REG16(0x3762), 0x0000 },
	{ CCI_REG16(0x3764), 0x0000 },
	{ CCI_REG16(0x3766), 0x0000 },
	{ CCI_REG16(0x3768), 0x0000 },
	{ CCI_REG16(0x376a), 0x0000 },
	{ CCI_REG16(0x376c), 0x0000 },
	{ CCI_REG16(0x376e), 0x0000 },
	{ CCI_REG16(0x3770), 0x0000 },
	{ CCI_REG16(0x3772), 0x0000 },
	{ CCI_REG16(0x3774), 0x0000 },
	{ CCI_REG16(0x3776), 0x0000 },
	{ CCI_REG16(0x3778), 0x0000 },
	{ CCI_REG16(0x377a), 0x0000 },
	{ CCI_REG16(0x377c), 0x0000 },
	{ CCI_REG16(0x377e), 0x0000 },
	{ CCI_REG16(0x3780), 0x0000 },
	{ CCI_REG16(0x3782), 0x0000 },
	{ CCI_REG16(0x3784), 0x0000 },
	{ CCI_REG16(0x3786), 0x0000 },
	{ CCI_REG16(0x3788), 0x0000 },
	{ CCI_REG16(0x378a), 0x0000 },
	{ CCI_REG16(0x378c), 0x0000 },
	{ CCI_REG16(0x378e), 0x0000 },
	{ CCI_REG16(0x3790), 0x0000 },
	{ CCI_REG16(0x3792), 0x0000 },
	{ CCI_REG16(0x3794), 0x0000 },
	{ CCI_REG16(0x3796), 0x0000 },
	{ CCI_REG16(0x3798), 0x0000 },
	{ CCI_REG16(0x379a), 0x0000 },
	{ CCI_REG16(0x379c), 0x0000 },
	{ CCI_REG16(0x0268), 0x00eb },
	{ CCI_REG16(0x026a), 0xffff },
	{ CCI_REG16(0x026c), 0x00ff },
	{ CCI_REG16(0x026e), 0x0000 },
	{ CCI_REG16(0x0360), 0x1e8e },
	{ CCI_REG16(0x040e), 0x01eb },
	{ CCI_REG16(0x0600), 0x1130 },
	{ CCI_REG16(0x0602), 0x3112 },
	{ CCI_REG16(0x0604), 0x8048 },
	{ CCI_REG16(0x0606), 0x00e9 },
	{ CCI_REG16(0x0676), 0x07ff },
	{ CCI_REG16(0x0678), 0x0002 },
	{ CCI_REG16(0x067a), 0x0505 },
	{ CCI_REG16(0x067c), 0x0505 },
	{ CCI_REG16(0x06a8), 0x0240 },
	{ CCI_REG16(0x06aa), 0x00ca },
	{ CCI_REG16(0x06ac), 0x0041 },
	{ CCI_REG16(0x06b4), 0x3fff },
	{ CCI_REG16(0x06de), 0x0505 },
	{ CCI_REG16(0x06e0), 0x0505 },
	{ CCI_REG16(0x06e2), 0xff00 },
	{ CCI_REG16(0x06e4), 0x8369 },
	{ CCI_REG16(0x06e6), 0x8369 },
	{ CCI_REG16(0x06e8), 0x8369 },
	{ CCI_REG16(0x06ea), 0x8369 },
	{ CCI_REG16(0x052a), 0x0000 },
	{ CCI_REG16(0x052c), 0x0000 },
	{ CCI_REG16(0x0f06), 0x0002 },
	{ CCI_REG16(0x1060), 0x0f38 },
	{ CCI_REG16(0x1062), 0x1f00 },
	{ CCI_REG16(0x1102), 0x0008 },
	{ CCI_REG16(0x0a04), 0xb4c5 },
	{ CCI_REG16(0x0a06), 0xc400 },
	{ CCI_REG16(0x0a08), 0x988a },
	{ CCI_REG16(0x0a0a), 0xf386 },
	{ CCI_REG16(0x0a0e), 0xeec0 },
	{ CCI_REG16(0x0a12), 0x0000 },
	{ CCI_REG16(0x0a18), 0x0010 },
	{ CCI_REG16(0x0a1e), 0x000f },
	{ CCI_REG16(0x0a20), 0x0015 },
	{ CCI_REG16(0x0c00), 0x0021 },
	{ CCI_REG16(0x0c16), 0x0002 },
	{ CCI_REG16(0x0708), 0x6fc0 },
	{ CCI_REG16(0x070c), 0x0000 },
	{ CCI_REG16(0x0780), 0x010f },
	{ CCI_REG16(0x120c), 0x1428 },
	{ CCI_REG16(0x121a), 0x0000 },
	{ CCI_REG16(0x121c), 0x1896 },
	{ CCI_REG16(0x121e), 0x0032 },
	{ CCI_REG16(0x1220), 0x0000 },
	{ CCI_REG16(0x1222), 0x96ff },
	{ CCI_REG16(0x1244), 0x0000 },
	{ CCI_REG16(0x1246), 0x012c },
	{ CCI_REG16(0x105c), 0x0f0b },
	{ CCI_REG16(0x1958), 0x003f },
	{ CCI_REG16(0x195a), 0x004c },
	{ CCI_REG16(0x195c), 0x0097 },
	{ CCI_REG16(0x195e), 0x0221 },
	{ CCI_REG16(0x1960), 0x03ff },
	{ CCI_REG16(0x1980), 0x007d },
	{ CCI_REG16(0x1982), 0x0028 },
	{ CCI_REG16(0x1984), 0x2018 },
	{ CCI_REG16(0x1986), 0x0010 },
	{ CCI_REG16(0x1988), 0x0000 },
	{ CCI_REG16(0x198a), 0x0000 },
	{ CCI_REG16(0x198c), 0x0428 },
	{ CCI_REG16(0x198e), 0x0000 },
	{ CCI_REG16(0x1990), 0x1b33 },
	{ CCI_REG16(0x1992), 0x0000 },
	{ CCI_REG16(0x1994), 0x3000 },
	{ CCI_REG16(0x1996), 0x0002 },
	{ CCI_REG16(0x1962), 0x003f },
	{ CCI_REG16(0x1964), 0x004c },
	{ CCI_REG16(0x1966), 0x0097 },
	{ CCI_REG16(0x1968), 0x0221 },
	{ CCI_REG16(0x196a), 0x03ff },
	{ CCI_REG16(0x19c0), 0x007d },
	{ CCI_REG16(0x19c2), 0x0028 },
	{ CCI_REG16(0x19c4), 0x2018 },
	{ CCI_REG16(0x19c6), 0x0010 },
	{ CCI_REG16(0x19c8), 0x0000 },
	{ CCI_REG16(0x19ca), 0x0000 },
	{ CCI_REG16(0x19cc), 0x0428 },
	{ CCI_REG16(0x19ce), 0x0000 },
	{ CCI_REG16(0x19d0), 0x1b33 },
	{ CCI_REG16(0x19d2), 0x0000 },
	{ CCI_REG16(0x19d4), 0x3000 },
	{ CCI_REG16(0x19d6), 0x0002 },
	{ CCI_REG16(0x196c), 0x003f },
	{ CCI_REG16(0x196e), 0x004c },
	{ CCI_REG16(0x1970), 0x0097 },
	{ CCI_REG16(0x1972), 0x0221 },
	{ CCI_REG16(0x1974), 0x03ff },
	{ CCI_REG16(0x1a00), 0x007d },
	{ CCI_REG16(0x1a02), 0x0028 },
	{ CCI_REG16(0x1a04), 0x2018 },
	{ CCI_REG16(0x1a06), 0x0010 },
	{ CCI_REG16(0x1a08), 0x0000 },
	{ CCI_REG16(0x1a0a), 0x0000 },
	{ CCI_REG16(0x1a0c), 0x0428 },
	{ CCI_REG16(0x1a0e), 0x0000 },
	{ CCI_REG16(0x1a10), 0x1b33 },
	{ CCI_REG16(0x1a12), 0x0000 },
	{ CCI_REG16(0x1a14), 0x3000 },
	{ CCI_REG16(0x1a16), 0x0002 },
	{ CCI_REG16(0x1976), 0x003f },
	{ CCI_REG16(0x1978), 0x004c },
	{ CCI_REG16(0x197a), 0x0097 },
	{ CCI_REG16(0x197c), 0x0221 },
	{ CCI_REG16(0x197e), 0x03ff },
	{ CCI_REG16(0x1a40), 0x007d },
	{ CCI_REG16(0x1a42), 0x0028 },
	{ CCI_REG16(0x1a44), 0x2018 },
	{ CCI_REG16(0x1a46), 0x0010 },
	{ CCI_REG16(0x1a48), 0x0000 },
	{ CCI_REG16(0x1a4a), 0x0000 },
	{ CCI_REG16(0x1a4c), 0x0428 },
	{ CCI_REG16(0x1a4e), 0x0000 },
	{ CCI_REG16(0x1a50), 0x1b33 },
	{ CCI_REG16(0x1a52), 0x0000 },
	{ CCI_REG16(0x1a54), 0x3000 },
	{ CCI_REG16(0x1a56), 0x0002 },
	{ CCI_REG16(0x027e), 0x0100 },
	{ CCI_REG16(0x0c34), 0x0300 },
};
/* 4208x3120 RAW10, 4 lanes, 30.2 fps (pixel rate 576 MHz, link 720 MHz) */
static const struct cci_reg_sequence hi1337_4208x3120_regs[] = {
	{ CCI_REG16(0x0b00), 0x0000 },
	{ CCI_REG16(0x0204), 0x0000 },
	{ CCI_REG16(0x0206), 0x02d0 },
	{ CCI_REG16(0x020a), 0x0ceb },
	{ CCI_REG16(0x020e), 0x0cef },
	{ CCI_REG16(0x0214), 0x0200 },
	{ CCI_REG16(0x0216), 0x0200 },
	{ CCI_REG16(0x0218), 0x0200 },
	{ CCI_REG16(0x021a), 0x0200 },
	{ CCI_REG16(0x0224), 0x002e },
	{ CCI_REG16(0x022a), 0x0017 },
	{ CCI_REG16(0x022c), 0x0e1f },
	{ CCI_REG16(0x022e), 0x0c61 },
	{ CCI_REG16(0x0234), 0x1111 },
	{ CCI_REG16(0x0236), 0x1111 },
	{ CCI_REG16(0x0238), 0x1111 },
	{ CCI_REG16(0x023a), 0x1111 },
	{ CCI_REG16(0x0248), 0x0100 },
	{ CCI_REG16(0x0250), 0x0000 },
	{ CCI_REG16(0x0252), 0x0006 },
	{ CCI_REG16(0x0254), 0x0000 },
	{ CCI_REG16(0x0256), 0x0000 },
	{ CCI_REG16(0x0258), 0x0000 },
	{ CCI_REG16(0x025a), 0x0000 },
	{ CCI_REG16(0x025c), 0x0000 },
	{ CCI_REG16(0x025e), 0x0202 },
	{ CCI_REG16(0x0440), 0x0032 },
	{ CCI_REG16(0x0f00), 0x0000 },
	{ CCI_REG16(0x0f04), 0x0008 },
	{ CCI_REG16(0x0b02), 0x0100 },
	{ CCI_REG16(0x0b04), 0x00dc },
	{ CCI_REG16(0x0b12), 0x1070 },
	{ CCI_REG16(0x0b14), 0x0c30 },
	{ CCI_REG16(0x0b20), 0x0100 },
	{ CCI_REG16(0x1100), 0x1100 },
	{ CCI_REG16(0x1108), 0x0202 },
	{ CCI_REG16(0x1118), 0x0000 },
	{ CCI_REG16(0x0a10), 0xb040 },
	{ CCI_REG16(0x0c14), 0x0008 },
	{ CCI_REG16(0x0c18), 0x1070 },
	{ CCI_REG16(0x0c1a), 0x0c30 },
	{ CCI_REG16(0x0730), 0x0001 },
	{ CCI_REG16(0x0732), 0x0000 },
	{ CCI_REG16(0x0734), 0x0300 },
	{ CCI_REG16(0x0736), 0x005a },
	{ CCI_REG16(0x0738), 0x0002 },
	{ CCI_REG16(0x073c), 0x0700 },
	{ CCI_REG16(0x0740), 0x0000 },
	{ CCI_REG16(0x0742), 0x0000 },
	{ CCI_REG16(0x0744), 0x0300 },
	{ CCI_REG16(0x0746), 0x0096 },
	{ CCI_REG16(0x0748), 0x0001 },
	{ CCI_REG16(0x074a), 0x0900 },
	{ CCI_REG16(0x074c), 0x0000 },
	{ CCI_REG16(0x074e), 0x0100 },
	{ CCI_REG16(0x0750), 0x0000 },
	{ CCI_REG16(0x1200), 0x0926 },
	{ CCI_REG16(0x1202), 0x0a00 },
	{ CCI_REG16(0x120e), 0x6027 },
	{ CCI_REG16(0x1210), 0x8027 },
	{ CCI_REG16(0x1000), 0x0300 },
	{ CCI_REG16(0x1002), 0xc311 },
	{ CCI_REG16(0x1004), 0x2bb0 },
	{ CCI_REG16(0x1010), 0x0d39 },
	{ CCI_REG16(0x1012), 0x0181 },
	{ CCI_REG16(0x1014), 0x0020 },
	{ CCI_REG16(0x1016), 0x0020 },
	{ CCI_REG16(0x101a), 0x0020 },
	{ CCI_REG16(0x1020), 0xc10b },
	{ CCI_REG16(0x1022), 0x0c31 },
	{ CCI_REG16(0x1024), 0x030c },
	{ CCI_REG16(0x1026), 0x1410 },
	{ CCI_REG16(0x1028), 0x1c0e },
	{ CCI_REG16(0x102a), 0x140a },
	{ CCI_REG16(0x102c), 0x2200 },
	{ CCI_REG16(0x1038), 0x0000 },
	{ CCI_REG16(0x103e), 0x0001 },
	{ CCI_REG16(0x1042), 0x0008 },
	{ CCI_REG16(0x1044), 0x0120 },
	{ CCI_REG16(0x1046), 0x01b0 },
	{ CCI_REG16(0x1048), 0x0090 },
	{ CCI_REG16(0x1066), 0x0d75 },
	{ CCI_REG16(0x1600), 0x0000 },
	{ CCI_REG16(0x1608), 0x0020 },
	{ CCI_REG16(0x160a), 0x1200 },
	{ CCI_REG16(0x160c), 0x001a },
	{ CCI_REG16(0x160e), 0x0d80 },
};
/* 2104x1560 RAW10 (2x2 binning), 4 lanes, 30.2 fps (pixel rate 288 MHz, link 360 MHz) */
static const struct cci_reg_sequence hi1337_2104x1560_regs[] = {
	{ CCI_REG16(0x0b00), 0x0000 },
	{ CCI_REG16(0x0204), 0x0200 },
	{ CCI_REG16(0x0206), 0x02d0 },
	{ CCI_REG16(0x020a), 0x0ceb },
	{ CCI_REG16(0x020e), 0x0cef },
	{ CCI_REG16(0x0214), 0x0200 },
	{ CCI_REG16(0x0216), 0x0200 },
	{ CCI_REG16(0x0218), 0x0200 },
	{ CCI_REG16(0x021a), 0x0200 },
	{ CCI_REG16(0x0224), 0x002c },
	{ CCI_REG16(0x022a), 0x0015 },
	{ CCI_REG16(0x022c), 0x0e2d },
	{ CCI_REG16(0x022e), 0x0c61 },
	{ CCI_REG16(0x0234), 0x3311 },
	{ CCI_REG16(0x0236), 0x3311 },
	{ CCI_REG16(0x0238), 0x3311 },
	{ CCI_REG16(0x023a), 0x2222 },
	{ CCI_REG16(0x0248), 0x0100 },
	{ CCI_REG16(0x0250), 0x0000 },
	{ CCI_REG16(0x0252), 0x0006 },
	{ CCI_REG16(0x0254), 0x0000 },
	{ CCI_REG16(0x0256), 0x0000 },
	{ CCI_REG16(0x0258), 0x0000 },
	{ CCI_REG16(0x025a), 0x0000 },
	{ CCI_REG16(0x025c), 0x0000 },
	{ CCI_REG16(0x025e), 0x0202 },
	{ CCI_REG16(0x0440), 0x0032 },
	{ CCI_REG16(0x0f00), 0x0400 },
	{ CCI_REG16(0x0f04), 0x0004 },
	{ CCI_REG16(0x0b02), 0x0100 },
	{ CCI_REG16(0x0b04), 0x00fc },
	{ CCI_REG16(0x0b12), 0x0838 },
	{ CCI_REG16(0x0b14), 0x0618 },
	{ CCI_REG16(0x0b20), 0x0200 },
	{ CCI_REG16(0x1100), 0x1100 },
	{ CCI_REG16(0x1108), 0x0402 },
	{ CCI_REG16(0x1118), 0x0000 },
	{ CCI_REG16(0x0a10), 0xb070 },
	{ CCI_REG16(0x0c14), 0x0008 },
	{ CCI_REG16(0x0c18), 0x1070 },
	{ CCI_REG16(0x0c1a), 0x0618 },
	{ CCI_REG16(0x0730), 0x0001 },
	{ CCI_REG16(0x0732), 0x0000 },
	{ CCI_REG16(0x0734), 0x0300 },
	{ CCI_REG16(0x0736), 0x005a },
	{ CCI_REG16(0x0738), 0x0002 },
	{ CCI_REG16(0x073c), 0x0700 },
	{ CCI_REG16(0x0740), 0x0000 },
	{ CCI_REG16(0x0742), 0x0000 },
	{ CCI_REG16(0x0744), 0x0300 },
	{ CCI_REG16(0x0746), 0x0096 },
	{ CCI_REG16(0x0748), 0x0001 },
	{ CCI_REG16(0x074a), 0x0900 },
	{ CCI_REG16(0x074c), 0x0100 },
	{ CCI_REG16(0x074e), 0x0100 },
	{ CCI_REG16(0x0750), 0x0000 },
	{ CCI_REG16(0x1200), 0x0926 },
	{ CCI_REG16(0x1202), 0x0a00 },
	{ CCI_REG16(0x120e), 0x6027 },
	{ CCI_REG16(0x1210), 0x8027 },
	{ CCI_REG16(0x1000), 0x0300 },
	{ CCI_REG16(0x1002), 0xc311 },
	{ CCI_REG16(0x1004), 0x2bb0 },
	{ CCI_REG16(0x1010), 0x0690 },
	{ CCI_REG16(0x1012), 0x00b4 },
	{ CCI_REG16(0x1014), 0x0020 },
	{ CCI_REG16(0x1016), 0x0020 },
	{ CCI_REG16(0x101a), 0x0020 },
	{ CCI_REG16(0x1020), 0xc106 },
	{ CCI_REG16(0x1022), 0x0618 },
	{ CCI_REG16(0x1024), 0x0306 },
	{ CCI_REG16(0x1026), 0x0a0a },
	{ CCI_REG16(0x1028), 0x1008 },
	{ CCI_REG16(0x102a), 0x0a05 },
	{ CCI_REG16(0x102c), 0x1200 },
	{ CCI_REG16(0x1038), 0x0000 },
	{ CCI_REG16(0x103e), 0x0101 },
	{ CCI_REG16(0x1042), 0x0008 },
	{ CCI_REG16(0x1044), 0x0120 },
	{ CCI_REG16(0x1046), 0x01b0 },
	{ CCI_REG16(0x1048), 0x0090 },
	{ CCI_REG16(0x1066), 0x06ae },
	{ CCI_REG16(0x1600), 0x0400 },
	{ CCI_REG16(0x1608), 0x0020 },
	{ CCI_REG16(0x160a), 0x1200 },
	{ CCI_REG16(0x160c), 0x001a },
	{ CCI_REG16(0x160e), 0x0d80 },
};
#define HI1337_LINK_FREQ_720MHZ		720000000ULL
#define HI1337_LINK_FREQ_360MHZ		360000000ULL

static const s64 hi1337_link_freqs[] = {
	HI1337_LINK_FREQ_720MHZ,
	HI1337_LINK_FREQ_360MHZ,
};

struct hi1337_mode {
	u32 width;
	u32 height;
	u32 hts;		/* line length in pixels: CamX lineLengthPclk x pixel_rate / 72 MHz */
	u32 vts;		/* default frame length in lines (30.2 fps) */
	u64 pixel_rate;
	unsigned int link_freq_index;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

static const struct hi1337_mode hi1337_modes[] = {
	{
		.width = 4208, .height = 3120, .hts = 5760, .vts = 3311,
		.pixel_rate = 576000000, .link_freq_index = 0,
		.regs = hi1337_4208x3120_regs, .num_regs = ARRAY_SIZE(hi1337_4208x3120_regs),
	},
	{
		.width = 2104, .height = 1560, .hts = 2880, .vts = 3311,
		.pixel_rate = 288000000, .link_freq_index = 1,
		.regs = hi1337_2104x1560_regs, .num_regs = ARRAY_SIZE(hi1337_2104x1560_regs),
	},
};

struct hi1337 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *clk;
	struct gpio_desc *reset_gpio;
	struct mux_control *mux;	/* CAM_SEL: CSIPHY1 board mux, state 1 = ultrawide */
	struct regulator_bulk_data supplies[ARRAY_SIZE(hi1337_supply_names)];
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;
	const struct hi1337_mode *mode;
};

static inline struct hi1337 *to_hi1337(struct v4l2_subdev *sd)
{
	return container_of(sd, struct hi1337, sd);
}

static int hi1337_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hi1337 *hi1337 = container_of(ctrl->handler, struct hi1337, ctrls);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = hi1337->mode->height + ctrl->val - HI1337_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(hi1337->exposure, hi1337->exposure->minimum, max,
					 hi1337->exposure->step,
					 clamp_t(s64, hi1337->exposure->default_value,
						 hi1337->exposure->minimum, max));
	}

	if (!pm_runtime_get_if_active(hi1337->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(hi1337->regmap, HI1337_REG_GROUP_HOLD, 0x0100, &ret);
		cci_write(hi1337->regmap, HI1337_REG_EXPOSURE, ctrl->val, &ret);
		cci_write(hi1337->regmap, HI1337_REG_GROUP_HOLD, 0x0000, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(hi1337->regmap, HI1337_REG_GROUP_HOLD, 0x0100, &ret);
		cci_write(hi1337->regmap, HI1337_REG_ANALOG_GAIN, ctrl->val, &ret);
		cci_write(hi1337->regmap, HI1337_REG_GROUP_HOLD, 0x0000, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(hi1337->regmap, HI1337_REG_FLL, hi1337->mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		if (ctrl->val) {
			cci_update_bits(hi1337->regmap, HI1337_REG_ISP, HI1337_ISP_TPG_EN,
					HI1337_ISP_TPG_EN, &ret);
			cci_write(hi1337->regmap, HI1337_REG_TEST_PATTERN, ctrl->val - 1, &ret);
		} else {
			cci_update_bits(hi1337->regmap, HI1337_REG_ISP, HI1337_ISP_TPG_EN, 0, &ret);
		}
		break;
	default:
		break;
	}

	pm_runtime_put(hi1337->dev);
	return ret;
}

static const struct v4l2_ctrl_ops hi1337_ctrl_ops = {
	.s_ctrl = hi1337_set_ctrl,
};

static void hi1337_update_mode_ctrls(struct hi1337 *hi1337)
{
	const struct hi1337_mode *m = hi1337->mode;
	s64 vblank = m->vts - m->height;
	s64 hblank = m->hts - m->width;

	__v4l2_ctrl_s_ctrl(hi1337->link_freq, m->link_freq_index);
	__v4l2_ctrl_s_ctrl_int64(hi1337->pixel_rate, m->pixel_rate);
	__v4l2_ctrl_modify_range(hi1337->hblank, hblank, hblank, 1, hblank);
	__v4l2_ctrl_modify_range(hi1337->vblank, vblank, HI1337_FLL_MAX - m->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(hi1337->vblank, vblank);
}

static int hi1337_init_controls(struct hi1337 *hi1337)
{
	struct v4l2_ctrl_handler *hdl = &hi1337->ctrls;
	const struct hi1337_mode *m = hi1337->mode;
	struct v4l2_fwnode_device_properties props;
	int ret;

	v4l2_ctrl_handler_init(hdl, 12);

	hi1337->link_freq = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(hi1337_link_freqs) - 1,
						   m->link_freq_index, hi1337_link_freqs);
	if (hi1337->link_freq)
		hi1337->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	hi1337->pixel_rate = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE, 1, 576000000, 1,
					       m->pixel_rate);
	hi1337->hblank = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_HBLANK, m->hts - m->width,
					   m->hts - m->width, 1, m->hts - m->width);
	if (hi1337->hblank)
		hi1337->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	hi1337->vblank = v4l2_ctrl_new_std(hdl, &hi1337_ctrl_ops, V4L2_CID_VBLANK,
					   m->vts - m->height, HI1337_FLL_MAX - m->height, 1,
					   m->vts - m->height);
	hi1337->exposure = v4l2_ctrl_new_std(hdl, &hi1337_ctrl_ops, V4L2_CID_EXPOSURE,
					     HI1337_EXPOSURE_MIN, m->vts - HI1337_EXPOSURE_MARGIN, 1,
					     m->vts - HI1337_EXPOSURE_MARGIN);
	v4l2_ctrl_new_std(hdl, &hi1337_ctrl_ops, V4L2_CID_ANALOGUE_GAIN, HI1337_GAIN_MIN,
			  HI1337_GAIN_MAX, 1, HI1337_GAIN_MIN);
	v4l2_ctrl_new_std_menu_items(hdl, &hi1337_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(hi1337_test_pattern_menu) - 1, 0, 0,
				     hi1337_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(hi1337->dev, &props);
	if (ret)
		return ret;
	v4l2_ctrl_new_fwnode_properties(hdl, &hi1337_ctrl_ops, &props);

	if (hdl->error)
		return hdl->error;

	hi1337->sd.ctrl_handler = hdl;
	return 0;
}

static void hi1337_fill_format(const struct hi1337_mode *m, struct v4l2_mbus_framefmt *fmt)
{
	memset(fmt, 0, sizeof(*fmt));
	fmt->width = m->width;
	fmt->height = m->height;
	fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;	/* as the Hi-847 */
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int hi1337_enable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct hi1337 *hi1337 = to_hi1337(sd);
	int ret;

	ret = pm_runtime_resume_and_get(hi1337->dev);
	if (ret)
		return ret;

	/* cci_multi_reg_write(), not regmap_multi_reg_write(): the tables hold 16-bit values */
	cci_multi_reg_write(hi1337->regmap, hi1337_init_regs, ARRAY_SIZE(hi1337_init_regs), &ret);
	cci_multi_reg_write(hi1337->regmap, hi1337->mode->regs, hi1337->mode->num_regs, &ret);
	if (!ret)
		ret = __v4l2_ctrl_handler_setup(&hi1337->ctrls);
	if (!ret)
		cci_write(hi1337->regmap, HI1337_REG_MODE_SELECT, 0x0100, &ret);
	if (ret) {
		dev_err(hi1337->dev, "failed to start streaming: %d\n", ret);
		pm_runtime_put(hi1337->dev);
	}
	return ret;
}

static int hi1337_disable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct hi1337 *hi1337 = to_hi1337(sd);
	int ret = 0;

	cci_write(hi1337->regmap, HI1337_REG_MODE_SELECT, 0x0000, &ret);
	pm_runtime_put(hi1337->dev);
	return ret;
}

static int hi1337_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int hi1337_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(hi1337_modes) || fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = hi1337_modes[fse->index].width;
	fse->min_height = fse->max_height = hi1337_modes[fse->index].height;
	return 0;
}

static int hi1337_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct hi1337 *hi1337 = to_hi1337(sd);
	const struct hi1337_mode *m;

	m = v4l2_find_nearest_size(hi1337_modes, ARRAY_SIZE(hi1337_modes), width, height,
				   fmt->format.width, fmt->format.height);
	hi1337_fill_format(m, &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		hi1337->mode = m;
		hi1337_update_mode_ctrls(hi1337);
	}
	return 0;
}

static int hi1337_get_selection(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = HI1337_NATIVE_WIDTH;
		sel->r.height = HI1337_NATIVE_HEIGHT;
		return 0;
	}
	return -EINVAL;
}

static const struct v4l2_subdev_video_ops hi1337_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops hi1337_pad_ops = {
	.enum_mbus_code = hi1337_enum_mbus_code,
	.enum_frame_size = hi1337_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = hi1337_set_fmt,
	.get_selection = hi1337_get_selection,
	.enable_streams = hi1337_enable_streams,
	.disable_streams = hi1337_disable_streams,
};

static const struct v4l2_subdev_ops hi1337_ops = {
	.video = &hi1337_video_ops,
	.pad = &hi1337_pad_ops,
};

static int hi1337_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	hi1337_fill_format(&hi1337_modes[0], v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_internal_ops hi1337_internal_ops = {
	.init_state = hi1337_init_state,
};

/* CamX power-up: STANDBY(CAM_SEL)=1, RESET=0 (1 ms), VIO, MCLK (1 ms), RESET=1 (1 ms) */
static int hi1337_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hi1337 *hi1337 = to_hi1337(sd);
	int ret;

	if (hi1337->mux) {
		ret = mux_control_try_select(hi1337->mux, HI1337_MUX_STATE);
		if (ret) {
			dev_err(dev, "CSIPHY1 mux busy (the front camera is on): %d\n", ret);
			return ret;
		}
	}
	gpiod_set_value_cansleep(hi1337->reset_gpio, 1);
	fsleep(1000);

	ret = regulator_bulk_enable(ARRAY_SIZE(hi1337->supplies), hi1337->supplies);
	if (ret)
		goto err_mux;
	ret = clk_prepare_enable(hi1337->clk);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(hi1337->supplies), hi1337->supplies);
		goto err_mux;
	}
	fsleep(1000);
	gpiod_set_value_cansleep(hi1337->reset_gpio, 0);
	fsleep(5000);
	return 0;

err_mux:
	if (hi1337->mux)
		mux_control_deselect(hi1337->mux);
	return ret;
}

static int hi1337_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hi1337 *hi1337 = to_hi1337(sd);

	gpiod_set_value_cansleep(hi1337->reset_gpio, 1);
	fsleep(1000);
	clk_disable_unprepare(hi1337->clk);
	regulator_bulk_disable(ARRAY_SIZE(hi1337->supplies), hi1337->supplies);
	if (hi1337->mux)
		mux_control_deselect(hi1337->mux);
	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(hi1337_pm_ops, hi1337_power_off, hi1337_power_on, NULL);

static int hi1337_identify(struct hi1337 *hi1337)
{
	u64 id = 0;
	int ret = 0;

	cci_read(hi1337->regmap, HI1337_REG_CHIP_ID, &id, &ret);
	if (ret)
		return dev_err_probe(hi1337->dev, ret, "failed to read the chip id\n");
	if (id != HI1337_CHIP_ID)
		return dev_err_probe(hi1337->dev, -ENODEV, "unexpected chip id 0x%04llx\n", id);
	return 0;
}

static int hi1337_check_hwcfg(struct hi1337 *hi1337)
{
	struct v4l2_fwnode_endpoint bus_cfg = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	unsigned long link_freq_bitmap;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(hi1337->dev), 0, 0, 0);
	if (!ep)
		return dev_err_probe(hi1337->dev, -EPROBE_DEFER, "waiting for the endpoint\n");
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(hi1337->dev, ret, "parsing the endpoint failed\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != HI1337_DATA_LANES)
		ret = dev_err_probe(hi1337->dev, -EINVAL, "%u data lanes, need %u\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes, HI1337_DATA_LANES);
	if (!ret)
		ret = v4l2_link_freq_to_bitmap(hi1337->dev, bus_cfg.link_frequencies,
					       bus_cfg.nr_of_link_frequencies, hi1337_link_freqs,
					       ARRAY_SIZE(hi1337_link_freqs), &link_freq_bitmap);
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int hi1337_probe(struct i2c_client *client)
{
	struct hi1337 *hi1337;
	unsigned int i;
	int ret;

	hi1337 = devm_kzalloc(&client->dev, sizeof(*hi1337), GFP_KERNEL);
	if (!hi1337)
		return -ENOMEM;
	hi1337->dev = &client->dev;
	hi1337->mode = &hi1337_modes[0];

	ret = hi1337_check_hwcfg(hi1337);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&hi1337->sd, client, &hi1337_ops);

	hi1337->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(hi1337->regmap))
		return PTR_ERR(hi1337->regmap);

	hi1337->clk = devm_v4l2_sensor_clk_get(hi1337->dev, NULL);
	if (IS_ERR(hi1337->clk))
		return dev_err_probe(hi1337->dev, PTR_ERR(hi1337->clk), "getting the clock\n");
	if (clk_get_rate(hi1337->clk) != HI1337_MCLK)
		dev_warn(hi1337->dev, "clock is %lu Hz, the tables expect %u\n",
			 clk_get_rate(hi1337->clk), HI1337_MCLK);

	hi1337->reset_gpio = devm_gpiod_get(hi1337->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(hi1337->reset_gpio))
		return dev_err_probe(hi1337->dev, PTR_ERR(hi1337->reset_gpio), "getting the reset gpio\n");
	/* the board mux in front of CSIPHY1, shared with the front camera (s5k3t2); optional */
	hi1337->mux = devm_mux_control_get(hi1337->dev, NULL);
	if (IS_ERR(hi1337->mux)) {
		if (PTR_ERR(hi1337->mux) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		hi1337->mux = NULL;
	}

	for (i = 0; i < ARRAY_SIZE(hi1337_supply_names); i++)
		hi1337->supplies[i].supply = hi1337_supply_names[i];
	ret = devm_regulator_bulk_get(hi1337->dev, ARRAY_SIZE(hi1337->supplies), hi1337->supplies);
	if (ret)
		return dev_err_probe(hi1337->dev, ret, "getting the supplies\n");

	ret = hi1337_power_on(hi1337->dev);
	if (ret)
		return ret;

	ret = hi1337_identify(hi1337);
	if (ret)
		goto err_power_off;

	ret = hi1337_init_controls(hi1337);
	if (ret)
		goto err_ctrls;

	hi1337->sd.internal_ops = &hi1337_internal_ops;
	hi1337->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	hi1337->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	hi1337->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&hi1337->sd.entity, 1, &hi1337->pad);
	if (ret)
		goto err_ctrls;

	hi1337->sd.state_lock = hi1337->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&hi1337->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(hi1337->dev);
	pm_runtime_enable(hi1337->dev);
	pm_runtime_set_autosuspend_delay(hi1337->dev, 1000);
	pm_runtime_use_autosuspend(hi1337->dev);

	ret = v4l2_async_register_subdev_sensor(&hi1337->sd);
	if (ret)
		goto err_pm;

	pm_runtime_idle(hi1337->dev);
	dev_info(hi1337->dev, "Hi-1337 found (chip id 0x%04x)\n", HI1337_CHIP_ID);
	return 0;

err_pm:
	pm_runtime_disable(hi1337->dev);
	pm_runtime_set_suspended(hi1337->dev);
	v4l2_subdev_cleanup(&hi1337->sd);
err_entity:
	media_entity_cleanup(&hi1337->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&hi1337->ctrls);
err_power_off:
	hi1337_power_off(hi1337->dev);
	return ret;
}

static void hi1337_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct hi1337 *hi1337 = to_hi1337(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&hi1337->ctrls);
	pm_runtime_disable(hi1337->dev);
	if (!pm_runtime_status_suspended(hi1337->dev)) {
		hi1337_power_off(hi1337->dev);
		pm_runtime_set_suspended(hi1337->dev);
	}
}

static const struct of_device_id hi1337_of_match[] = {
	{ .compatible = "hynix,hi1337" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi1337_of_match);

static struct i2c_driver hi1337_i2c_driver = {
	.driver = {
		.name = "hi1337",
		.of_match_table = hi1337_of_match,
		.pm = pm_ptr(&hi1337_pm_ops),
	},
	.probe = hi1337_probe,
	.remove = hi1337_remove,
};
module_i2c_driver(hi1337_i2c_driver);

MODULE_DESCRIPTION("SK Hynix Hi-1337 camera sensor driver");
MODULE_LICENSE("GPL");
