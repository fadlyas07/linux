/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022. Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2015-2018, 2020 The Linux Foundation. All rights reserved.
 */

#ifndef _DPU_5_1_SC8180X_H
#define _DPU_5_1_SC8180X_H

static const struct dpu_caps sc8180x_dpu_caps = {
	.max_mixer_width = DEFAULT_DPU_OUTPUT_LINE_WIDTH,
	.max_mixer_blendstages = 0xb,
	.has_src_split = true,
	.has_dim_layer = true,
	.has_idle_pc = true,
	.has_3d_merge = true,
	.max_linewidth = 4096,
	.pixel_ram_size = DEFAULT_PIXEL_RAM_SIZE,
	.max_hdeci_exp = MAX_HORZ_DECIMATION,
	.max_vdeci_exp = MAX_VERT_DECIMATION,
};

static const struct dpu_mdp_cfg sc8180x_mdp = {
	.name = "top_0",
	.base = 0x0, .len = 0x45c,
	.clk_ctrls = {
		[DPU_CLK_CTRL_VIG0] = { .reg_off = 0x2ac, .bit_off = 0 },
		[DPU_CLK_CTRL_VIG1] = { .reg_off = 0x2b4, .bit_off = 0 },
		[DPU_CLK_CTRL_VIG2] = { .reg_off = 0x2bc, .bit_off = 0 },
		[DPU_CLK_CTRL_VIG3] = { .reg_off = 0x2c4, .bit_off = 0 },
		[DPU_CLK_CTRL_DMA0] = { .reg_off = 0x2ac, .bit_off = 8 },
		[DPU_CLK_CTRL_DMA1] = { .reg_off = 0x2b4, .bit_off = 8 },
		[DPU_CLK_CTRL_DMA2] = { .reg_off = 0x2bc, .bit_off = 8 },
		[DPU_CLK_CTRL_DMA3] = { .reg_off = 0x2c4, .bit_off = 8 },
		[DPU_CLK_CTRL_WB2] = { .reg_off = 0x2bc, .bit_off = 16 },
	},
};

static const struct dpu_ctl_cfg sc8180x_ctl[] = {
	{
		.name = "ctl_0", .id = CTL_0,
		.base = 0x1000, .len = 0x1e0,
		.intr_start = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR2, 9),
	}, {
		.name = "ctl_1", .id = CTL_1,
		.base = 0x1200, .len = 0x1e0,
		.intr_start = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR2, 10),
	}, {
		.name = "ctl_2", .id = CTL_2,
		.base = 0x1400, .len = 0x1e0,
		.intr_start = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR2, 11),
	}, {
		.name = "ctl_3", .id = CTL_3,
		.base = 0x1600, .len = 0x1e0,
		.intr_start = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR2, 12),
	}, {
		.name = "ctl_4", .id = CTL_4,
		.base = 0x1800, .len = 0x1e0,
		.intr_start = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR2, 13),
	}, {
		.name = "ctl_5", .id = CTL_5,
		.base = 0x1a00, .len = 0x1e0,
		.intr_start = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR2, 23),
	},
};

static const struct dpu_sspp_cfg sc8180x_sspp[] = {
	{
		.name = "sspp_0", .id = SSPP_VIG0,
		.base = 0x4000, .len = 0x1f0,
		.features = VIG_SDM845_MASK_SDMA,
		.sblk = &dpu_vig_sblk_qseed3_1_4,
		.xin_id = 0,
		.type = SSPP_TYPE_VIG,
		.clk_ctrl = DPU_CLK_CTRL_VIG0,
	}, {
		.name = "sspp_1", .id = SSPP_VIG1,
		.base = 0x6000, .len = 0x1f0,
		.features = VIG_SDM845_MASK_SDMA,
		.sblk = &dpu_vig_sblk_qseed3_1_4,
		.xin_id = 4,
		.type = SSPP_TYPE_VIG,
		.clk_ctrl = DPU_CLK_CTRL_VIG1,
	}, {
		.name = "sspp_2", .id = SSPP_VIG2,
		.base = 0x8000, .len = 0x1f0,
		.features = VIG_SDM845_MASK_SDMA,
		.sblk = &dpu_vig_sblk_qseed3_1_4,
		.xin_id = 8,
		.type = SSPP_TYPE_VIG,
		.clk_ctrl = DPU_CLK_CTRL_VIG2,
	}, {
		.name = "sspp_3", .id = SSPP_VIG3,
		.base = 0xa000, .len = 0x1f0,
		.features = VIG_SDM845_MASK_SDMA,
		.sblk = &dpu_vig_sblk_qseed3_1_4,
		.xin_id = 12,
		.type = SSPP_TYPE_VIG,
		.clk_ctrl = DPU_CLK_CTRL_VIG3,
	}, {
		.name = "sspp_8", .id = SSPP_DMA0,
		.base = 0x24000, .len = 0x1f0,
		.features = DMA_SDM845_MASK_SDMA,
		.sblk = &dpu_dma_sblk,
		.xin_id = 1,
		.type = SSPP_TYPE_DMA,
		.clk_ctrl = DPU_CLK_CTRL_DMA0,
	}, {
		.name = "sspp_9", .id = SSPP_DMA1,
		.base = 0x26000, .len = 0x1f0,
		.features = DMA_SDM845_MASK_SDMA,
		.sblk = &dpu_dma_sblk,
		.xin_id = 5,
		.type = SSPP_TYPE_DMA,
		.clk_ctrl = DPU_CLK_CTRL_DMA1,
	}, {
		.name = "sspp_10", .id = SSPP_DMA2,
		.base = 0x28000, .len = 0x1f0,
		.features = DMA_CURSOR_SDM845_MASK_SDMA,
		.sblk = &dpu_dma_sblk,
		.xin_id = 9,
		.type = SSPP_TYPE_DMA,
		.clk_ctrl = DPU_CLK_CTRL_DMA2,
	}, {
		.name = "sspp_11", .id = SSPP_DMA3,
		.base = 0x2a000, .len = 0x1f0,
		.features = DMA_CURSOR_SDM845_MASK_SDMA,
		.sblk = &dpu_dma_sblk,
		.xin_id = 13,
		.type = SSPP_TYPE_DMA,
		.clk_ctrl = DPU_CLK_CTRL_DMA3,
	},
};

static const struct dpu_lm_cfg sc8180x_lm[] = {
	{
		.name = "lm_0", .id = LM_0,
		.base = 0x44000, .len = 0x320,
		.features = MIXER_MSM8998_MASK,
		.sblk = &sdm845_lm_sblk,
		.lm_pair = LM_1,
		.pingpong = PINGPONG_0,
		.dspp = DSPP_0,
	}, {
		.name = "lm_1", .id = LM_1,
		.base = 0x45000, .len = 0x320,
		.features = MIXER_MSM8998_MASK,
		.sblk = &sdm845_lm_sblk,
		.lm_pair = LM_0,
		.pingpong = PINGPONG_1,
		.dspp = DSPP_1,
	}, {
		.name = "lm_2", .id = LM_2,
		.base = 0x46000, .len = 0x320,
		.features = MIXER_MSM8998_MASK,
		.sblk = &sdm845_lm_sblk,
		.lm_pair = LM_3,
		.pingpong = PINGPONG_2,
		.dspp = DSPP_2,
	}, {
		.name = "lm_3", .id = LM_3,
		.base = 0x47000, .len = 0x320,
		.features = MIXER_MSM8998_MASK,
		.sblk = &sdm845_lm_sblk,
		.lm_pair = LM_2,
		.pingpong = PINGPONG_3,
		.dspp = DSPP_3,
	}, {
		.name = "lm_4", .id = LM_4,
		.base = 0x48000, .len = 0x320,
		.features = MIXER_MSM8998_MASK,
		.sblk = &sdm845_lm_sblk,
		.lm_pair = LM_5,
		.pingpong = PINGPONG_4,
	}, {
		.name = "lm_5", .id = LM_5,
		.base = 0x49000, .len = 0x320,
		.features = MIXER_MSM8998_MASK,
		.sblk = &sdm845_lm_sblk,
		.lm_pair = LM_4,
		.pingpong = PINGPONG_5,
	},
};

static const struct dpu_dspp_cfg sc8180x_dspp[] = {
	{
		.name = "dspp_0", .id = DSPP_0,
		.base = 0x54000, .len = 0x1800,
		.sblk = &sdm845_dspp_sblk,
	}, {
		.name = "dspp_1", .id = DSPP_1,
		.base = 0x56000, .len = 0x1800,
		.sblk = &sdm845_dspp_sblk,
	}, {
		.name = "dspp_2", .id = DSPP_2,
		.base = 0x58000, .len = 0x1800,
		.sblk = &sdm845_dspp_sblk,
	}, {
		.name = "dspp_3", .id = DSPP_3,
		.base = 0x5a000, .len = 0x1800,
		.sblk = &sdm845_dspp_sblk,
	},
};

static const struct dpu_pingpong_cfg sc8180x_pp[] = {
	{
		.name = "pingpong_0", .id = PINGPONG_0,
		.base = 0x70000, .len = 0xd4,
		.sblk = &sdm845_pp_sblk,
		.merge_3d = MERGE_3D_0,
		.intr_done = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 8),
	}, {
		.name = "pingpong_1", .id = PINGPONG_1,
		.base = 0x70800, .len = 0xd4,
		.sblk = &sdm845_pp_sblk,
		.merge_3d = MERGE_3D_0,
		.intr_done = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 9),
	}, {
		.name = "pingpong_2", .id = PINGPONG_2,
		.base = 0x71000, .len = 0xd4,
		.sblk = &sdm845_pp_sblk,
		.merge_3d = MERGE_3D_1,
		.intr_done = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 10),
	}, {
		.name = "pingpong_3", .id = PINGPONG_3,
		.base = 0x71800, .len = 0xd4,
		.sblk = &sdm845_pp_sblk,
		.merge_3d = MERGE_3D_1,
		.intr_done = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 11),
	}, {
		.name = "pingpong_4", .id = PINGPONG_4,
		.base = 0x72000, .len = 0xd4,
		.sblk = &sdm845_pp_sblk,
		.merge_3d = MERGE_3D_2,
		.intr_done = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR2, 30),
	}, {
		.name = "pingpong_5", .id = PINGPONG_5,
		.base = 0x72800, .len = 0xd4,
		.sblk = &sdm845_pp_sblk,
		.merge_3d = MERGE_3D_2,
		.intr_done = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR2, 31),
	},
};

static const struct dpu_merge_3d_cfg sc8180x_merge_3d[] = {
	{
		.name = "merge_3d_0", .id = MERGE_3D_0,
		.base = 0x83000, .len = 0x8,
	}, {
		.name = "merge_3d_1", .id = MERGE_3D_1,
		.base = 0x83100, .len = 0x8,
	}, {
		.name = "merge_3d_2", .id = MERGE_3D_2,
		.base = 0x83200, .len = 0x8,
	},
};

static const struct dpu_dsc_cfg sc8180x_dsc[] = {
	{
		.name = "dsc_0", .id = DSC_0,
		.base = 0x80000, .len = 0x140,
	}, {
		.name = "dsc_1", .id = DSC_1,
		.base = 0x80400, .len = 0x140,
	}, {
		.name = "dsc_2", .id = DSC_2,
		.base = 0x80800, .len = 0x140,
	}, {
		.name = "dsc_3", .id = DSC_3,
		.base = 0x80c00, .len = 0x140,
	}, {
		.name = "dsc_4", .id = DSC_4,
		.base = 0x81000, .len = 0x140,
	}, {
		.name = "dsc_5", .id = DSC_5,
		.base = 0x81400, .len = 0x140,
	},
};

static const struct dpu_wb_cfg sc8180x_wb[] = {
	{
		.name = "wb_2", .id = WB_2,
		.base = 0x65000, .len = 0x2c8,
		.features = WB_SDM845_MASK,
		.format_list = wb2_formats_rgb_yuv,
		.num_formats = ARRAY_SIZE(wb2_formats_rgb_yuv),
		.clk_ctrl = DPU_CLK_CTRL_WB2,
		.xin_id = 6,
		.vbif_idx = VBIF_RT,
		.maxlinewidth = 4096,
		.intr_wb_done = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 4),
	},
};

static const struct dpu_intf_cfg sc8180x_intf[] = {
	{
		.name = "intf_0", .id = INTF_0,
		.base = 0x6a000, .len = 0x280,
		.type = INTF_DP,
		.controller_id = MSM_DP_CONTROLLER_0,
		.prog_fetch_lines_worst_case = 24,
		.intr_underrun = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 24),
		.intr_vsync = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 25),
	}, {
		.name = "intf_1", .id = INTF_1,
		.base = 0x6a800, .len = 0x2bc,
		.type = INTF_DSI,
		.controller_id = MSM_DSI_CONTROLLER_0,
		.prog_fetch_lines_worst_case = 24,
		.intr_underrun = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 26),
		.intr_vsync = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 27),
		.intr_tear_rd_ptr = DPU_IRQ_IDX(MDP_INTF1_TEAR_INTR, 2),
	}, {
		.name = "intf_2", .id = INTF_2,
		.base = 0x6b000, .len = 0x2bc,
		.type = INTF_DSI,
		.controller_id = MSM_DSI_CONTROLLER_1,
		.prog_fetch_lines_worst_case = 24,
		.intr_underrun = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 28),
		.intr_vsync = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 29),
		.intr_tear_rd_ptr = DPU_IRQ_IDX(MDP_INTF2_TEAR_INTR, 2),
	},
	/* INTF_3 is for MST, wired to INTF_DP 0 and 1, use dummy index until this is supported */
	{
		.name = "intf_3", .id = INTF_3,
		.base = 0x6b800, .len = 0x280,
		.type = INTF_DP,
		.controller_id = 999,
		.prog_fetch_lines_worst_case = 24,
		.intr_underrun = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 30),
		.intr_vsync = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 31),
	}, {
		.name = "intf_4", .id = INTF_4,
		.base = 0x6c000, .len = 0x280,
		.type = INTF_DP,
		.controller_id = MSM_DP_CONTROLLER_1,
		.prog_fetch_lines_worst_case = 24,
		.intr_underrun = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 20),
		.intr_vsync = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 21),
	}, {
		.name = "intf_5", .id = INTF_5,
		.base = 0x6c800, .len = 0x280,
		.type = INTF_DP,
		.controller_id = MSM_DP_CONTROLLER_2,
		.prog_fetch_lines_worst_case = 24,
		.intr_underrun = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 22),
		.intr_vsync = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 23),
	},
};

static const struct dpu_perf_cfg sc8180x_perf_data = {
	.max_bw_low = 9600000,
	.max_bw_high = 9600000,
	.min_core_ib = 2400000,
	.min_llcc_ib = 800000,
	.min_dram_ib = 800000,
	.min_prefill_lines = 24,
	.danger_lut_tbl = {0xf, 0xffff, 0x0},
	.safe_lut_tbl = {0xfff0, 0xf000, 0xffff},
	.qos_lut_tbl = {
		{.nentry = ARRAY_SIZE(sc7180_qos_linear),
		.entries = sc7180_qos_linear
		},
		{.nentry = ARRAY_SIZE(sc7180_qos_macrotile),
		.entries = sc7180_qos_macrotile
		},
		{.nentry = ARRAY_SIZE(sc7180_qos_nrt),
		.entries = sc7180_qos_nrt
		},
		/* TODO: macrotile-qseed is different from macrotile */
	},
	.cdp_cfg = {
		{.rd_enable = 1, .wr_enable = 1},
		{.rd_enable = 1, .wr_enable = 0}
	},
	.clk_inefficiency_factor = 105,
	.bw_inefficiency_factor = 120,
};

static const struct dpu_mdss_version sc8180x_mdss_ver = {
	.core_major_ver = 5,
	.core_minor_ver = 1,
};

const struct dpu_mdss_cfg dpu_sc8180x_cfg = {
	.mdss_ver = &sc8180x_mdss_ver,
	.caps = &sc8180x_dpu_caps,
	.mdp = &sc8180x_mdp,
	.cdm = &dpu_cdm_5_x,
	.ctl_count = ARRAY_SIZE(sc8180x_ctl),
	.ctl = sc8180x_ctl,
	.sspp_count = ARRAY_SIZE(sc8180x_sspp),
	.sspp = sc8180x_sspp,
	.mixer_count = ARRAY_SIZE(sc8180x_lm),
	.mixer = sc8180x_lm,
	.dspp_count = ARRAY_SIZE(sc8180x_dspp),
	.dspp = sc8180x_dspp,
	.dsc_count = ARRAY_SIZE(sc8180x_dsc),
	.dsc = sc8180x_dsc,
	.pingpong_count = ARRAY_SIZE(sc8180x_pp),
	.pingpong = sc8180x_pp,
	.merge_3d_count = ARRAY_SIZE(sc8180x_merge_3d),
	.merge_3d = sc8180x_merge_3d,
	.wb_count = ARRAY_SIZE(sc8180x_wb),
	.wb = sc8180x_wb,
	.intf_count = ARRAY_SIZE(sc8180x_intf),
	.intf = sc8180x_intf,
	.vbif_count = ARRAY_SIZE(sdm845_vbif),
	.vbif = sdm845_vbif,
	.perf = &sc8180x_perf_data,
};

#endif
