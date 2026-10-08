// SPDX-License-Identifier: GPL-2.0
/*
 * Will Semiconductor WL2866D camera power IC: two DVDD and two AVDD LDOs behind I2C.
 *
 * No public datasheet. Layout from Xiaomi's surya driver (wl2866d.c): output voltage codes in
 * 0x03 (DVDD1), 0x04 (DVDD2), 0x05 (AVDD1), 0x06 (AVDD2); enable bits 0..3 of 0x0e in the same
 * order. The vendor codes fit DVDD = 0.6 V + code * 6 mV (0x55 = 1.11 V) and
 * AVDD = 1.2 V + code * 12.5 mV (0x80 = 2.8 V, 0x88 = 2.9 V), the rails those sensors need.
 */

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define WL2866D_REG_ENABLE	0x0e

enum { WL2866D_DVDD1, WL2866D_DVDD2, WL2866D_AVDD1, WL2866D_AVDD2, WL2866D_NUM };

static const struct regulator_ops wl2866d_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

#define WL2866D_LDO(_id, _name, _vsel, _min, _step)			\
	[_id] = {							\
		.name = _name,						\
		.of_match = of_match_ptr(_name),			\
		.regulators_node = of_match_ptr("regulators"),		\
		.id = _id,						\
		.ops = &wl2866d_ops,					\
		.type = REGULATOR_VOLTAGE,				\
		.owner = THIS_MODULE,					\
		.n_voltages = 256,					\
		.min_uV = _min,						\
		.uV_step = _step,					\
		.vsel_reg = _vsel,					\
		.vsel_mask = 0xff,					\
		.enable_reg = WL2866D_REG_ENABLE,			\
		.enable_mask = BIT(_id),				\
		.enable_time = 1000,					\
	}

static const struct regulator_desc wl2866d_desc[WL2866D_NUM] = {
	WL2866D_LDO(WL2866D_DVDD1, "dvdd1", 0x03, 600000, 6000),
	WL2866D_LDO(WL2866D_DVDD2, "dvdd2", 0x04, 600000, 6000),
	WL2866D_LDO(WL2866D_AVDD1, "avdd1", 0x05, 1200000, 12500),
	WL2866D_LDO(WL2866D_AVDD2, "avdd2", 0x06, 1200000, 12500),
};

static const struct regmap_config wl2866d_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x0f,
};

static int wl2866d_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config cfg = { .dev = dev };
	struct gpio_desc *enable;
	unsigned int i;

	/* the chip enable (pm6150l gpio1 on surya); optional, it may be strapped or left on */
	enable = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(enable))
		return dev_err_probe(dev, PTR_ERR(enable), "getting the enable gpio\n");
	if (enable)
		fsleep(1000);

	cfg.regmap = devm_regmap_init_i2c(client, &wl2866d_regmap);
	if (IS_ERR(cfg.regmap))
		return PTR_ERR(cfg.regmap);

	for (i = 0; i < WL2866D_NUM; i++) {
		struct regulator_dev *rdev = devm_regulator_register(dev, &wl2866d_desc[i], &cfg);

		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "registering %s\n", wl2866d_desc[i].name);
	}
	return 0;
}

static const struct of_device_id wl2866d_of_match[] = {
	{ .compatible = "willsemi,wl2866d" },
	{ }
};
MODULE_DEVICE_TABLE(of, wl2866d_of_match);

static struct i2c_driver wl2866d_driver = {
	.driver = {
		.name = "wl2866d",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = wl2866d_of_match,
	},
	.probe = wl2866d_probe,
};
module_i2c_driver(wl2866d_driver);

MODULE_DESCRIPTION("Will Semiconductor WL2866D camera LDO driver");
MODULE_LICENSE("GPL");
