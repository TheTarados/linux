// SPDX-License-Identifier: GPL-2.0-only
/*
 * PLL clock driver for the Mobileye EyeQ5, EyeQ6L and EyeQ6H platforms.
 *
 * This controller handles read-only PLLs, all derived from the same main
 * crystal clock. It also exposes divider clocks, those are children to PLLs.
 * Parent clock is expected to be constant. This driver's registers live in
 * a shared region called OLB. Some PLLs are initialised early by of_clk_init();
 * if so, two clk providers are registered.
 *
 * We use eqc_ as prefix, as-in "EyeQ Clock", but way shorter.
 *
 * Copyright (C) 2024 Mobileye Vision Technologies Ltd.
 */

/*
 * Set pr_fmt() for printing from eqc_early_init().
 * It is called at of_clk_init() stage (read: really early).
 */
#define pr_fmt(fmt) "clk-eyeq: " fmt

#include <linux/array_size.h>
#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <dt-bindings/clock/mobileye,eyeq5-clk.h>

/* In frac mode, it enables fractional noise canceling DAC. Else, no function. */
#define PCSR0_DAC_EN			BIT(0)
/* Fractional or integer mode */
#define PCSR0_DSM_EN			BIT(1)
#define PCSR0_PLL_EN			BIT(2)
/* All clocks output held at 0 */
#define PCSR0_FOUTPOSTDIV_EN		BIT(3)
#define PCSR0_POST_DIV1			GENMASK(6, 4)
#define PCSR0_POST_DIV2			GENMASK(9, 7)
#define PCSR0_REF_DIV			GENMASK(15, 10)
#define PCSR0_INTIN			GENMASK(27, 16)
#define PCSR0_BYPASS			BIT(28)
/* Bits 30..29 are reserved */
#define PCSR0_PLL_LOCKED		BIT(31)

#define PCSR1_RESET			BIT(0)
#define PCSR1_SSGC_DIV			GENMASK(4, 1)
/* Spread amplitude (% = 0.1 * SPREAD[4:0]) */
#define PCSR1_SPREAD			GENMASK(9, 5)
#define PCSR1_DIS_SSCG			BIT(10)
/* Down-spread or center-spread */
#define PCSR1_DOWN_SPREAD		BIT(11)
#define PCSR1_FRAC_IN			GENMASK(31, 12)

struct eqc_pll {
	unsigned int	index;
	const char	*name;
	unsigned int	reg64;
};

/*
 * Divider clock. Divider is 2*(v+1), with v the register value.
 * Min divider is 2, max is 2*(2^width).
 */
struct eqc_div {
	unsigned int	index;
	const char	*name;
	unsigned int	parent;
	unsigned int	reg;
	u8		shift;
	u8		width;
};

struct eqc_match_data {
	unsigned int		pll_count;
	const struct eqc_pll	*plls;

	unsigned int		div_count;
	const struct eqc_div	*divs;

	const char		*reset_auxdev_name;
	const char		*pinctrl_auxdev_name;

	unsigned int		early_clk_count;
};

struct eqc_early_match_data {
	unsigned int		early_pll_count;
	const struct eqc_pll	*early_plls;

	/*
	 * We want our of_xlate callback to EPROBE_DEFER instead of dev_err()
	 * and EINVAL. For that, we must know the total clock count.
	 */
	unsigned int		late_clk_count;
};

/*
 * Both factors (mult and div) must fit in 32 bits. When an operation overflows,
 * this function throws away low bits so that factors still fit in 32 bits.
 *
 * Precision loss depends on amplitude of mult and div. Worst theorical
 * loss is: (UINT_MAX+1) / UINT_MAX - 1 = 2.3e-10.
 * This is 1Hz every 4.3GHz.
 */
static void eqc_pll_downshift_factors(unsigned long *mult, unsigned long *div)
{
	unsigned long biggest;
	unsigned int shift;

	/* This function can be removed if mult/div switch to unsigned long. */
	static_assert(sizeof_field(struct clk_fixed_factor, mult) == sizeof(unsigned int));
	static_assert(sizeof_field(struct clk_fixed_factor, div) == sizeof(unsigned int));

	/* No overflow, nothing to be done. */
	if (*mult <= UINT_MAX && *div <= UINT_MAX)
		return;

	/*
	 * Compute the shift required to bring the biggest factor into unsigned
	 * int range. That is, shift its highest set bit to the unsigned int
	 * most significant bit.
	 */
	biggest = max(*mult, *div);
	shift = __fls(biggest) - (BITS_PER_BYTE * sizeof(unsigned int)) + 1;

	*mult >>= shift;
	*div >>= shift;
}

static int eqc_pll_parse_registers(u32 r0, u32 r1, unsigned long *mult,
				   unsigned long *div, unsigned long *acc)
{
	u32 spread;

	if (r0 & PCSR0_BYPASS) {
		*mult = 1;
		*div = 1;
		*acc = 0;
		return 0;
	}

	if (!(r0 & PCSR0_PLL_LOCKED))
		return -EINVAL;

	*mult = FIELD_GET(PCSR0_INTIN, r0);
	*div = FIELD_GET(PCSR0_REF_DIV, r0);
	if (r0 & PCSR0_FOUTPOSTDIV_EN)
		*div *= FIELD_GET(PCSR0_POST_DIV1, r0) * FIELD_GET(PCSR0_POST_DIV2, r0);

	/* Fractional mode, in 2^20 (0x100000) parts. */
	if (r0 & PCSR0_DSM_EN) {
		*div *= (1ULL << 20);
		*mult = *mult * (1ULL << 20) + FIELD_GET(PCSR1_FRAC_IN, r1);
	}

	if (!*mult || !*div)
		return -EINVAL;

	if (r1 & (PCSR1_RESET | PCSR1_DIS_SSCG)) {
		*acc = 0;
		return 0;
	}

	/*
	 * Spread spectrum.
	 *
	 * Spread is 1/1000 parts of frequency, accuracy is half of
	 * that. To get accuracy, convert to ppb (parts per billion).
	 *
	 * acc = spread * 1e6 / 2
	 *   with acc in parts per billion and,
	 *        spread in parts per thousand.
	 */
	spread = FIELD_GET(PCSR1_SPREAD, r1);
	*acc = spread * 500000;

	if (r1 & PCSR1_DOWN_SPREAD) {
		/*
		 * Downspreading: the central frequency is half a
		 * spread lower.
		 */
		*mult *= 2000 - spread;
		*div *= 2000;

		/*
		 * Previous operation might overflow 32 bits. If it
		 * does, throw away the least amount of low bits.
		 */
		eqc_pll_downshift_factors(mult, div);
	}

	return 0;
}

static void eqc_probe_init_plls(struct device *dev, const struct eqc_match_data *data,
				void __iomem *base, struct clk_hw_onecell_data *cells)
{
	unsigned long mult, div, acc;
	const struct eqc_pll *pll;
	struct clk_hw *hw;
	unsigned int i;
	u32 r0, r1;
	u64 val;
	int ret;

	for (i = 0; i < data->pll_count; i++) {
		pll = &data->plls[i];

		val = readq(base + pll->reg64);
		r0 = val;
		r1 = val >> 32;

		ret = eqc_pll_parse_registers(r0, r1, &mult, &div, &acc);
		if (ret) {
			dev_warn(dev, "failed parsing state of %s\n", pll->name);
			cells->hws[pll->index] = ERR_PTR(ret);
			continue;
		}

		hw = clk_hw_register_fixed_factor_with_accuracy_fwname(dev,
				dev->of_node, pll->name, "ref", 0, mult, div, acc);
		cells->hws[pll->index] = hw;
		if (IS_ERR(hw))
			dev_warn(dev, "failed registering %s: %pe\n", pll->name, hw);
	}
}

static void eqc_probe_init_divs(struct device *dev, const struct eqc_match_data *data,
				void __iomem *base, struct clk_hw_onecell_data *cells)
{
	struct clk_parent_data parent_data = { };
	const struct eqc_div *div;
	struct clk_hw *parent;
	void __iomem *reg;
	struct clk_hw *hw;
	unsigned int i;

	for (i = 0; i < data->div_count; i++) {
		div = &data->divs[i];
		reg = base + div->reg;
		parent = cells->hws[div->parent];

		if (IS_ERR(parent)) {
			/* Parent is in early clk provider. */
			parent_data.index = div->parent;
			parent_data.hw = NULL;
		} else {
			/* Avoid clock lookup when we already have the hw reference. */
			parent_data.index = 0;
			parent_data.hw = parent;
		}

		hw = clk_hw_register_divider_table_parent_data(dev, div->name,
				&parent_data, 0, reg, div->shift, div->width,
				CLK_DIVIDER_EVEN_INTEGERS, NULL, NULL);
		cells->hws[div->index] = hw;
		if (IS_ERR(hw))
			dev_warn(dev, "failed registering %s: %pe\n",
				 div->name, hw);
	}
}

static void eqc_auxdev_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);

	kfree(adev);
}

static int eqc_auxdev_create(struct device *dev, void __iomem *base,
			     const char *name, u32 id)
{
	struct auxiliary_device *adev;
	int ret;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	adev->name = name;
	adev->dev.parent = dev;
	adev->dev.platform_data = (void __force *)base;
	adev->dev.release = eqc_auxdev_release;
	adev->id = id;

	ret = auxiliary_device_init(adev);
	if (ret)
		return ret;

	ret = auxiliary_device_add(adev);
	if (ret)
		auxiliary_device_uninit(adev);

	return ret;
}

static int eqc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct eqc_match_data *data;
	struct clk_hw_onecell_data *cells;
	unsigned int i, clk_count;
	struct resource *res;
	void __iomem *base;
	int ret;

	data = device_get_match_data(dev);
	if (!data)
		return 0; /* No clocks nor auxdevs, we are done. */

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	base = ioremap(res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	/* Init optional reset auxiliary device. */
	if (data->reset_auxdev_name) {
		ret = eqc_auxdev_create(dev, base, data->reset_auxdev_name, 0);
		if (ret)
			dev_warn(dev, "failed creating auxiliary device %s.%s: %d\n",
				 KBUILD_MODNAME, data->reset_auxdev_name, ret);
	}

	/* Init optional pinctrl auxiliary device. */
	if (data->pinctrl_auxdev_name) {
		ret = eqc_auxdev_create(dev, base, data->pinctrl_auxdev_name, 0);
		if (ret)
			dev_warn(dev, "failed creating auxiliary device %s.%s: %d\n",
				 KBUILD_MODNAME, data->pinctrl_auxdev_name, ret);
	}

	if (data->pll_count + data->div_count == 0)
		return 0; /* Zero clocks, we are done. */

	clk_count = data->pll_count + data->div_count + data->early_clk_count;
	cells = kzalloc(struct_size(cells, hws, clk_count), GFP_KERNEL);
	if (!cells)
		return -ENOMEM;

	cells->num = clk_count;

	/* Early PLLs are marked as errors: the early provider will get queried. */
	for (i = 0; i < clk_count; i++)
		cells->hws[i] = ERR_PTR(-EINVAL);

	eqc_probe_init_plls(dev, data, base, cells);

	eqc_probe_init_divs(dev, data, base, cells);

	/* When providing a single clock, require no cell. */
	if (clk_count == 1)
		return of_clk_add_hw_provider(np, of_clk_hw_simple_get, cells->hws[0]);
	else
		return of_clk_add_hw_provider(np, of_clk_hw_onecell_get, cells);
}

/* Required early for GIC timer (pll-cpu) and UARTs (pll-per). */
static const struct eqc_pll eqc_eyeq5_early_plls[] = {
	{ .index = EQ5C_PLL_CPU, .name = "pll-cpu",  .reg64 = 0x02C },
	{ .index = EQ5C_PLL_PER, .name = "pll-per",  .reg64 = 0x05C },
};

static const struct eqc_pll eqc_eyeq5_plls[] = {
	{ .index = EQ5C_PLL_VMP,  .name = "pll-vmp",  .reg64 = 0x034 },
	{ .index = EQ5C_PLL_PMA,  .name = "pll-pma",  .reg64 = 0x03C },
	{ .index = EQ5C_PLL_VDI,  .name = "pll-vdi",  .reg64 = 0x044 },
	{ .index = EQ5C_PLL_DDR0, .name = "pll-ddr0", .reg64 = 0x04C },
	{ .index = EQ5C_PLL_PCI,  .name = "pll-pci",  .reg64 = 0x054 },
	{ .index = EQ5C_PLL_PMAC, .name = "pll-pmac", .reg64 = 0x064 },
	{ .index = EQ5C_PLL_MPC,  .name = "pll-mpc",  .reg64 = 0x06C },
	{ .index = EQ5C_PLL_DDR1, .name = "pll-ddr1", .reg64 = 0x074 },
};

static const struct eqc_div eqc_eyeq5_divs[] = {
	{
		.index = EQ5C_DIV_OSPI,
		.name = "div-ospi",
		.parent = EQ5C_PLL_PER,
		.reg = 0x11C,
		.shift = 0,
		.width = 4,
	},
};

static const struct eqc_early_match_data eqc_eyeq5_early_match_data __initconst = {
	.early_pll_count	= ARRAY_SIZE(eqc_eyeq5_early_plls),
	.early_plls		= eqc_eyeq5_early_plls,

	.late_clk_count		= ARRAY_SIZE(eqc_eyeq5_plls) + ARRAY_SIZE(eqc_eyeq5_divs),
};

static const struct eqc_match_data eqc_eyeq5_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq5_plls),
	.plls		= eqc_eyeq5_plls,

	.div_count	= ARRAY_SIZE(eqc_eyeq5_divs),
	.divs		= eqc_eyeq5_divs,

	.reset_auxdev_name = "reset",
	.pinctrl_auxdev_name = "pinctrl",

	.early_clk_count = ARRAY_SIZE(eqc_eyeq5_early_plls),
};

static const struct eqc_pll eqc_eyeq6l_plls[] = {
	{ .index = EQ6LC_PLL_DDR, .name = "pll-ddr", .reg64 = 0x02C },
	{ .index = EQ6LC_PLL_CPU, .name = "pll-cpu", .reg64 = 0x034 }, /* also acc */
	{ .index = EQ6LC_PLL_PER, .name = "pll-per", .reg64 = 0x03C },
	{ .index = EQ6LC_PLL_VDI, .name = "pll-vdi", .reg64 = 0x044 },
};

static const struct eqc_match_data eqc_eyeq6l_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6l_plls),
	.plls		= eqc_eyeq6l_plls,

	.reset_auxdev_name = "reset",
};

static const struct eqc_match_data eqc_eyeq6h_west_match_data = {
	.reset_auxdev_name = "reset_west",
};

static const struct eqc_pll eqc_eyeq6h_east_plls[] = {
	{ .index = 0, .name = "pll-east", .reg64 = 0x074 },
};

static const struct eqc_match_data eqc_eyeq6h_east_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_east_plls),
	.plls		= eqc_eyeq6h_east_plls,

	.reset_auxdev_name = "reset_east",
};

static const struct eqc_pll eqc_eyeq6h_south_plls[] = {
	{ .index = EQ6HC_SOUTH_PLL_VDI,  .name = "pll-vdi",  .reg64 = 0x000 },
	{ .index = EQ6HC_SOUTH_PLL_PCIE, .name = "pll-pcie", .reg64 = 0x008 },
	{ .index = EQ6HC_SOUTH_PLL_PER,  .name = "pll-per",  .reg64 = 0x010 },
	{ .index = EQ6HC_SOUTH_PLL_ISP,  .name = "pll-isp",  .reg64 = 0x018 },
};

static const struct eqc_div eqc_eyeq6h_south_divs[] = {
	{
		.index = EQ6HC_SOUTH_DIV_EMMC,
		.name = "div-emmc",
		.parent = EQ6HC_SOUTH_PLL_PER,
		.reg = 0x070,
		.shift = 4,
		.width = 4,
	},
	{
		.index = EQ6HC_SOUTH_DIV_OSPI_REF,
		.name = "div-ospi-ref",
		.parent = EQ6HC_SOUTH_PLL_PER,
		.reg = 0x090,
		.shift = 4,
		.width = 4,
	},
	{
		.index = EQ6HC_SOUTH_DIV_OSPI_SYS,
		.name = "div-ospi-sys",
		.parent = EQ6HC_SOUTH_PLL_PER,
		.reg = 0x090,
		.shift = 8,
		.width = 1,
	},
	{
		.index = EQ6HC_SOUTH_DIV_TSU,
		.name = "div-tsu",
		.parent = EQ6HC_SOUTH_PLL_PCIE,
		.reg = 0x098,
		.shift = 4,
		.width = 8,
	},
};

static const struct eqc_match_data eqc_eyeq6h_south_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_south_plls),
	.plls		= eqc_eyeq6h_south_plls,

	.div_count	= ARRAY_SIZE(eqc_eyeq6h_south_divs),
	.divs		= eqc_eyeq6h_south_divs,
};

static const struct eqc_pll eqc_eyeq6h_ddr0_plls[] = {
	{ .index = 0, .name = "pll-ddr0", .reg64 = 0x074 },
};

static const struct eqc_match_data eqc_eyeq6h_ddr0_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_ddr0_plls),
	.plls		= eqc_eyeq6h_ddr0_plls,
};

static const struct eqc_pll eqc_eyeq6h_ddr1_plls[] = {
	{ .index = 0, .name = "pll-ddr1", .reg64 = 0x074 },
};

static const struct eqc_match_data eqc_eyeq6h_ddr1_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_ddr1_plls),
	.plls		= eqc_eyeq6h_ddr1_plls,
};

static const struct eqc_pll eqc_eyeq6h_acc_plls[] = {
	{ .index = EQ6HC_ACC_PLL_XNN, .name = "pll-xnn", .reg64 = 0x040 },
	{ .index = EQ6HC_ACC_PLL_VMP, .name = "pll-vmp", .reg64 = 0x050 },
	{ .index = EQ6HC_ACC_PLL_PMA, .name = "pll-pma", .reg64 = 0x05C },
	{ .index = EQ6HC_ACC_PLL_MPC, .name = "pll-mpc", .reg64 = 0x068 },
	{ .index = EQ6HC_ACC_PLL_NOC, .name = "pll-noc", .reg64 = 0x070 },
};

static const struct eqc_match_data eqc_eyeq6h_acc_match_data = {
	.pll_count	= ARRAY_SIZE(eqc_eyeq6h_acc_plls),
	.plls		= eqc_eyeq6h_acc_plls,

	.reset_auxdev_name = "reset_acc",
};

static const struct of_device_id eqc_match_table[] = {
	{ .compatible = "mobileye,eyeq5-olb", .data = &eqc_eyeq5_match_data },
	{ .compatible = "mobileye,eyeq6l-olb", .data = &eqc_eyeq6l_match_data },
	{ .compatible = "mobileye,eyeq6h-west-olb", .data = &eqc_eyeq6h_west_match_data },
	{ .compatible = "mobileye,eyeq6h-east-olb", .data = &eqc_eyeq6h_east_match_data },
	{ .compatible = "mobileye,eyeq6h-south-olb", .data = &eqc_eyeq6h_south_match_data },
	{ .compatible = "mobileye,eyeq6h-ddr0-olb", .data = &eqc_eyeq6h_ddr0_match_data },
	{ .compatible = "mobileye,eyeq6h-ddr1-olb", .data = &eqc_eyeq6h_ddr1_match_data },
	{ .compatible = "mobileye,eyeq6h-acc-olb", .data = &eqc_eyeq6h_acc_match_data },
	{}
};

static struct platform_driver eqc_driver = {
	.probe = eqc_probe,
	.driver = {
		.name = "clk-eyeq",
		.of_match_table = eqc_match_table,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(eqc_driver);

/* Required early for GIC timer. */
static const struct eqc_pll eqc_eyeq6h_central_early_plls[] = {
	{ .index = 0, .name = "pll-cpu", .reg64 = 0x02C },
};

static const struct eqc_early_match_data eqc_eyeq6h_central_early_match_data __initconst = {
	.early_pll_count	= ARRAY_SIZE(eqc_eyeq6h_central_early_plls),
	.early_plls		= eqc_eyeq6h_central_early_plls,
};

/* Required early for UART. */
static const struct eqc_pll eqc_eyeq6h_west_early_plls[] = {
	{ .index = 0, .name = "pll-west", .reg64 = 0x074 },
};

static const struct eqc_early_match_data eqc_eyeq6h_west_early_match_data __initconst = {
	.early_pll_count	= ARRAY_SIZE(eqc_eyeq6h_west_early_plls),
	.early_plls		= eqc_eyeq6h_west_early_plls,
};

static void __init eqc_early_init(struct device_node *np,
				  const struct eqc_early_match_data *early_data)
{
	struct clk_hw_onecell_data *cells;
	unsigned int i, clk_count;
	void __iomem *base;
	int ret;

	clk_count = early_data->early_pll_count + early_data->late_clk_count;
	cells = kzalloc(struct_size(cells, hws, clk_count), GFP_KERNEL);
	if (!cells) {
		ret = -ENOMEM;
		goto err;
	}

	cells->num = clk_count;

	/*
	 * Mark all clocks as deferred; some are registered here, the rest at
	 * platform device probe.
	 *
	 * Once the platform device is probed, its provider will take priority
	 * when looking up clocks.
	 */
	for (i = 0; i < clk_count; i++)
		cells->hws[i] = ERR_PTR(-EPROBE_DEFER);

	/* Offsets (reg64) of early PLLs are relative to OLB block. */
	base = of_iomap(np, 0);
	if (!base) {
		ret = -ENODEV;
		goto err;
	}

	for (i = 0; i < early_data->early_pll_count; i++) {
		const struct eqc_pll *pll = &early_data->early_plls[i];
		unsigned long mult, div, acc;
		struct clk_hw *hw;
		u32 r0, r1;
		u64 val;

		val = readq(base + pll->reg64);
		r0 = val;
		r1 = val >> 32;

		ret = eqc_pll_parse_registers(r0, r1, &mult, &div, &acc);
		if (ret) {
			pr_err("failed parsing state of %s\n", pll->name);
			goto err;
		}

		hw = clk_hw_register_fixed_factor_with_accuracy_fwname(NULL,
				np, pll->name, "ref", 0, mult, div, acc);
		cells->hws[pll->index] = hw;
		if (IS_ERR(hw)) {
			pr_err("failed registering %s: %pe\n", pll->name, hw);
			ret = PTR_ERR(hw);
			goto err;
		}
	}

	/* When providing a single clock, require no cell. */
	if (clk_count == 1)
		ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, cells->hws[0]);
	else
		ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, cells);
	if (ret) {
		pr_err("failed registering clk provider: %d\n", ret);
		goto err;
	}

	return;

err:
	/*
	 * We are doomed. The system will not be able to boot.
	 *
	 * Let's still try to be good citizens by freeing resources and print
	 * a last error message that might help debugging.
	 */

	pr_err("failed clk init: %d\n", ret);

	if (cells) {
		of_clk_del_provider(np);

		for (i = 0; i < early_data->early_pll_count; i++) {
			const struct eqc_pll *pll = &early_data->early_plls[i];
			struct clk_hw *hw = cells->hws[pll->index];

			if (!IS_ERR_OR_NULL(hw))
				clk_hw_unregister_fixed_factor(hw);
		}

		kfree(cells);
	}
}

static void __init eqc_eyeq5_early_init(struct device_node *np)
{
	eqc_early_init(np, &eqc_eyeq5_early_match_data);
}
CLK_OF_DECLARE_DRIVER(eqc_eyeq5, "mobileye,eyeq5-olb", eqc_eyeq5_early_init);

static void __init eqc_eyeq6h_central_early_init(struct device_node *np)
{
	eqc_early_init(np, &eqc_eyeq6h_central_early_match_data);
}
CLK_OF_DECLARE_DRIVER(eqc_eyeq6h_central, "mobileye,eyeq6h-central-olb",
		      eqc_eyeq6h_central_early_init);

static void __init eqc_eyeq6h_west_early_init(struct device_node *np)
{
	eqc_early_init(np, &eqc_eyeq6h_west_early_match_data);
}
CLK_OF_DECLARE_DRIVER(eqc_eyeq6h_west, "mobileye,eyeq6h-west-olb",
		      eqc_eyeq6h_west_early_init);
