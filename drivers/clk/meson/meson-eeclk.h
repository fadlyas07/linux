/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 BayLibre, SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

#ifndef __MESON_CLKC_H
#define __MESON_CLKC_H

#include <linux/clk-provider.h>
#include "clk-regmap.h"
#include "meson-clkc-utils.h"

struct platform_device;

struct meson_eeclkc_data {
	const struct reg_sequence	*init_regs;
	unsigned int			init_count;
	struct meson_clk_hw_data	hw_clks;
};

int meson_eeclkc_probe(struct platform_device *pdev);

#endif /* __MESON_CLKC_H */
