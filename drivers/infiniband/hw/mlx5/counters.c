// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2013-2020, Mellanox Technologies inc. All rights reserved.
 */

#include "mlx5_ib.h"
#include <linux/mlx5/eswitch.h>
#include <linux/mlx5/vport.h>
#include "counters.h"
#include "ib_rep.h"
#include "qp.h"

struct mlx5_ib_counter {
	const char *name;
	size_t offset;
	u32 type;
};

struct mlx5_rdma_counter {
	struct rdma_counter rdma_counter;

	struct mlx5_fc *fc[MLX5_IB_OPCOUNTER_MAX];
	struct xarray qpn_opfc_xa;
};

static struct mlx5_rdma_counter *to_mcounter(struct rdma_counter *counter)
{
	return container_of(counter, struct mlx5_rdma_counter, rdma_counter);
}

#define INIT_Q_COUNTER(_name)		\
	{ .name = #_name, .offset = MLX5_BYTE_OFF(query_q_counter_out, _name)}

#define INIT_VPORT_Q_COUNTER(_name)		\
	{ .name = "vport_" #_name, .offset =	\
		MLX5_BYTE_OFF(query_q_counter_out, _name)}

static const struct mlx5_ib_counter basic_q_cnts[] = {
	INIT_Q_COUNTER(rx_write_requests),
	INIT_Q_COUNTER(rx_read_requests),
	INIT_Q_COUNTER(rx_atomic_requests),
	INIT_Q_COUNTER(rx_dct_connect),
	INIT_Q_COUNTER(out_of_buffer),
};

static const struct mlx5_ib_counter out_of_seq_q_cnts[] = {
	INIT_Q_COUNTER(out_of_sequence),
};

static const struct mlx5_ib_counter retrans_q_cnts[] = {
	INIT_Q_COUNTER(duplicate_request),
	INIT_Q_COUNTER(rnr_nak_retry_err),
	INIT_Q_COUNTER(packet_seq_err),
	INIT_Q_COUNTER(implied_nak_seq_err),
	INIT_Q_COUNTER(local_ack_timeout_err),
};

static const struct mlx5_ib_counter vport_basic_q_cnts[] = {
	INIT_VPORT_Q_COUNTER(rx_write_requests),
	INIT_VPORT_Q_COUNTER(rx_read_requests),
	INIT_VPORT_Q_COUNTER(rx_atomic_requests),
	INIT_VPORT_Q_COUNTER(rx_dct_connect),
	INIT_VPORT_Q_COUNTER(out_of_buffer),
};

static const struct mlx5_ib_counter vport_out_of_seq_q_cnts[] = {
	INIT_VPORT_Q_COUNTER(out_of_sequence),
};

static const struct mlx5_ib_counter vport_retrans_q_cnts[] = {
	INIT_VPORT_Q_COUNTER(duplicate_request),
	INIT_VPORT_Q_COUNTER(rnr_nak_retry_err),
	INIT_VPORT_Q_COUNTER(packet_seq_err),
	INIT_VPORT_Q_COUNTER(implied_nak_seq_err),
	INIT_VPORT_Q_COUNTER(local_ack_timeout_err),
};

#define INIT_CONG_COUNTER(_name)		\
	{ .name = #_name, .offset =	\
		MLX5_BYTE_OFF(query_cong_statistics_out, _name ## _high)}

static const struct mlx5_ib_counter cong_cnts[] = {
	INIT_CONG_COUNTER(rp_cnp_ignored),
	INIT_CONG_COUNTER(rp_cnp_handled),
	INIT_CONG_COUNTER(np_ecn_marked_roce_packets),
	INIT_CONG_COUNTER(np_cnp_sent),
};

static const struct mlx5_ib_counter extended_err_cnts[] = {
	INIT_Q_COUNTER(resp_local_length_error),
	INIT_Q_COUNTER(resp_cqe_error),
	INIT_Q_COUNTER(req_cqe_error),
	INIT_Q_COUNTER(req_remote_invalid_request),
	INIT_Q_COUNTER(req_remote_access_errors),
	INIT_Q_COUNTER(resp_remote_access_errors),
	INIT_Q_COUNTER(resp_cqe_flush_error),
	INIT_Q_COUNTER(req_cqe_flush_error),
	INIT_Q_COUNTER(req_transport_retries_exceeded),
	INIT_Q_COUNTER(req_rnr_retries_exceeded),
};

static const struct mlx5_ib_counter roce_accl_cnts[] = {
	INIT_Q_COUNTER(roce_adp_retrans),
	INIT_Q_COUNTER(roce_adp_retrans_to),
	INIT_Q_COUNTER(roce_slow_restart),
	INIT_Q_COUNTER(roce_slow_restart_cnps),
	INIT_Q_COUNTER(roce_slow_restart_trans),
};

static const struct mlx5_ib_counter vport_extended_err_cnts[] = {
	INIT_VPORT_Q_COUNTER(resp_local_length_error),
	INIT_VPORT_Q_COUNTER(resp_cqe_error),
	INIT_VPORT_Q_COUNTER(req_cqe_error),
	INIT_VPORT_Q_COUNTER(req_remote_invalid_request),
	INIT_VPORT_Q_COUNTER(req_remote_access_errors),
	INIT_VPORT_Q_COUNTER(resp_remote_access_errors),
	INIT_VPORT_Q_COUNTER(resp_cqe_flush_error),
	INIT_VPORT_Q_COUNTER(req_cqe_flush_error),
	INIT_VPORT_Q_COUNTER(req_transport_retries_exceeded),
	INIT_VPORT_Q_COUNTER(req_rnr_retries_exceeded),
};

static const struct mlx5_ib_counter vport_roce_accl_cnts[] = {
	INIT_VPORT_Q_COUNTER(roce_adp_retrans),
	INIT_VPORT_Q_COUNTER(roce_adp_retrans_to),
	INIT_VPORT_Q_COUNTER(roce_slow_restart),
	INIT_VPORT_Q_COUNTER(roce_slow_restart_cnps),
	INIT_VPORT_Q_COUNTER(roce_slow_restart_trans),
};

#define INIT_EXT_PPCNT_COUNTER(_name)		\
	{ .name = #_name, .offset =	\
	MLX5_BYTE_OFF(ppcnt_reg, \
		      counter_set.eth_extended_cntrs_grp_data_layout._name##_high)}

static const struct mlx5_ib_counter ext_ppcnt_cnts[] = {
	INIT_EXT_PPCNT_COUNTER(rx_icrc_encapsulated),
};

#define INIT_OP_COUNTER(_name, _type)		\
	{ .name = #_name, .type = MLX5_IB_OPCOUNTER_##_type}

static const struct mlx5_ib_counter basic_op_cnts[] = {
	INIT_OP_COUNTER(cc_rx_ce_pkts, CC_RX_CE_PKTS),
};

static const struct mlx5_ib_counter rdmarx_cnp_op_cnts[] = {
	INIT_OP_COUNTER(cc_rx_cnp_pkts, CC_RX_CNP_PKTS),
};

static const struct mlx5_ib_counter rdmatx_cnp_op_cnts[] = {
	INIT_OP_COUNTER(cc_tx_cnp_pkts, CC_TX_CNP_PKTS),
};

static const struct mlx5_ib_counter packets_op_cnts[] = {
	INIT_OP_COUNTER(rdma_tx_packets, RDMA_TX_PACKETS),
	INIT_OP_COUNTER(rdma_tx_bytes, RDMA_TX_BYTES),
	INIT_OP_COUNTER(rdma_rx_packets, RDMA_RX_PACKETS),
	INIT_OP_COUNTER(rdma_rx_bytes, RDMA_RX_BYTES),
};

static int mlx5_ib_read_counters(struct ib_counters *counters,
				 struct ib_counters_read_attr *read_attr,
				 struct uverbs_attr_bundle *attrs)
{
	struct mlx5_ib_mcounters *mcounters = to_mcounters(counters);
	struct mlx5_read_counters_attr mread_attr = {};
	struct mlx5_ib_flow_counters_desc *desc;
	int ret, i;

	mutex_lock(&mcounters->mcntrs_mutex);
	if (mcounters->cntrs_max_index > read_attr->ncounters) {
		ret = -EINVAL;
		goto err_bound;
	}

	mread_attr.out = kcalloc(mcounters->counters_num, sizeof(u64),
				 GFP_KERNEL);
	if (!mread_attr.out) {
		ret = -ENOMEM;
		goto err_bound;
	}

	mread_attr.hw_cntrs_hndl = mcounters->hw_cntrs_hndl;
	mread_attr.flags = read_attr->flags;
	ret = mcounters->read_counters(counters->device, &mread_attr);
	if (ret)
		goto err_read;

	/* do the pass over the counters data array to assign according to the
	 * descriptions and indexing pairs
	 */
	desc = mcounters->counters_data;
	for (i = 0; i < mcounters->ncounters; i++)
		read_attr->counters_buff[desc[i].index] += mread_attr.out[desc[i].description];

err_read:
	kfree(mread_attr.out);
err_bound:
	mutex_unlock(&mcounters->mcntrs_mutex);
	return ret;
}

static int mlx5_ib_destroy_counters(struct ib_counters *counters)
{
	struct mlx5_ib_mcounters *mcounters = to_mcounters(counters);

	mlx5_ib_counters_clear_description(counters);
	if (mcounters->hw_cntrs_hndl)
		mlx5_fc_destroy(to_mdev(counters->device)->mdev,
				mcounters->hw_cntrs_hndl);
	return 0;
}

static int mlx5_ib_create_counters(struct ib_counters *counters,
				   struct uverbs_attr_bundle *attrs)
{
	struct mlx5_ib_mcounters *mcounters = to_mcounters(counters);

	mutex_init(&mcounters->mcntrs_mutex);
	return 0;
}

static bool vport_qcounters_supported(struct mlx5_ib_dev *dev)
{
	return MLX5_CAP_GEN(dev->mdev, q_counter_other_vport) &&
	       MLX5_CAP_GEN(dev->mdev, q_counter_aggregation);
}

static const struct mlx5_ib_counters *get_counters(struct mlx5_ib_dev *dev,
						   u32 port_num)
{
	if ((is_mdev_switchdev_mode(dev->mdev) &&
	     !vport_qcounters_supported(dev)) || !port_num)
		return &dev->port[0].cnts;

	return is_mdev_switchdev_mode(dev->mdev) ?
	       &dev->port[1].cnts : &dev->port[port_num - 1].cnts;
}

/**
 * mlx5_ib_get_counters_id - Returns counters id to use for device+port
 * @dev:	Pointer to mlx5 IB device
 * @port_num:	Zero based port number
 *
 * mlx5_ib_get_counters_id() Returns counters set id to use for given
 * device port combination in switchdev and non switchdev mode of the
 * parent device.
 */
u16 mlx5_ib_get_counters_id(struct mlx5_ib_dev *dev, u32 port_num)
{
	const struct mlx5_ib_counters *cnts = get_counters(dev, port_num + 1);

	return cnts->set_id;
}

static struct rdma_hw_stats *do_alloc_stats(const struct mlx5_ib_counters *cnts)
{
	struct rdma_hw_stats *stats;
	u32 num_hw_counters;
	int i;

	num_hw_counters = cnts->num_q_counters + cnts->num_cong_counters +
			  cnts->num_ext_ppcnt_counters;
	stats = rdma_alloc_hw_stats_struct(cnts->descs,
					   num_hw_counters +
					   cnts->num_op_counters,
					   RDMA_HW_STATS_DEFAULT_LIFESPAN);
	if (!stats)
		return NULL;

	for (i = 0; i < cnts->num_op_counters; i++)
		set_bit(num_hw_counters + i, stats->is_disabled);

	return stats;
}

static struct rdma_hw_stats *
mlx5_ib_alloc_hw_device_stats(struct ib_device *ibdev)
{
	struct mlx5_ib_dev *dev = to_mdev(ibdev);
	const struct mlx5_ib_counters *cnts = &dev->port[0].cnts;

	return do_alloc_stats(cnts);
}

static struct rdma_hw_stats *
mlx5_ib_alloc_hw_port_stats(struct ib_device *ibdev, u32 port_num)
{
	struct mlx5_ib_dev *dev = to_mdev(ibdev);
	const struct mlx5_ib_counters *cnts = get_counters(dev, port_num);

	return do_alloc_stats(cnts);
}

static int mlx5_ib_query_q_counters(struct mlx5_core_dev *mdev,
				    const struct mlx5_ib_counters *cnts,
				    struct rdma_hw_stats *stats,
				    u16 set_id)
{
	u32 out[MLX5_ST_SZ_DW(query_q_counter_out)] = {};
	u32 in[MLX5_ST_SZ_DW(query_q_counter_in)] = {};
	__be32 val;
	int ret, i;

	MLX5_SET(query_q_counter_in, in, opcode, MLX5_CMD_OP_QUERY_Q_COUNTER);
	MLX5_SET(query_q_counter_in, in, counter_set_id, set_id);
	ret = mlx5_cmd_exec_inout(mdev, query_q_counter, in, out);
	if (ret)
		return ret;

	for (i = 0; i < cnts->num_q_counters; i++) {
		val = *(__be32 *)((void *)out + cnts->offsets[i]);
		stats->value[i] = (u64)be32_to_cpu(val);
	}

	return 0;
}

static int mlx5_ib_query_ext_ppcnt_counters(struct mlx5_ib_dev *dev,
					    const struct mlx5_ib_counters *cnts,
					    struct rdma_hw_stats *stats)
{
	int offset = cnts->num_q_counters + cnts->num_cong_counters;
	u32 in[MLX5_ST_SZ_DW(ppcnt_reg)] = {};
	int sz = MLX5_ST_SZ_BYTES(ppcnt_reg);
	int ret, i;
	void *out;

	out = kvzalloc(sz, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	MLX5_SET(ppcnt_reg, in, local_port, 1);
	MLX5_SET(ppcnt_reg, in, grp, MLX5_ETHERNET_EXTENDED_COUNTERS_GROUP);
	ret = mlx5_core_access_reg(dev->mdev, in, sz, out, sz, MLX5_REG_PPCNT,
				   0, 0);
	if (ret)
		goto free;

	for (i = 0; i < cnts->num_ext_ppcnt_counters; i++)
		stats->value[i + offset] =
			be64_to_cpup((__be64 *)(out +
				    cnts->offsets[i + offset]));
free:
	kvfree(out);
	return ret;
}

static int mlx5_ib_query_q_counters_vport(struct mlx5_ib_dev *dev,
					  u32 port_num,
					  const struct mlx5_ib_counters *cnts,
					  struct rdma_hw_stats *stats)

{
	u32 out[MLX5_ST_SZ_DW(query_q_counter_out)] = {};
	u32 in[MLX5_ST_SZ_DW(query_q_counter_in)] = {};
	struct mlx5_core_dev *mdev;
	__be32 val;
	int ret, i;

	if (!dev->port[port_num].rep ||
	    dev->port[port_num].rep->vport == MLX5_VPORT_UPLINK)
		return 0;

	mdev = mlx5_eswitch_get_core_dev(dev->port[port_num].rep->esw);
	if (!mdev)
		return -EOPNOTSUPP;

	MLX5_SET(query_q_counter_in, in, opcode, MLX5_CMD_OP_QUERY_Q_COUNTER);
	MLX5_SET(query_q_counter_in, in, other_vport, 1);
	MLX5_SET(query_q_counter_in, in, vport_number,
		 dev->port[port_num].rep->vport);
	MLX5_SET(query_q_counter_in, in, aggregate, 1);
	ret = mlx5_cmd_exec_inout(mdev, query_q_counter, in, out);
	if (ret)
		return ret;

	for (i = 0; i < cnts->num_q_counters; i++) {
		val = *(__be32 *)((void *)out + cnts->offsets[i]);
		stats->value[i] = (u64)be32_to_cpu(val);
	}

	return 0;
}

static int do_get_hw_stats(struct ib_device *ibdev,
			   struct rdma_hw_stats *stats,
			   u32 port_num, int index)
{
	struct mlx5_ib_dev *dev = to_mdev(ibdev);
	const struct mlx5_ib_counters *cnts = get_counters(dev, port_num);
	struct mlx5_core_dev *mdev;
	int ret, num_counters;

	if (!stats)
		return -EINVAL;

	num_counters = cnts->num_q_counters +
		       cnts->num_cong_counters +
		       cnts->num_ext_ppcnt_counters;

	if (is_mdev_switchdev_mode(dev->mdev) && dev->is_rep && port_num != 0)
		ret = mlx5_ib_query_q_counters_vport(dev, port_num - 1, cnts,
						     stats);
	else
		ret = mlx5_ib_query_q_counters(dev->mdev, cnts, stats,
					       cnts->set_id);
	if (ret)
		return ret;

	/* We don't expose device counters over Vports */
	if (is_mdev_switchdev_mode(dev->mdev) && dev->is_rep && port_num != 0)
		goto done;

	if (MLX5_CAP_PCAM_FEATURE(dev->mdev, rx_icrc_encapsulated_counter)) {
		ret =  mlx5_ib_query_ext_ppcnt_counters(dev, cnts, stats);
		if (ret)
			return ret;
	}

	if (MLX5_CAP_GEN(dev->mdev, cc_query_allowed)) {
		if (!port_num)
			port_num = 1;
		mdev = mlx5_ib_get_native_port_mdev(dev, port_num, NULL);
		if (!mdev) {
			/* If port is not affiliated yet, its in down state
			 * which doesn't have any counters yet, so it would be
			 * zero. So no need to read from the HCA.
			 */
			goto done;
		}
		ret = mlx5_lag_query_cong_counters(mdev,
						   stats->value +
						   cnts->num_q_counters,
						   cnts->num_cong_counters,
						   cnts->offsets +
						   cnts->num_q_counters);

		mlx5_ib_put_native_port_mdev(dev, port_num);
		if (ret)
			return ret;
	}

done:
	return num_counters;
}

static bool is_rdma_bytes_counter(u32 type)
{
	if (type == MLX5_IB_OPCOUNTER_RDMA_TX_BYTES ||
	    type == MLX5_IB_OPCOUNTER_RDMA_RX_BYTES ||
	    type == MLX5_IB_OPCOUNTER_RDMA_TX_BYTES_PER_QP ||
	    type == MLX5_IB_OPCOUNTER_RDMA_RX_BYTES_PER_QP)
		return true;

	return false;
}

static int do_per_qp_get_op_stat(struct rdma_counter *counter)
{
	struct mlx5_ib_dev *dev = to_mdev(counter->device);
	const struct mlx5_ib_counters *cnts = get_counters(dev, counter->port);
	struct mlx5_rdma_counter *mcounter = to_mcounter(counter);
	int i, ret, index, num_hw_counters;
	u64 packets = 0, bytes = 0;

	for (i = MLX5_IB_OPCOUNTER_CC_RX_CE_PKTS_PER_QP;
	     i <= MLX5_IB_OPCOUNTER_RDMA_RX_BYTES_PER_QP; i++) {
		if (!mcounter->fc[i])
			continue;

		ret = mlx5_fc_query(dev->mdev, mcounter->fc[i],
				    &packets, &bytes);
		if (ret)
			return ret;

		num_hw_counters = cnts->num_q_counters +
				  cnts->num_cong_counters +
				  cnts->num_ext_ppcnt_counters;

		index = i - MLX5_IB_OPCOUNTER_CC_RX_CE_PKTS_PER_QP +
			num_hw_counters;

		if (is_rdma_bytes_counter(i))
			counter->stats->value[index] = bytes;
		else
			counter->stats->value[index] = packets;

		clear_bit(index, counter->stats->is_disabled);
	}
	return 0;
}

static int do_get_op_stat(struct ib_device *ibdev,
			  struct rdma_hw_stats *stats,
			  u32 port_num, int index)
{
	struct mlx5_ib_dev *dev = to_mdev(ibdev);
	const struct mlx5_ib_counters *cnts;
	const struct mlx5_ib_op_fc *opfcs;
	u64 packets, bytes;
	u32 type;
	int ret;

	cnts = get_counters(dev, port_num);

	opfcs = cnts->opfcs;
	type = *(u32 *)cnts->descs[index].priv;
	if (type >= MLX5_IB_OPCOUNTER_MAX)
		return -EINVAL;

	if (!opfcs[type].fc)
		goto out;

	ret = mlx5_fc_query(dev->mdev, opfcs[type].fc,
			    &packets, &bytes);
	if (ret)
		return ret;

	if (is_rdma_bytes_counter(type))
		stats->value[index] = bytes;
	else
		stats->value[index] = packets;
out:
	return index;
}

static int do_get_op_stats(struct ib_device *ibdev,
			   struct rdma_hw_stats *stats,
			   u32 port_num)
{
	struct mlx5_ib_dev *dev = to_mdev(ibdev);
	const struct mlx5_ib_counters *cnts;
	int index, ret, num_hw_counters;

	cnts = get_counters(dev, port_num);
	num_hw_counters = cnts->num_q_counters + cnts->num_cong_counters +
			  cnts->num_ext_ppcnt_counters;
	for (index = num_hw_counters;
	     index < (num_hw_counters + cnts->num_op_counters); index++) {
		ret = do_get_op_stat(ibdev, stats, port_num, index);
		if (ret != index)
			return ret;
	}

	return cnts->num_op_counters;
}

static int mlx5_ib_get_hw_stats(struct ib_device *ibdev,
				struct rdma_hw_stats *stats,
				u32 port_num, int index)
{
	int num_counters, num_hw_counters, num_op_counters;
	struct mlx5_ib_dev *dev = to_mdev(ibdev);
	const struct mlx5_ib_counters *cnts;

	cnts = get_counters(dev, port_num);
	num_hw_counters = cnts->num_q_counters + cnts->num_cong_counters +
		cnts->num_ext_ppcnt_counters;
	num_counters = num_hw_counters + cnts->num_op_counters;

	if (index < 0 || index > num_counters)
		return -EINVAL;
	else if (index > 0 && index < num_hw_counters)
		return do_get_hw_stats(ibdev, stats, port_num, index);
	else if (index >= num_hw_counters && index < num_counters)
		return do_get_op_stat(ibdev, stats, port_num, index);

	num_hw_counters = do_get_hw_stats(ibdev, stats, port_num, index);
	if (num_hw_counters < 0)
		return num_hw_counters;

	num_op_counters = do_get_op_stats(ibdev, stats, port_num);
	if (num_op_counters < 0)
		return num_op_counters;

	return num_hw_counters + num_op_counters;
}

static struct rdma_hw_stats *
mlx5_ib_counter_alloc_stats(struct rdma_counter *counter)
{
	struct mlx5_ib_dev *dev = to_mdev(counter->device);
	const struct mlx5_ib_counters *cnts = get_counters(dev, counter->port);

	return do_alloc_stats(cnts);
}

static int mlx5_ib_counter_update_stats(struct rdma_counter *counter)
{
	struct mlx5_ib_dev *dev = to_mdev(counter->device);
	const struct mlx5_ib_counters *cnts = get_counters(dev, counter->port);
	int ret;

	ret = mlx5_ib_query_q_counters(dev->mdev, cnts, counter->stats,
				       counter->id);
	if (ret)
		return ret;

	if (!counter->mode.bind_opcnt)
		return 0;

	return do_per_qp_get_op_stat(counter);
}

static int mlx5_ib_counter_dealloc(struct rdma_counter *counter)
{
	struct mlx5_rdma_counter *mcounter = to_mcounter(counter);
	struct mlx5_ib_dev *dev = to_mdev(counter->device);
	u32 in[MLX5_ST_SZ_DW(dealloc_q_counter_in)] = {};

	if (!counter->id)
		return 0;

	WARN_ON(!xa_empty(&mcounter->qpn_opfc_xa));
	mlx5r_fs_destroy_fcs(dev, mcounter->fc);
	MLX5_SET(dealloc_q_counter_in, in, opcode,
		 MLX5_CMD_OP_DEALLOC_Q_COUNTER);
	MLX5_SET(dealloc_q_counter_in, in, counter_set_id, counter->id);
	return mlx5_cmd_exec_in(dev->mdev, dealloc_q_counter, in);
}

static int mlx5_ib_counter_bind_qp(struct rdma_counter *counter,
				   struct ib_qp *qp, u32 port)
{
	struct mlx5_rdma_counter *mcounter = to_mcounter(counter);
	struct mlx5_ib_dev *dev = to_mdev(qp->device);
	bool new = false;
	int err;

	if (!counter->id) {
		u32 out[MLX5_ST_SZ_DW(alloc_q_counter_out)] = {};
		u32 in[MLX5_ST_SZ_DW(alloc_q_counter_in)] = {};

		MLX5_SET(alloc_q_counter_in, in, opcode,
			 MLX5_CMD_OP_ALLOC_Q_COUNTER);
		MLX5_SET(alloc_q_counter_in, in, uid, MLX5_SHARED_RESOURCE_UID);
		err = mlx5_cmd_exec_inout(dev->mdev, alloc_q_counter, in, out);
		if (err)
			return err;
		counter->id =
			MLX5_GET(alloc_q_counter_out, out, counter_set_id);
		new = true;
	}

	err = mlx5_ib_qp_set_counter(qp, counter);
	if (err)
		goto fail_set_counter;

	if (!counter->mode.bind_opcnt)
		return 0;

	err = mlx5r_fs_bind_op_fc(qp, mcounter->fc, &mcounter->qpn_opfc_xa,
				  port);
	if (err)
		goto fail_bind_op_fc;

	return 0;

fail_bind_op_fc:
	mlx5_ib_qp_set_counter(qp, NULL);
fail_set_counter:
	if (new) {
		mlx5_ib_counter_dealloc(counter);
		counter->id = 0;
	}

	return err;
}

static int mlx5_ib_counter_unbind_qp(struct ib_qp *qp, u32 port)
{
	struct rdma_counter *counter = qp->counter;
	struct mlx5_rdma_counter *mcounter;
	int err;

	mcounter = to_mcounter(counter);

	mlx5r_fs_unbind_op_fc(qp, &mcounter->qpn_opfc_xa);

	err = mlx5_ib_qp_set_counter(qp, NULL);
	if (err)
		goto fail_set_counter;

	return 0;

fail_set_counter:
	if (counter->mode.bind_opcnt)
		mlx5r_fs_bind_op_fc(qp, mcounter->fc,
				    &mcounter->qpn_opfc_xa, port);
	return err;
}

static void mlx5_ib_fill_counters(struct mlx5_ib_dev *dev,
				  struct rdma_stat_desc *descs, size_t *offsets,
				  u32 port_num)
{
	bool is_vport = is_mdev_switchdev_mode(dev->mdev) &&
			port_num != MLX5_VPORT_PF;
	const struct mlx5_ib_counter *names;
	int j = 0, i, size;

	names = is_vport ? vport_basic_q_cnts : basic_q_cnts;
	size = is_vport ? ARRAY_SIZE(vport_basic_q_cnts) :
			  ARRAY_SIZE(basic_q_cnts);
	for (i = 0; i < size; i++, j++) {
		descs[j].name = names[i].name;
		offsets[j] = names[i].offset;
	}

	names = is_vport ? vport_out_of_seq_q_cnts : out_of_seq_q_cnts;
	size = is_vport ? ARRAY_SIZE(vport_out_of_seq_q_cnts) :
			  ARRAY_SIZE(out_of_seq_q_cnts);
	if (MLX5_CAP_GEN(dev->mdev, out_of_seq_cnt)) {
		for (i = 0; i < size; i++, j++) {
			descs[j].name = names[i].name;
			offsets[j] = names[i].offset;
		}
	}

	names = is_vport ? vport_retrans_q_cnts : retrans_q_cnts;
	size = is_vport ? ARRAY_SIZE(vport_retrans_q_cnts) :
			  ARRAY_SIZE(retrans_q_cnts);
	if (MLX5_CAP_GEN(dev->mdev, retransmission_q_counters)) {
		for (i = 0; i < size; i++, j++) {
			descs[j].name = names[i].name;
			offsets[j] = names[i].offset;
		}
	}

	names = is_vport ? vport_extended_err_cnts : extended_err_cnts;
	size = is_vport ? ARRAY_SIZE(vport_extended_err_cnts) :
			  ARRAY_SIZE(extended_err_cnts);
	if (MLX5_CAP_GEN(dev->mdev, enhanced_error_q_counters)) {
		for (i = 0; i < size; i++, j++) {
			descs[j].name = names[i].name;
			offsets[j] = names[i].offset;
		}
	}

	names = is_vport ? vport_roce_accl_cnts : roce_accl_cnts;
	size = is_vport ? ARRAY_SIZE(vport_roce_accl_cnts) :
			  ARRAY_SIZE(roce_accl_cnts);
	if (MLX5_CAP_GEN(dev->mdev, roce_accl)) {
		for (i = 0; i < size; i++, j++) {
			descs[j].name = names[i].name;
			offsets[j] = names[i].offset;
		}
	}

	if (is_vport)
		return;

	if (MLX5_CAP_GEN(dev->mdev, cc_query_allowed)) {
		for (i = 0; i < ARRAY_SIZE(cong_cnts); i++, j++) {
			descs[j].name = cong_cnts[i].name;
			offsets[j] = cong_cnts[i].offset;
		}
	}

	if (MLX5_CAP_PCAM_FEATURE(dev->mdev, rx_icrc_encapsulated_counter)) {
		for (i = 0; i < ARRAY_SIZE(ext_ppcnt_cnts); i++, j++) {
			descs[j].name = ext_ppcnt_cnts[i].name;
			offsets[j] = ext_ppcnt_cnts[i].offset;
		}
	}

	for (i = 0; i < ARRAY_SIZE(basic_op_cnts); i++, j++) {
		descs[j].name = basic_op_cnts[i].name;
		descs[j].flags |= IB_STAT_FLAG_OPTIONAL;
		descs[j].priv = &basic_op_cnts[i].type;
	}

	if (MLX5_CAP_FLOWTABLE(dev->mdev,
			       ft_field_support_2_nic_receive_rdma.bth_opcode)) {
		for (i = 0; i < ARRAY_SIZE(rdmarx_cnp_op_cnts); i++, j++) {
			descs[j].name = rdmarx_cnp_op_cnts[i].name;
			descs[j].flags |= IB_STAT_FLAG_OPTIONAL;
			descs[j].priv = &rdmarx_cnp_op_cnts[i].type;
		}
	}

	if (MLX5_CAP_FLOWTABLE(dev->mdev,
			       ft_field_support_2_nic_transmit_rdma.bth_opcode)) {
		for (i = 0; i < ARRAY_SIZE(rdmatx_cnp_op_cnts); i++, j++) {
			descs[j].name = rdmatx_cnp_op_cnts[i].name;
			descs[j].flags |= IB_STAT_FLAG_OPTIONAL;
			descs[j].priv = &rdmatx_cnp_op_cnts[i].type;
		}
	}

	for (i = 0; i < ARRAY_SIZE(packets_op_cnts); i++, j++) {
		descs[j].name = packets_op_cnts[i].name;
		descs[j].flags |= IB_STAT_FLAG_OPTIONAL;
		descs[j].priv = &packets_op_cnts[i].type;
	}
}


static int __mlx5_ib_alloc_counters(struct mlx5_ib_dev *dev,
				    struct mlx5_ib_counters *cnts, u32 port_num)
{
	bool is_vport = is_mdev_switchdev_mode(dev->mdev) &&
			port_num != MLX5_VPORT_PF;
	u32 num_counters, num_op_counters = 0, size;

	size = is_vport ? ARRAY_SIZE(vport_basic_q_cnts) :
			  ARRAY_SIZE(basic_q_cnts);
	num_counters = size;

	size = is_vport ? ARRAY_SIZE(vport_out_of_seq_q_cnts) :
			  ARRAY_SIZE(out_of_seq_q_cnts);
	if (MLX5_CAP_GEN(dev->mdev, out_of_seq_cnt))
		num_counters += size;

	size = is_vport ? ARRAY_SIZE(vport_retrans_q_cnts) :
			  ARRAY_SIZE(retrans_q_cnts);
	if (MLX5_CAP_GEN(dev->mdev, retransmission_q_counters))
		num_counters += size;

	size = is_vport ? ARRAY_SIZE(vport_extended_err_cnts) :
			  ARRAY_SIZE(extended_err_cnts);
	if (MLX5_CAP_GEN(dev->mdev, enhanced_error_q_counters))
		num_counters += size;

	size = is_vport ? ARRAY_SIZE(vport_roce_accl_cnts) :
			  ARRAY_SIZE(roce_accl_cnts);
	if (MLX5_CAP_GEN(dev->mdev, roce_accl))
		num_counters += size;

	cnts->num_q_counters = num_counters;

	if (is_vport)
		goto skip_non_qcounters;

	if (MLX5_CAP_GEN(dev->mdev, cc_query_allowed)) {
		cnts->num_cong_counters = ARRAY_SIZE(cong_cnts);
		num_counters += ARRAY_SIZE(cong_cnts);
	}
	if (MLX5_CAP_PCAM_FEATURE(dev->mdev, rx_icrc_encapsulated_counter)) {
		cnts->num_ext_ppcnt_counters = ARRAY_SIZE(ext_ppcnt_cnts);
		num_counters += ARRAY_SIZE(ext_ppcnt_cnts);
	}

	num_op_counters = ARRAY_SIZE(basic_op_cnts);

	num_op_counters += ARRAY_SIZE(packets_op_cnts);

	if (MLX5_CAP_FLOWTABLE(dev->mdev,
			       ft_field_support_2_nic_receive_rdma.bth_opcode))
		num_op_counters += ARRAY_SIZE(rdmarx_cnp_op_cnts);

	if (MLX5_CAP_FLOWTABLE(dev->mdev,
			       ft_field_support_2_nic_transmit_rdma.bth_opcode))
		num_op_counters += ARRAY_SIZE(rdmatx_cnp_op_cnts);

skip_non_qcounters:
	cnts->num_op_counters = num_op_counters;
	num_counters += num_op_counters;
	cnts->descs = kcalloc(num_counters,
			      sizeof(struct rdma_stat_desc), GFP_KERNEL);
	if (!cnts->descs)
		return -ENOMEM;

	cnts->offsets = kcalloc(num_counters,
				sizeof(*cnts->offsets), GFP_KERNEL);
	if (!cnts->offsets)
		goto err;

	return 0;

err:
	kfree(cnts->descs);
	cnts->descs = NULL;
	return -ENOMEM;
}

/*
 * Checks if the given flow counter type should be sharing the same flow counter
 * with another type and if it should, checks if that other type flow counter
 * was already created, if both conditions are met return true and the counter
 * else return false.
 */
bool mlx5r_is_opfc_shared_and_in_use(struct mlx5_ib_op_fc *opfcs, u32 type,
				     struct mlx5_ib_op_fc **opfc)
{
	u32 shared_fc_type;

	switch (type) {
	case MLX5_IB_OPCOUNTER_RDMA_TX_PACKETS:
		shared_fc_type = MLX5_IB_OPCOUNTER_RDMA_TX_BYTES;
		break;
	case MLX5_IB_OPCOUNTER_RDMA_TX_BYTES:
		shared_fc_type = MLX5_IB_OPCOUNTER_RDMA_TX_PACKETS;
		break;
	case MLX5_IB_OPCOUNTER_RDMA_RX_PACKETS:
		shared_fc_type = MLX5_IB_OPCOUNTER_RDMA_RX_BYTES;
		break;
	case MLX5_IB_OPCOUNTER_RDMA_RX_BYTES:
		shared_fc_type = MLX5_IB_OPCOUNTER_RDMA_RX_PACKETS;
		break;
	case MLX5_IB_OPCOUNTER_RDMA_TX_PACKETS_PER_QP:
		shared_fc_type = MLX5_IB_OPCOUNTER_RDMA_TX_BYTES_PER_QP;
		break;
	case MLX5_IB_OPCOUNTER_RDMA_TX_BYTES_PER_QP:
		shared_fc_type = MLX5_IB_OPCOUNTER_RDMA_TX_PACKETS_PER_QP;
		break;
	case MLX5_IB_OPCOUNTER_RDMA_RX_PACKETS_PER_QP:
		shared_fc_type = MLX5_IB_OPCOUNTER_RDMA_RX_BYTES_PER_QP;
		break;
	case MLX5_IB_OPCOUNTER_RDMA_RX_BYTES_PER_QP:
		shared_fc_type = MLX5_IB_OPCOUNTER_RDMA_RX_PACKETS_PER_QP;
		break;
	default:
		return false;
	}

	*opfc = &opfcs[shared_fc_type];
	if (!(*opfc)->fc)
		return false;

	return true;
}

static void mlx5_ib_dealloc_counters(struct mlx5_ib_dev *dev)
{
	u32 in[MLX5_ST_SZ_DW(dealloc_q_counter_in)] = {};
	int num_cnt_ports = dev->num_ports;
	struct mlx5_ib_op_fc *in_use_opfc;
	int i, j;

	if (is_mdev_switchdev_mode(dev->mdev))
		num_cnt_ports = min(2, num_cnt_ports);

	MLX5_SET(dealloc_q_counter_in, in, opcode,
		 MLX5_CMD_OP_DEALLOC_Q_COUNTER);

	for (i = 0; i < num_cnt_ports; i++) {
		if (dev->port[i].cnts.set_id) {
			MLX5_SET(dealloc_q_counter_in, in, counter_set_id,
				 dev->port[i].cnts.set_id);
			mlx5_cmd_exec_in(dev->mdev, dealloc_q_counter, in);
		}
		kfree(dev->port[i].cnts.descs);
		kfree(dev->port[i].cnts.offsets);

		for (j = 0; j < MLX5_IB_OPCOUNTER_MAX; j++) {
			if (!dev->port[i].cnts.opfcs[j].fc)
				continue;

			if (mlx5r_is_opfc_shared_and_in_use(
				    dev->port[i].cnts.opfcs, j, &in_use_opfc))
				goto skip;

			mlx5_ib_fs_remove_op_fc(dev,
						&dev->port[i].cnts.opfcs[j], j);
			mlx5_fc_destroy(dev->mdev,
					dev->port[i].cnts.opfcs[j].fc);
skip:
			dev->port[i].cnts.opfcs[j].fc = NULL;
		}
	}
}

static int mlx5_ib_alloc_counters(struct mlx5_ib_dev *dev)
{
	u32 out[MLX5_ST_SZ_DW(alloc_q_counter_out)] = {};
	u32 in[MLX5_ST_SZ_DW(alloc_q_counter_in)] = {};
	int num_cnt_ports = dev->num_ports;
	int err = 0;
	int i;
	bool is_shared;

	MLX5_SET(alloc_q_counter_in, in, opcode, MLX5_CMD_OP_ALLOC_Q_COUNTER);
	is_shared = MLX5_CAP_GEN(dev->mdev, log_max_uctx) != 0;

	/*
	 * In switchdev we need to allocate two ports, one that is used for
	 * the device Q_counters and it is essentially the real Q_counters of
	 * this device, while the other is used as a helper for PF to be able to
	 * query all other vports.
	 */
	if (is_mdev_switchdev_mode(dev->mdev))
		num_cnt_ports = min(2, num_cnt_ports);

	for (i = 0; i < num_cnt_ports; i++) {
		err = __mlx5_ib_alloc_counters(dev, &dev->port[i].cnts, i);
		if (err)
			goto err_alloc;

		mlx5_ib_fill_counters(dev, dev->port[i].cnts.descs,
				      dev->port[i].cnts.offsets, i);

		MLX5_SET(alloc_q_counter_in, in, uid,
			 is_shared ? MLX5_SHARED_RESOURCE_UID : 0);

		err = mlx5_cmd_exec_inout(dev->mdev, alloc_q_counter, in, out);
		if (err) {
			mlx5_ib_warn(dev,
				     "couldn't allocate queue counter for port %d, err %d\n",
				     i + 1, err);
			goto err_alloc;
		}

		dev->port[i].cnts.set_id =
			MLX5_GET(alloc_q_counter_out, out, counter_set_id);
	}
	return 0;

err_alloc:
	mlx5_ib_dealloc_counters(dev);
	return err;
}

static int read_flow_counters(struct ib_device *ibdev,
			      struct mlx5_read_counters_attr *read_attr)
{
	struct mlx5_fc *fc = read_attr->hw_cntrs_hndl;
	struct mlx5_ib_dev *dev = to_mdev(ibdev);

	return mlx5_fc_query(dev->mdev, fc,
			     &read_attr->out[IB_COUNTER_PACKETS],
			     &read_attr->out[IB_COUNTER_BYTES]);
}

/* flow counters currently expose two counters packets and bytes */
#define FLOW_COUNTERS_NUM 2
static int counters_set_description(
	struct ib_counters *counters, enum mlx5_ib_counters_type counters_type,
	struct mlx5_ib_flow_counters_desc *desc_data, u32 ncounters)
{
	struct mlx5_ib_mcounters *mcounters = to_mcounters(counters);
	u32 cntrs_max_index = 0;
	int i;

	if (counters_type != MLX5_IB_COUNTERS_FLOW)
		return -EINVAL;

	/* init the fields for the object */
	mcounters->type = counters_type;
	mcounters->read_counters = read_flow_counters;
	mcounters->counters_num = FLOW_COUNTERS_NUM;
	mcounters->ncounters = ncounters;
	/* each counter entry have both description and index pair */
	for (i = 0; i < ncounters; i++) {
		if (desc_data[i].description > IB_COUNTER_BYTES)
			return -EINVAL;

		if (cntrs_max_index <= desc_data[i].index)
			cntrs_max_index = desc_data[i].index + 1;
	}

	mutex_lock(&mcounters->mcntrs_mutex);
	mcounters->counters_data = desc_data;
	mcounters->cntrs_max_index = cntrs_max_index;
	mutex_unlock(&mcounters->mcntrs_mutex);

	return 0;
}

#define MAX_COUNTERS_NUM (USHRT_MAX / (sizeof(u32) * 2))
int mlx5_ib_flow_counters_set_data(struct ib_counters *ibcounters,
				   struct mlx5_ib_create_flow *ucmd)
{
	struct mlx5_ib_mcounters *mcounters = to_mcounters(ibcounters);
	struct mlx5_ib_flow_counters_data *cntrs_data = NULL;
	struct mlx5_ib_flow_counters_desc *desc_data = NULL;
	bool hw_hndl = false;
	int ret = 0;

	if (ucmd && ucmd->ncounters_data != 0) {
		cntrs_data = ucmd->data;
		if (cntrs_data->ncounters > MAX_COUNTERS_NUM)
			return -EINVAL;

		desc_data = kcalloc(cntrs_data->ncounters,
				    sizeof(*desc_data),
				    GFP_KERNEL);
		if (!desc_data)
			return  -ENOMEM;

		if (copy_from_user(desc_data,
				   u64_to_user_ptr(cntrs_data->counters_data),
				   sizeof(*desc_data) * cntrs_data->ncounters)) {
			ret = -EFAULT;
			goto free;
		}
	}

	if (!mcounters->hw_cntrs_hndl) {
		mcounters->hw_cntrs_hndl = mlx5_fc_create(
			to_mdev(ibcounters->device)->mdev, false);
		if (IS_ERR(mcounters->hw_cntrs_hndl)) {
			ret = PTR_ERR(mcounters->hw_cntrs_hndl);
			goto free;
		}
		hw_hndl = true;
	}

	if (desc_data) {
		/* counters already bound to at least one flow */
		if (mcounters->cntrs_max_index) {
			ret = -EINVAL;
			goto free_hndl;
		}

		ret = counters_set_description(ibcounters,
					       MLX5_IB_COUNTERS_FLOW,
					       desc_data,
					       cntrs_data->ncounters);
		if (ret)
			goto free_hndl;

	} else if (!mcounters->cntrs_max_index) {
		/* counters not bound yet, must have udata passed */
		ret = -EINVAL;
		goto free_hndl;
	}

	return 0;

free_hndl:
	if (hw_hndl) {
		mlx5_fc_destroy(to_mdev(ibcounters->device)->mdev,
				mcounters->hw_cntrs_hndl);
		mcounters->hw_cntrs_hndl = NULL;
	}
free:
	kfree(desc_data);
	return ret;
}

void mlx5_ib_counters_clear_description(struct ib_counters *counters)
{
	struct mlx5_ib_mcounters *mcounters;

	if (!counters || atomic_read(&counters->usecnt) != 1)
		return;

	mcounters = to_mcounters(counters);

	mutex_lock(&mcounters->mcntrs_mutex);
	kfree(mcounters->counters_data);
	mcounters->counters_data = NULL;
	mcounters->cntrs_max_index = 0;
	mutex_unlock(&mcounters->mcntrs_mutex);
}

static int mlx5_ib_modify_stat(struct ib_device *device, u32 port,
			       unsigned int index, bool enable)
{
	struct mlx5_ib_dev *dev = to_mdev(device);
	struct mlx5_ib_op_fc *opfc, *in_use_opfc;
	struct mlx5_ib_counters *cnts;
	u32 num_hw_counters, type;
	int ret;

	cnts = &dev->port[port - 1].cnts;
	num_hw_counters = cnts->num_q_counters + cnts->num_cong_counters +
		cnts->num_ext_ppcnt_counters;
	if (index < num_hw_counters ||
	    index >= (num_hw_counters + cnts->num_op_counters))
		return -EINVAL;

	if (!(cnts->descs[index].flags & IB_STAT_FLAG_OPTIONAL))
		return -EINVAL;

	type = *(u32 *)cnts->descs[index].priv;
	if (type >= MLX5_IB_OPCOUNTER_MAX)
		return -EINVAL;

	opfc = &cnts->opfcs[type];

	if (enable) {
		if (opfc->fc)
			return -EEXIST;

		if (mlx5r_is_opfc_shared_and_in_use(cnts->opfcs, type,
						    &in_use_opfc)) {
			opfc->fc = in_use_opfc->fc;
			opfc->rule[0] = in_use_opfc->rule[0];
			return 0;
		}

		opfc->fc = mlx5_fc_create(dev->mdev, false);
		if (IS_ERR(opfc->fc))
			return PTR_ERR(opfc->fc);

		ret = mlx5_ib_fs_add_op_fc(dev, port, opfc, type);
		if (ret) {
			mlx5_fc_destroy(dev->mdev, opfc->fc);
			opfc->fc = NULL;
		}
		return ret;
	}

	if (!opfc->fc)
		return -EINVAL;

	if (mlx5r_is_opfc_shared_and_in_use(cnts->opfcs, type, &in_use_opfc))
		goto out;

	mlx5_ib_fs_remove_op_fc(dev, opfc, type);
	mlx5_fc_destroy(dev->mdev, opfc->fc);
out:
	opfc->fc = NULL;
	return 0;
}

static void mlx5_ib_counter_init(struct rdma_counter *counter)
{
	struct mlx5_rdma_counter *mcounter = to_mcounter(counter);

	xa_init(&mcounter->qpn_opfc_xa);
}

static const struct ib_device_ops hw_stats_ops = {
	.alloc_hw_port_stats = mlx5_ib_alloc_hw_port_stats,
	.get_hw_stats = mlx5_ib_get_hw_stats,
	.counter_bind_qp = mlx5_ib_counter_bind_qp,
	.counter_unbind_qp = mlx5_ib_counter_unbind_qp,
	.counter_dealloc = mlx5_ib_counter_dealloc,
	.counter_alloc_stats = mlx5_ib_counter_alloc_stats,
	.counter_update_stats = mlx5_ib_counter_update_stats,
	.modify_hw_stat = mlx5_ib_modify_stat,
	.counter_init = mlx5_ib_counter_init,

	INIT_RDMA_OBJ_SIZE(rdma_counter, mlx5_rdma_counter, rdma_counter),
};

static const struct ib_device_ops hw_switchdev_vport_op = {
	.alloc_hw_port_stats = mlx5_ib_alloc_hw_port_stats,
};

static const struct ib_device_ops hw_switchdev_stats_ops = {
	.alloc_hw_device_stats = mlx5_ib_alloc_hw_device_stats,
	.get_hw_stats = mlx5_ib_get_hw_stats,
	.counter_bind_qp = mlx5_ib_counter_bind_qp,
	.counter_unbind_qp = mlx5_ib_counter_unbind_qp,
	.counter_dealloc = mlx5_ib_counter_dealloc,
	.counter_alloc_stats = mlx5_ib_counter_alloc_stats,
	.counter_update_stats = mlx5_ib_counter_update_stats,
	.counter_init = mlx5_ib_counter_init,

	INIT_RDMA_OBJ_SIZE(rdma_counter, mlx5_rdma_counter, rdma_counter),
};

static const struct ib_device_ops counters_ops = {
	.create_counters = mlx5_ib_create_counters,
	.destroy_counters = mlx5_ib_destroy_counters,
	.read_counters = mlx5_ib_read_counters,

	INIT_RDMA_OBJ_SIZE(ib_counters, mlx5_ib_mcounters, ibcntrs),
};

int mlx5_ib_counters_init(struct mlx5_ib_dev *dev)
{
	ib_set_device_ops(&dev->ib_dev, &counters_ops);

	if (!MLX5_CAP_GEN(dev->mdev, max_qp_cnt))
		return 0;

	if (is_mdev_switchdev_mode(dev->mdev)) {
		ib_set_device_ops(&dev->ib_dev, &hw_switchdev_stats_ops);
		if (vport_qcounters_supported(dev))
			ib_set_device_ops(&dev->ib_dev, &hw_switchdev_vport_op);
	} else
		ib_set_device_ops(&dev->ib_dev, &hw_stats_ops);
	return mlx5_ib_alloc_counters(dev);
}

void mlx5_ib_counters_cleanup(struct mlx5_ib_dev *dev)
{
	if (!MLX5_CAP_GEN(dev->mdev, max_qp_cnt))
		return;

	mlx5_ib_dealloc_counters(dev);
}
