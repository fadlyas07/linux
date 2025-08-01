// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2019 Pensando Systems, Inc */

#include <linux/ethtool.h>
#include <linux/printk.h>
#include <linux/dynamic_debug.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/rtnetlink.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/cpumask.h>
#include <linux/crash_dump.h>
#include <linux/vmalloc.h>
#include <net/page_pool/helpers.h>

#include "ionic.h"
#include "ionic_bus.h"
#include "ionic_dev.h"
#include "ionic_lif.h"
#include "ionic_txrx.h"
#include "ionic_ethtool.h"
#include "ionic_debugfs.h"

/* queuetype support level */
static const u8 ionic_qtype_versions[IONIC_QTYPE_MAX] = {
	[IONIC_QTYPE_ADMINQ]  = 0,   /* 0 = Base version with CQ support */
	[IONIC_QTYPE_NOTIFYQ] = 0,   /* 0 = Base version */
	[IONIC_QTYPE_RXQ]     = 2,   /* 0 = Base version with CQ+SG support
				      * 2 =       ... with CMB rings
				      */
	[IONIC_QTYPE_TXQ]     = 3,   /* 0 = Base version with CQ+SG support
				      * 1 =       ... with Tx SG version 1
				      * 3 =       ... with CMB rings
				      */
};

static void ionic_link_status_check(struct ionic_lif *lif);
static void ionic_lif_handle_fw_down(struct ionic_lif *lif);
static void ionic_lif_handle_fw_up(struct ionic_lif *lif);
static void ionic_lif_set_netdev_info(struct ionic_lif *lif);

static void ionic_txrx_deinit(struct ionic_lif *lif);
static int ionic_txrx_init(struct ionic_lif *lif);
static int ionic_start_queues(struct ionic_lif *lif);
static void ionic_stop_queues(struct ionic_lif *lif);
static void ionic_lif_queue_identify(struct ionic_lif *lif);

static void ionic_xdp_rxqs_prog_update(struct ionic_lif *lif);
static void ionic_unregister_rxq_info(struct ionic_queue *q);
static int ionic_register_rxq_info(struct ionic_queue *q, unsigned int napi_id);

static void ionic_dim_work(struct work_struct *work)
{
	struct dim *dim = container_of(work, struct dim, work);
	struct dim_cq_moder cur_moder;
	struct ionic_intr_info *intr;
	struct ionic_qcq *qcq;
	struct ionic_lif *lif;
	struct ionic_queue *q;
	u32 new_coal;

	qcq = container_of(dim, struct ionic_qcq, dim);
	q = &qcq->q;
	if (q->type == IONIC_QTYPE_RXQ)
		cur_moder = net_dim_get_rx_moderation(dim->mode, dim->profile_ix);
	else
		cur_moder = net_dim_get_tx_moderation(dim->mode, dim->profile_ix);
	lif = q->lif;
	new_coal = ionic_coal_usec_to_hw(lif->ionic, cur_moder.usec);
	new_coal = new_coal ? new_coal : 1;

	intr = &qcq->intr;
	if (intr->dim_coal_hw != new_coal) {
		intr->dim_coal_hw = new_coal;

		ionic_intr_coal_init(lif->ionic->idev.intr_ctrl,
				     intr->index, intr->dim_coal_hw);
	}

	dim->state = DIM_START_MEASURE;
}

static void ionic_lif_deferred_work(struct work_struct *work)
{
	struct ionic_lif *lif = container_of(work, struct ionic_lif, deferred.work);
	struct ionic_deferred *def = &lif->deferred;
	struct ionic_deferred_work *w = NULL;

	do {
		spin_lock_bh(&def->lock);
		if (!list_empty(&def->list)) {
			w = list_first_entry(&def->list,
					     struct ionic_deferred_work, list);
			list_del(&w->list);
		}
		spin_unlock_bh(&def->lock);

		if (!w)
			break;

		switch (w->type) {
		case IONIC_DW_TYPE_RX_MODE:
			ionic_lif_rx_mode(lif);
			break;
		case IONIC_DW_TYPE_LINK_STATUS:
			ionic_link_status_check(lif);
			break;
		case IONIC_DW_TYPE_LIF_RESET:
			if (w->fw_status) {
				ionic_lif_handle_fw_up(lif);
			} else {
				ionic_lif_handle_fw_down(lif);

				/* Fire off another watchdog to see
				 * if the FW is already back rather than
				 * waiting another whole cycle
				 */
				mod_timer(&lif->ionic->watchdog_timer, jiffies + 1);
			}
			break;
		default:
			break;
		}
		kfree(w);
		w = NULL;
	} while (true);
}

void ionic_lif_deferred_enqueue(struct ionic_lif *lif,
				struct ionic_deferred_work *work)
{
	spin_lock_bh(&lif->deferred.lock);
	list_add_tail(&work->list, &lif->deferred.list);
	spin_unlock_bh(&lif->deferred.lock);
	queue_work(lif->ionic->wq, &lif->deferred.work);
}

static void ionic_link_status_check(struct ionic_lif *lif)
{
	struct net_device *netdev = lif->netdev;
	u16 link_status;
	bool link_up;

	if (!test_bit(IONIC_LIF_F_LINK_CHECK_REQUESTED, lif->state))
		return;

	/* Don't put carrier back up if we're in a broken state */
	if (test_bit(IONIC_LIF_F_BROKEN, lif->state)) {
		clear_bit(IONIC_LIF_F_LINK_CHECK_REQUESTED, lif->state);
		return;
	}

	link_status = le16_to_cpu(lif->info->status.link_status);
	link_up = link_status == IONIC_PORT_OPER_STATUS_UP;

	if (link_up) {
		int err = 0;

		if (netdev->flags & IFF_UP && netif_running(netdev)) {
			mutex_lock(&lif->queue_lock);
			err = ionic_start_queues(lif);
			if (err && err != -EBUSY) {
				netdev_err(netdev,
					   "Failed to start queues: %d\n", err);
				set_bit(IONIC_LIF_F_BROKEN, lif->state);
				netif_carrier_off(lif->netdev);
			}
			mutex_unlock(&lif->queue_lock);
		}

		if (!err && !netif_carrier_ok(netdev)) {
			ionic_port_identify(lif->ionic);
			netdev_info(netdev, "Link up - %d Gbps\n",
				    le32_to_cpu(lif->info->status.link_speed) / 1000);
			netif_carrier_on(netdev);
		}
	} else {
		if (netif_carrier_ok(netdev)) {
			lif->link_down_count++;
			netdev_info(netdev, "Link down\n");
			netif_carrier_off(netdev);
		}

		if (netdev->flags & IFF_UP && netif_running(netdev)) {
			mutex_lock(&lif->queue_lock);
			ionic_stop_queues(lif);
			mutex_unlock(&lif->queue_lock);
		}
	}

	clear_bit(IONIC_LIF_F_LINK_CHECK_REQUESTED, lif->state);
}

void ionic_link_status_check_request(struct ionic_lif *lif, bool can_sleep)
{
	struct ionic_deferred_work *work;

	/* we only need one request outstanding at a time */
	if (test_and_set_bit(IONIC_LIF_F_LINK_CHECK_REQUESTED, lif->state))
		return;

	if (!can_sleep) {
		work = kzalloc(sizeof(*work), GFP_ATOMIC);
		if (!work) {
			clear_bit(IONIC_LIF_F_LINK_CHECK_REQUESTED, lif->state);
			return;
		}

		work->type = IONIC_DW_TYPE_LINK_STATUS;
		ionic_lif_deferred_enqueue(lif, work);
	} else {
		ionic_link_status_check(lif);
	}
}

static irqreturn_t ionic_isr(int irq, void *data)
{
	struct napi_struct *napi = data;

	napi_schedule_irqoff(napi);

	return IRQ_HANDLED;
}

static int ionic_request_irq(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct ionic_intr_info *intr = &qcq->intr;
	struct device *dev = lif->ionic->dev;
	struct ionic_queue *q = &qcq->q;
	const char *name;

	if (lif->registered)
		name = netdev_name(lif->netdev);
	else
		name = dev_name(dev);

	snprintf(intr->name, sizeof(intr->name),
		 "%.5s-%.16s-%.8s", IONIC_DRV_NAME, name, q->name);

	return devm_request_irq(dev, intr->vector, ionic_isr,
				0, intr->name, &qcq->napi);
}

static int ionic_intr_alloc(struct ionic_lif *lif, struct ionic_intr_info *intr)
{
	struct ionic *ionic = lif->ionic;
	int index;

	index = find_first_zero_bit(ionic->intrs, ionic->nintrs);
	if (index == ionic->nintrs) {
		netdev_warn(lif->netdev, "%s: no intr, index=%d nintrs=%d\n",
			    __func__, index, ionic->nintrs);
		return -ENOSPC;
	}

	set_bit(index, ionic->intrs);
	ionic_intr_init(&ionic->idev, intr, index);

	return 0;
}

static void ionic_intr_free(struct ionic *ionic, int index)
{
	if (index != IONIC_INTR_INDEX_NOT_ASSIGNED && index < ionic->nintrs)
		clear_bit(index, ionic->intrs);
}

static void ionic_irq_aff_notify(struct irq_affinity_notify *notify,
				 const cpumask_t *mask)
{
	struct ionic_intr_info *intr = container_of(notify, struct ionic_intr_info, aff_notify);

	cpumask_copy(*intr->affinity_mask, mask);
}

static void ionic_irq_aff_release(struct kref __always_unused *ref)
{
}

static int ionic_qcq_enable(struct ionic_qcq *qcq)
{
	struct ionic_queue *q = &qcq->q;
	struct ionic_lif *lif = q->lif;
	struct ionic_dev *idev;
	struct device *dev;

	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_control = {
			.opcode = IONIC_CMD_Q_CONTROL,
			.lif_index = cpu_to_le16(lif->index),
			.type = q->type,
			.index = cpu_to_le32(q->index),
			.oper = IONIC_Q_ENABLE,
		},
	};
	int ret;

	idev = &lif->ionic->idev;
	dev = lif->ionic->dev;

	dev_dbg(dev, "q_enable.index %d q_enable.qtype %d\n",
		ctx.cmd.q_control.index, ctx.cmd.q_control.type);

	if (qcq->flags & IONIC_QCQ_F_INTR)
		ionic_intr_clean(idev->intr_ctrl, qcq->intr.index);

	ret = ionic_adminq_post_wait(lif, &ctx);
	if (ret)
		return ret;

	if (qcq->flags & IONIC_QCQ_F_INTR) {
		napi_enable(&qcq->napi);
		irq_set_affinity_notifier(qcq->intr.vector,
					  &qcq->intr.aff_notify);
		irq_set_affinity_hint(qcq->intr.vector,
				      *qcq->intr.affinity_mask);
		ionic_intr_mask(idev->intr_ctrl, qcq->intr.index,
				IONIC_INTR_MASK_CLEAR);
	}

	return 0;
}

static int ionic_qcq_disable(struct ionic_lif *lif, struct ionic_qcq *qcq, int fw_err)
{
	struct ionic_queue *q;

	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_control = {
			.opcode = IONIC_CMD_Q_CONTROL,
			.oper = IONIC_Q_DISABLE,
		},
	};

	if (!qcq) {
		netdev_err(lif->netdev, "%s: bad qcq\n", __func__);
		return -ENXIO;
	}

	q = &qcq->q;

	if (qcq->flags & IONIC_QCQ_F_INTR) {
		struct ionic_dev *idev = &lif->ionic->idev;

		if (lif->doorbell_wa)
			cancel_work_sync(&qcq->doorbell_napi_work);
		cancel_work_sync(&qcq->dim.work);
		ionic_intr_mask(idev->intr_ctrl, qcq->intr.index,
				IONIC_INTR_MASK_SET);
		synchronize_irq(qcq->intr.vector);
		irq_set_affinity_notifier(qcq->intr.vector, NULL);
		irq_set_affinity_hint(qcq->intr.vector, NULL);
		napi_disable(&qcq->napi);
	}

	/* If there was a previous fw communcation error, don't bother with
	 * sending the adminq command and just return the same error value.
	 */
	if (fw_err == -ETIMEDOUT || fw_err == -ENXIO)
		return fw_err;

	ctx.cmd.q_control.lif_index = cpu_to_le16(lif->index);
	ctx.cmd.q_control.type = q->type;
	ctx.cmd.q_control.index = cpu_to_le32(q->index);
	dev_dbg(lif->ionic->dev, "q_disable.index %d q_disable.qtype %d\n",
		ctx.cmd.q_control.index, ctx.cmd.q_control.type);

	return ionic_adminq_post_wait(lif, &ctx);
}

static void ionic_lif_qcq_deinit(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct ionic_dev *idev = &lif->ionic->idev;

	if (!qcq)
		return;

	if (!(qcq->flags & IONIC_QCQ_F_INITED))
		return;

	ionic_unregister_rxq_info(&qcq->q);
	if (qcq->flags & IONIC_QCQ_F_INTR) {
		ionic_intr_mask(idev->intr_ctrl, qcq->intr.index,
				IONIC_INTR_MASK_SET);
		netif_napi_del(&qcq->napi);
	}

	qcq->flags &= ~IONIC_QCQ_F_INITED;
}

static void ionic_qcq_intr_free(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	if (!(qcq->flags & IONIC_QCQ_F_INTR) || qcq->intr.vector == 0)
		return;

	irq_set_affinity_hint(qcq->intr.vector, NULL);
	devm_free_irq(lif->ionic->dev, qcq->intr.vector, &qcq->napi);
	qcq->intr.vector = 0;
	ionic_intr_free(lif->ionic, qcq->intr.index);
	qcq->intr.index = IONIC_INTR_INDEX_NOT_ASSIGNED;
}

static void ionic_qcq_free(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct device *dev = lif->ionic->dev;

	if (!qcq)
		return;

	ionic_debugfs_del_qcq(qcq);

	if (qcq->q_base) {
		dma_free_coherent(dev, qcq->q_size, qcq->q_base, qcq->q_base_pa);
		qcq->q_base = NULL;
		qcq->q_base_pa = 0;
	}

	if (qcq->cmb_q_base) {
		iounmap(qcq->cmb_q_base);
		ionic_put_cmb(lif, qcq->cmb_pgid, qcq->cmb_order);
		qcq->cmb_pgid = 0;
		qcq->cmb_order = 0;
		qcq->cmb_q_base = NULL;
		qcq->cmb_q_base_pa = 0;
	}

	if (qcq->cq_base) {
		dma_free_coherent(dev, qcq->cq_size, qcq->cq_base, qcq->cq_base_pa);
		qcq->cq_base = NULL;
		qcq->cq_base_pa = 0;
	}

	if (qcq->sg_base) {
		dma_free_coherent(dev, qcq->sg_size, qcq->sg_base, qcq->sg_base_pa);
		qcq->sg_base = NULL;
		qcq->sg_base_pa = 0;
	}

	page_pool_destroy(qcq->q.page_pool);
	qcq->q.page_pool = NULL;

	ionic_qcq_intr_free(lif, qcq);
	vfree(qcq->q.info);
	qcq->q.info = NULL;
}

void ionic_qcqs_free(struct ionic_lif *lif)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_qcq *adminqcq;
	unsigned long irqflags;

	if (lif->notifyqcq) {
		ionic_qcq_free(lif, lif->notifyqcq);
		devm_kfree(dev, lif->notifyqcq);
		lif->notifyqcq = NULL;
	}

	if (lif->adminqcq) {
		spin_lock_irqsave(&lif->adminq_lock, irqflags);
		adminqcq = READ_ONCE(lif->adminqcq);
		lif->adminqcq = NULL;
		spin_unlock_irqrestore(&lif->adminq_lock, irqflags);
		if (adminqcq) {
			ionic_qcq_free(lif, adminqcq);
			devm_kfree(dev, adminqcq);
		}
	}

	if (lif->rxqcqs) {
		devm_kfree(dev, lif->rxqstats);
		lif->rxqstats = NULL;
		devm_kfree(dev, lif->rxqcqs);
		lif->rxqcqs = NULL;
	}

	if (lif->txqcqs) {
		devm_kfree(dev, lif->txqstats);
		lif->txqstats = NULL;
		devm_kfree(dev, lif->txqcqs);
		lif->txqcqs = NULL;
	}
}

static void ionic_link_qcq_interrupts(struct ionic_qcq *src_qcq,
				      struct ionic_qcq *n_qcq)
{
	n_qcq->intr.vector = src_qcq->intr.vector;
	n_qcq->intr.index = src_qcq->intr.index;
}

static int ionic_alloc_qcq_interrupt(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	cpumask_var_t *affinity_mask;
	int err;

	if (!(qcq->flags & IONIC_QCQ_F_INTR)) {
		qcq->intr.index = IONIC_INTR_INDEX_NOT_ASSIGNED;
		return 0;
	}

	err = ionic_intr_alloc(lif, &qcq->intr);
	if (err) {
		netdev_warn(lif->netdev, "no intr for %s: %d\n",
			    qcq->q.name, err);
		goto err_out;
	}

	err = ionic_bus_get_irq(lif->ionic, qcq->intr.index);
	if (err < 0) {
		netdev_warn(lif->netdev, "no vector for %s: %d\n",
			    qcq->q.name, err);
		goto err_out_free_intr;
	}
	qcq->intr.vector = err;
	ionic_intr_mask_assert(lif->ionic->idev.intr_ctrl, qcq->intr.index,
			       IONIC_INTR_MASK_SET);

	err = ionic_request_irq(lif, qcq);
	if (err) {
		netdev_warn(lif->netdev, "irq request failed %d\n", err);
		goto err_out_free_intr;
	}

	/* try to get the irq on the local numa node first */
	affinity_mask = &lif->ionic->affinity_masks[qcq->intr.index];
	if (cpumask_empty(*affinity_mask)) {
		unsigned int cpu;

		cpu = cpumask_local_spread(qcq->intr.index,
					   dev_to_node(lif->ionic->dev));
		if (cpu != -1)
			cpumask_set_cpu(cpu, *affinity_mask);
	}

	qcq->intr.affinity_mask = affinity_mask;
	qcq->intr.aff_notify.notify = ionic_irq_aff_notify;
	qcq->intr.aff_notify.release = ionic_irq_aff_release;

	netdev_dbg(lif->netdev, "%s: Interrupt index %d\n", qcq->q.name, qcq->intr.index);
	return 0;

err_out_free_intr:
	ionic_intr_free(lif->ionic, qcq->intr.index);
err_out:
	return err;
}

static int ionic_qcq_alloc(struct ionic_lif *lif, unsigned int type,
			   unsigned int index,
			   const char *name, unsigned int flags,
			   unsigned int num_descs, unsigned int desc_size,
			   unsigned int cq_desc_size,
			   unsigned int sg_desc_size,
			   unsigned int desc_info_size,
			   unsigned int pid, struct bpf_prog *xdp_prog,
			   struct ionic_qcq **qcq)
{
	struct ionic_dev *idev = &lif->ionic->idev;
	struct device *dev = lif->ionic->dev;
	struct ionic_qcq *new;
	int err;

	*qcq = NULL;

	new = devm_kzalloc(dev, sizeof(*new), GFP_KERNEL);
	if (!new) {
		netdev_err(lif->netdev, "Cannot allocate queue structure\n");
		err = -ENOMEM;
		goto err_out;
	}

	new->q.dev = dev;
	new->flags = flags;

	new->q.info = vcalloc(num_descs, desc_info_size);
	if (!new->q.info) {
		netdev_err(lif->netdev, "Cannot allocate queue info\n");
		err = -ENOMEM;
		goto err_out_free_qcq;
	}

	if (type == IONIC_QTYPE_RXQ) {
		struct page_pool_params pp_params = {
			.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
			.order = 0,
			.pool_size = num_descs,
			.nid = NUMA_NO_NODE,
			.dev = lif->ionic->dev,
			.napi = &new->napi,
			.dma_dir = DMA_FROM_DEVICE,
			.max_len = PAGE_SIZE,
			.netdev = lif->netdev,
		};

		if (xdp_prog)
			pp_params.dma_dir = DMA_BIDIRECTIONAL;

		new->q.page_pool = page_pool_create(&pp_params);
		if (IS_ERR(new->q.page_pool)) {
			netdev_err(lif->netdev, "Cannot create page_pool\n");
			err = PTR_ERR(new->q.page_pool);
			new->q.page_pool = NULL;
			goto err_out_free_q_info;
		}
	}

	new->q.type = type;
	new->q.max_sg_elems = lif->qtype_info[type].max_sg_elems;

	err = ionic_q_init(lif, idev, &new->q, index, name, num_descs,
			   desc_size, sg_desc_size, pid);
	if (err) {
		netdev_err(lif->netdev, "Cannot initialize queue\n");
		goto err_out_free_page_pool;
	}

	err = ionic_alloc_qcq_interrupt(lif, new);
	if (err)
		goto err_out_free_page_pool;

	err = ionic_cq_init(lif, &new->cq, &new->intr, num_descs, cq_desc_size);
	if (err) {
		netdev_err(lif->netdev, "Cannot initialize completion queue\n");
		goto err_out_free_irq;
	}

	if (flags & IONIC_QCQ_F_NOTIFYQ) {
		int q_size;

		/* q & cq need to be contiguous in NotifyQ, so alloc it all in q
		 * and don't alloc qc.  We leave new->qc_size and new->qc_base
		 * as 0 to be sure we don't try to free it later.
		 */
		q_size = ALIGN(num_descs * desc_size, PAGE_SIZE);
		new->q_size = PAGE_SIZE + q_size +
			      ALIGN(num_descs * cq_desc_size, PAGE_SIZE);
		new->q_base = dma_alloc_coherent(dev, new->q_size,
						 &new->q_base_pa, GFP_KERNEL);
		if (!new->q_base) {
			netdev_err(lif->netdev, "Cannot allocate qcq DMA memory\n");
			err = -ENOMEM;
			goto err_out_free_irq;
		}
		new->q.base = PTR_ALIGN(new->q_base, PAGE_SIZE);
		new->q.base_pa = ALIGN(new->q_base_pa, PAGE_SIZE);

		/* Base the NotifyQ cq.base off of the ALIGNed q.base */
		new->cq.base = PTR_ALIGN(new->q.base + q_size, PAGE_SIZE);
		new->cq.base_pa = ALIGN(new->q_base_pa + q_size, PAGE_SIZE);
		new->cq.bound_q = &new->q;
	} else {
		/* regular DMA q descriptors */
		new->q_size = PAGE_SIZE + (num_descs * desc_size);
		new->q_base = dma_alloc_coherent(dev, new->q_size, &new->q_base_pa,
						 GFP_KERNEL);
		if (!new->q_base) {
			netdev_err(lif->netdev, "Cannot allocate queue DMA memory\n");
			err = -ENOMEM;
			goto err_out_free_irq;
		}
		new->q.base = PTR_ALIGN(new->q_base, PAGE_SIZE);
		new->q.base_pa = ALIGN(new->q_base_pa, PAGE_SIZE);

		if (flags & IONIC_QCQ_F_CMB_RINGS) {
			/* on-chip CMB q descriptors */
			new->cmb_q_size = num_descs * desc_size;
			new->cmb_order = order_base_2(new->cmb_q_size / PAGE_SIZE);

			err = ionic_get_cmb(lif, &new->cmb_pgid, &new->cmb_q_base_pa,
					    new->cmb_order);
			if (err) {
				netdev_err(lif->netdev,
					   "Cannot allocate queue order %d from cmb: err %d\n",
					   new->cmb_order, err);
				goto err_out_free_q;
			}

			new->cmb_q_base = ioremap_wc(new->cmb_q_base_pa, new->cmb_q_size);
			if (!new->cmb_q_base) {
				netdev_err(lif->netdev, "Cannot map queue from cmb\n");
				ionic_put_cmb(lif, new->cmb_pgid, new->cmb_order);
				err = -ENOMEM;
				goto err_out_free_q;
			}

			new->cmb_q_base_pa -= idev->phy_cmb_pages;
			new->q.cmb_base = new->cmb_q_base;
			new->q.cmb_base_pa = new->cmb_q_base_pa;
		}

		/* cq DMA descriptors */
		new->cq_size = PAGE_SIZE + (num_descs * cq_desc_size);
		new->cq_base = dma_alloc_coherent(dev, new->cq_size, &new->cq_base_pa,
						  GFP_KERNEL);
		if (!new->cq_base) {
			netdev_err(lif->netdev, "Cannot allocate cq DMA memory\n");
			err = -ENOMEM;
			goto err_out_free_q;
		}
		new->cq.base = PTR_ALIGN(new->cq_base, PAGE_SIZE);
		new->cq.base_pa = ALIGN(new->cq_base_pa, PAGE_SIZE);
		new->cq.bound_q = &new->q;
	}

	if (flags & IONIC_QCQ_F_SG) {
		new->sg_size = PAGE_SIZE + (num_descs * sg_desc_size);
		new->sg_base = dma_alloc_coherent(dev, new->sg_size, &new->sg_base_pa,
						  GFP_KERNEL);
		if (!new->sg_base) {
			netdev_err(lif->netdev, "Cannot allocate sg DMA memory\n");
			err = -ENOMEM;
			goto err_out_free_cq;
		}
		new->q.sg_base = PTR_ALIGN(new->sg_base, PAGE_SIZE);
		new->q.sg_base_pa = ALIGN(new->sg_base_pa, PAGE_SIZE);
	}

	INIT_WORK(&new->dim.work, ionic_dim_work);
	new->dim.mode = DIM_CQ_PERIOD_MODE_START_FROM_CQE;
	if (lif->doorbell_wa)
		INIT_WORK(&new->doorbell_napi_work, ionic_doorbell_napi_work);

	*qcq = new;

	return 0;

err_out_free_cq:
	dma_free_coherent(dev, new->cq_size, new->cq_base, new->cq_base_pa);
err_out_free_q:
	if (new->cmb_q_base) {
		iounmap(new->cmb_q_base);
		ionic_put_cmb(lif, new->cmb_pgid, new->cmb_order);
	}
	dma_free_coherent(dev, new->q_size, new->q_base, new->q_base_pa);
err_out_free_irq:
	if (flags & IONIC_QCQ_F_INTR) {
		devm_free_irq(dev, new->intr.vector, &new->napi);
		ionic_intr_free(lif->ionic, new->intr.index);
	}
err_out_free_page_pool:
	page_pool_destroy(new->q.page_pool);
err_out_free_q_info:
	vfree(new->q.info);
err_out_free_qcq:
	devm_kfree(dev, new);
err_out:
	dev_err(dev, "qcq alloc of %s%d failed %d\n", name, index, err);
	return err;
}

static int ionic_qcqs_alloc(struct ionic_lif *lif)
{
	struct device *dev = lif->ionic->dev;
	unsigned int flags;
	int err;

	flags = IONIC_QCQ_F_INTR;
	err = ionic_qcq_alloc(lif, IONIC_QTYPE_ADMINQ, 0, "admin", flags,
			      IONIC_ADMINQ_LENGTH,
			      sizeof(struct ionic_admin_cmd),
			      sizeof(struct ionic_admin_comp),
			      0,
			      sizeof(struct ionic_admin_desc_info),
			      lif->kern_pid, NULL, &lif->adminqcq);
	if (err)
		return err;
	ionic_debugfs_add_qcq(lif, lif->adminqcq);

	if (lif->ionic->nnqs_per_lif) {
		flags = IONIC_QCQ_F_NOTIFYQ;
		err = ionic_qcq_alloc(lif, IONIC_QTYPE_NOTIFYQ, 0, "notifyq",
				      flags, IONIC_NOTIFYQ_LENGTH,
				      sizeof(struct ionic_notifyq_cmd),
				      sizeof(union ionic_notifyq_comp),
				      0,
				      sizeof(struct ionic_admin_desc_info),
				      lif->kern_pid, NULL, &lif->notifyqcq);
		if (err)
			goto err_out;
		ionic_debugfs_add_qcq(lif, lif->notifyqcq);

		/* Let the notifyq ride on the adminq interrupt */
		ionic_link_qcq_interrupts(lif->adminqcq, lif->notifyqcq);
	}

	err = -ENOMEM;
	lif->txqcqs = devm_kcalloc(dev, lif->ionic->ntxqs_per_lif,
				   sizeof(*lif->txqcqs), GFP_KERNEL);
	if (!lif->txqcqs)
		goto err_out;
	lif->rxqcqs = devm_kcalloc(dev, lif->ionic->nrxqs_per_lif,
				   sizeof(*lif->rxqcqs), GFP_KERNEL);
	if (!lif->rxqcqs)
		goto err_out;

	lif->txqstats = devm_kcalloc(dev, lif->ionic->ntxqs_per_lif + 1,
				     sizeof(*lif->txqstats), GFP_KERNEL);
	if (!lif->txqstats)
		goto err_out;
	lif->rxqstats = devm_kcalloc(dev, lif->ionic->nrxqs_per_lif + 1,
				     sizeof(*lif->rxqstats), GFP_KERNEL);
	if (!lif->rxqstats)
		goto err_out;

	return 0;

err_out:
	ionic_qcqs_free(lif);
	return err;
}

static void ionic_qcq_sanitize(struct ionic_qcq *qcq)
{
	qcq->q.tail_idx = 0;
	qcq->q.head_idx = 0;
	qcq->cq.tail_idx = 0;
	qcq->cq.done_color = 1;
	memset(qcq->q_base, 0, qcq->q_size);
	if (qcq->cmb_q_base)
		memset_io(qcq->cmb_q_base, 0, qcq->cmb_q_size);
	memset(qcq->cq_base, 0, qcq->cq_size);
	memset(qcq->sg_base, 0, qcq->sg_size);
}

static int ionic_lif_txq_init(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_queue *q = &qcq->q;
	struct ionic_cq *cq = &qcq->cq;
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_init = {
			.opcode = IONIC_CMD_Q_INIT,
			.lif_index = cpu_to_le16(lif->index),
			.type = q->type,
			.ver = lif->qtype_info[q->type].version,
			.index = cpu_to_le32(q->index),
			.flags = cpu_to_le16(IONIC_QINIT_F_IRQ |
					     IONIC_QINIT_F_SG),
			.intr_index = cpu_to_le16(qcq->intr.index),
			.pid = cpu_to_le16(q->pid),
			.ring_size = ilog2(q->num_descs),
			.ring_base = cpu_to_le64(q->base_pa),
			.cq_ring_base = cpu_to_le64(cq->base_pa),
			.sg_ring_base = cpu_to_le64(q->sg_base_pa),
			.features = cpu_to_le64(q->features),
		},
	};
	int err;

	if (qcq->flags & IONIC_QCQ_F_CMB_RINGS) {
		ctx.cmd.q_init.flags |= cpu_to_le16(IONIC_QINIT_F_CMB);
		ctx.cmd.q_init.ring_base = cpu_to_le64(qcq->cmb_q_base_pa);
	}

	dev_dbg(dev, "txq_init.pid %d\n", ctx.cmd.q_init.pid);
	dev_dbg(dev, "txq_init.index %d\n", ctx.cmd.q_init.index);
	dev_dbg(dev, "txq_init.ring_base 0x%llx\n", ctx.cmd.q_init.ring_base);
	dev_dbg(dev, "txq_init.ring_size %d\n", ctx.cmd.q_init.ring_size);
	dev_dbg(dev, "txq_init.cq_ring_base 0x%llx\n", ctx.cmd.q_init.cq_ring_base);
	dev_dbg(dev, "txq_init.sg_ring_base 0x%llx\n", ctx.cmd.q_init.sg_ring_base);
	dev_dbg(dev, "txq_init.flags 0x%x\n", ctx.cmd.q_init.flags);
	dev_dbg(dev, "txq_init.ver %d\n", ctx.cmd.q_init.ver);
	dev_dbg(dev, "txq_init.intr_index %d\n", ctx.cmd.q_init.intr_index);

	ionic_qcq_sanitize(qcq);

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	q->hw_type = ctx.comp.q_init.hw_type;
	q->hw_index = le32_to_cpu(ctx.comp.q_init.hw_index);
	q->dbval = IONIC_DBELL_QID(q->hw_index);

	dev_dbg(dev, "txq->hw_type %d\n", q->hw_type);
	dev_dbg(dev, "txq->hw_index %d\n", q->hw_index);

	q->dbell_deadline = IONIC_TX_DOORBELL_DEADLINE;
	q->dbell_jiffies = jiffies;

	if (test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state))
		netif_napi_add(lif->netdev, &qcq->napi, ionic_tx_napi);

	qcq->flags |= IONIC_QCQ_F_INITED;

	return 0;
}

static int ionic_lif_rxq_init(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_queue *q = &qcq->q;
	struct ionic_cq *cq = &qcq->cq;
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_init = {
			.opcode = IONIC_CMD_Q_INIT,
			.lif_index = cpu_to_le16(lif->index),
			.type = q->type,
			.ver = lif->qtype_info[q->type].version,
			.index = cpu_to_le32(q->index),
			.flags = cpu_to_le16(IONIC_QINIT_F_IRQ),
			.intr_index = cpu_to_le16(cq->bound_intr->index),
			.pid = cpu_to_le16(q->pid),
			.ring_size = ilog2(q->num_descs),
			.ring_base = cpu_to_le64(q->base_pa),
			.cq_ring_base = cpu_to_le64(cq->base_pa),
			.sg_ring_base = cpu_to_le64(q->sg_base_pa),
			.features = cpu_to_le64(q->features),
		},
	};
	int err;

	q->partner = &lif->txqcqs[q->index]->q;
	q->partner->partner = q;

	if (!lif->xdp_prog ||
	    (lif->xdp_prog->aux && lif->xdp_prog->aux->xdp_has_frags))
		ctx.cmd.q_init.flags |= cpu_to_le16(IONIC_QINIT_F_SG);

	if (qcq->flags & IONIC_QCQ_F_CMB_RINGS) {
		ctx.cmd.q_init.flags |= cpu_to_le16(IONIC_QINIT_F_CMB);
		ctx.cmd.q_init.ring_base = cpu_to_le64(qcq->cmb_q_base_pa);
	}

	dev_dbg(dev, "rxq_init.pid %d\n", ctx.cmd.q_init.pid);
	dev_dbg(dev, "rxq_init.index %d\n", ctx.cmd.q_init.index);
	dev_dbg(dev, "rxq_init.ring_base 0x%llx\n", ctx.cmd.q_init.ring_base);
	dev_dbg(dev, "rxq_init.ring_size %d\n", ctx.cmd.q_init.ring_size);
	dev_dbg(dev, "rxq_init.flags 0x%x\n", ctx.cmd.q_init.flags);
	dev_dbg(dev, "rxq_init.ver %d\n", ctx.cmd.q_init.ver);
	dev_dbg(dev, "rxq_init.intr_index %d\n", ctx.cmd.q_init.intr_index);

	ionic_qcq_sanitize(qcq);

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	q->hw_type = ctx.comp.q_init.hw_type;
	q->hw_index = le32_to_cpu(ctx.comp.q_init.hw_index);
	q->dbval = IONIC_DBELL_QID(q->hw_index);

	dev_dbg(dev, "rxq->hw_type %d\n", q->hw_type);
	dev_dbg(dev, "rxq->hw_index %d\n", q->hw_index);

	q->dbell_deadline = IONIC_RX_MIN_DOORBELL_DEADLINE;
	q->dbell_jiffies = jiffies;

	if (test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state))
		netif_napi_add(lif->netdev, &qcq->napi, ionic_rx_napi);
	else
		netif_napi_add(lif->netdev, &qcq->napi, ionic_txrx_napi);
	err = ionic_register_rxq_info(q, qcq->napi.napi_id);
	if (err) {
		netif_napi_del(&qcq->napi);
		return err;
	}

	qcq->flags |= IONIC_QCQ_F_INITED;

	return 0;
}

int ionic_lif_create_hwstamp_txq(struct ionic_lif *lif)
{
	unsigned int num_desc, desc_sz, comp_sz, sg_desc_sz;
	unsigned int txq_i, flags;
	struct ionic_qcq *txq;
	u64 features;
	int err;

	if (lif->hwstamp_txq)
		return 0;

	features = IONIC_Q_F_2X_CQ_DESC | IONIC_TXQ_F_HWSTAMP;

	num_desc = IONIC_MIN_TXRX_DESC;
	desc_sz = sizeof(struct ionic_txq_desc);
	comp_sz = 2 * sizeof(struct ionic_txq_comp);

	if (lif->qtype_info[IONIC_QTYPE_TXQ].version >= 1 &&
	    lif->qtype_info[IONIC_QTYPE_TXQ].sg_desc_sz == sizeof(struct ionic_txq_sg_desc_v1))
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc_v1);
	else
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc);

	txq_i = lif->ionic->ntxqs_per_lif;
	flags = IONIC_QCQ_F_TX_STATS | IONIC_QCQ_F_SG;

	err = ionic_qcq_alloc(lif, IONIC_QTYPE_TXQ, txq_i, "hwstamp_tx", flags,
			      num_desc, desc_sz, comp_sz, sg_desc_sz,
			      sizeof(struct ionic_tx_desc_info),
			      lif->kern_pid, NULL, &txq);
	if (err)
		goto err_qcq_alloc;

	txq->q.features = features;

	ionic_link_qcq_interrupts(lif->adminqcq, txq);
	ionic_debugfs_add_qcq(lif, txq);

	lif->hwstamp_txq = txq;

	if (netif_running(lif->netdev)) {
		err = ionic_lif_txq_init(lif, txq);
		if (err)
			goto err_qcq_init;

		if (test_bit(IONIC_LIF_F_UP, lif->state)) {
			err = ionic_qcq_enable(txq);
			if (err)
				goto err_qcq_enable;
		}
	}

	return 0;

err_qcq_enable:
	ionic_lif_qcq_deinit(lif, txq);
err_qcq_init:
	lif->hwstamp_txq = NULL;
	ionic_debugfs_del_qcq(txq);
	ionic_qcq_free(lif, txq);
	devm_kfree(lif->ionic->dev, txq);
err_qcq_alloc:
	return err;
}

int ionic_lif_create_hwstamp_rxq(struct ionic_lif *lif)
{
	unsigned int num_desc, desc_sz, comp_sz, sg_desc_sz;
	unsigned int rxq_i, flags;
	struct ionic_qcq *rxq;
	u64 features;
	int err;

	if (lif->hwstamp_rxq)
		return 0;

	features = IONIC_Q_F_2X_CQ_DESC | IONIC_RXQ_F_HWSTAMP;

	num_desc = IONIC_MIN_TXRX_DESC;
	desc_sz = sizeof(struct ionic_rxq_desc);
	comp_sz = 2 * sizeof(struct ionic_rxq_comp);
	sg_desc_sz = sizeof(struct ionic_rxq_sg_desc);

	rxq_i = lif->ionic->nrxqs_per_lif;
	flags = IONIC_QCQ_F_RX_STATS | IONIC_QCQ_F_SG;

	err = ionic_qcq_alloc(lif, IONIC_QTYPE_RXQ, rxq_i, "hwstamp_rx", flags,
			      num_desc, desc_sz, comp_sz, sg_desc_sz,
			      sizeof(struct ionic_rx_desc_info),
			      lif->kern_pid, NULL, &rxq);
	if (err)
		goto err_qcq_alloc;

	rxq->q.features = features;

	ionic_link_qcq_interrupts(lif->adminqcq, rxq);
	ionic_debugfs_add_qcq(lif, rxq);

	lif->hwstamp_rxq = rxq;

	if (netif_running(lif->netdev)) {
		err = ionic_lif_rxq_init(lif, rxq);
		if (err)
			goto err_qcq_init;

		if (test_bit(IONIC_LIF_F_UP, lif->state)) {
			ionic_rx_fill(&rxq->q, NULL);
			err = ionic_qcq_enable(rxq);
			if (err)
				goto err_qcq_enable;
		}
	}

	return 0;

err_qcq_enable:
	ionic_lif_qcq_deinit(lif, rxq);
err_qcq_init:
	lif->hwstamp_rxq = NULL;
	ionic_debugfs_del_qcq(rxq);
	ionic_qcq_free(lif, rxq);
	devm_kfree(lif->ionic->dev, rxq);
err_qcq_alloc:
	return err;
}

int ionic_lif_config_hwstamp_rxq_all(struct ionic_lif *lif, bool rx_all)
{
	struct ionic_queue_params qparam;

	ionic_init_queue_params(lif, &qparam);

	if (rx_all)
		qparam.rxq_features = IONIC_Q_F_2X_CQ_DESC | IONIC_RXQ_F_HWSTAMP;
	else
		qparam.rxq_features = 0;

	/* if we're not running, just set the values and return */
	if (!netif_running(lif->netdev)) {
		lif->rxq_features = qparam.rxq_features;
		return 0;
	}

	return ionic_reconfigure_queues(lif, &qparam);
}

int ionic_lif_set_hwstamp_txmode(struct ionic_lif *lif, u16 txstamp_mode)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_TXSTAMP,
			.txstamp_mode = cpu_to_le16(txstamp_mode),
		},
	};

	return ionic_adminq_post_wait(lif, &ctx);
}

static void ionic_lif_del_hwstamp_rxfilt(struct ionic_lif *lif)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.rx_filter_del = {
			.opcode = IONIC_CMD_RX_FILTER_DEL,
			.lif_index = cpu_to_le16(lif->index),
		},
	};
	struct ionic_rx_filter *f;
	u32 filter_id;
	int err;

	spin_lock_bh(&lif->rx_filters.lock);

	f = ionic_rx_filter_rxsteer(lif);
	if (!f) {
		spin_unlock_bh(&lif->rx_filters.lock);
		return;
	}

	filter_id = f->filter_id;
	ionic_rx_filter_free(lif, f);

	spin_unlock_bh(&lif->rx_filters.lock);

	netdev_dbg(lif->netdev, "rx_filter del RXSTEER (id %d)\n", filter_id);

	ctx.cmd.rx_filter_del.filter_id = cpu_to_le32(filter_id);

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err && err != -EEXIST)
		netdev_dbg(lif->netdev, "failed to delete rx_filter RXSTEER (id %d)\n", filter_id);
}

static int ionic_lif_add_hwstamp_rxfilt(struct ionic_lif *lif, u64 pkt_class)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.rx_filter_add = {
			.opcode = IONIC_CMD_RX_FILTER_ADD,
			.lif_index = cpu_to_le16(lif->index),
			.match = cpu_to_le16(IONIC_RX_FILTER_STEER_PKTCLASS),
			.pkt_class = cpu_to_le64(pkt_class),
		},
	};
	u8 qtype;
	u32 qid;
	int err;

	if (!lif->hwstamp_rxq)
		return -EINVAL;

	qtype = lif->hwstamp_rxq->q.type;
	ctx.cmd.rx_filter_add.qtype = qtype;

	qid = lif->hwstamp_rxq->q.index;
	ctx.cmd.rx_filter_add.qid = cpu_to_le32(qid);

	netdev_dbg(lif->netdev, "rx_filter add RXSTEER\n");
	err = ionic_adminq_post_wait(lif, &ctx);
	if (err && err != -EEXIST)
		return err;

	spin_lock_bh(&lif->rx_filters.lock);
	err = ionic_rx_filter_save(lif, 0, qid, 0, &ctx, IONIC_FILTER_STATE_SYNCED);
	spin_unlock_bh(&lif->rx_filters.lock);

	return err;
}

int ionic_lif_set_hwstamp_rxfilt(struct ionic_lif *lif, u64 pkt_class)
{
	ionic_lif_del_hwstamp_rxfilt(lif);

	if (!pkt_class)
		return 0;

	return ionic_lif_add_hwstamp_rxfilt(lif, pkt_class);
}

static int ionic_adminq_napi(struct napi_struct *napi, int budget)
{
	struct ionic_intr_info *intr = napi_to_cq(napi)->bound_intr;
	struct ionic_lif *lif = napi_to_cq(napi)->lif;
	struct ionic_dev *idev = &lif->ionic->idev;
	unsigned long irqflags;
	unsigned int flags = 0;
	int rx_work = 0;
	int tx_work = 0;
	int n_work = 0;
	int a_work = 0;
	int work_done;
	int credits;

	if (lif->notifyqcq && lif->notifyqcq->flags & IONIC_QCQ_F_INITED)
		n_work = ionic_cq_service(&lif->notifyqcq->cq, budget,
					  ionic_notifyq_service, NULL, NULL);

	spin_lock_irqsave(&lif->adminq_lock, irqflags);
	if (lif->adminqcq && lif->adminqcq->flags & IONIC_QCQ_F_INITED)
		a_work = ionic_cq_service(&lif->adminqcq->cq, budget,
					  ionic_adminq_service, NULL, NULL);

	spin_unlock_irqrestore(&lif->adminq_lock, irqflags);

	if (lif->hwstamp_rxq)
		rx_work = ionic_cq_service(&lif->hwstamp_rxq->cq, budget,
					   ionic_rx_service, NULL, NULL);

	if (lif->hwstamp_txq)
		tx_work = ionic_tx_cq_service(&lif->hwstamp_txq->cq, budget, !!budget);

	work_done = max(max(n_work, a_work), max(rx_work, tx_work));
	if (work_done < budget && napi_complete_done(napi, work_done)) {
		flags |= IONIC_INTR_CRED_UNMASK;
		intr->rearm_count++;
	}

	if (work_done || flags) {
		flags |= IONIC_INTR_CRED_RESET_COALESCE;
		credits = n_work + a_work + rx_work + tx_work;
		ionic_intr_credits(idev->intr_ctrl, intr->index, credits, flags);
	}

	if (lif->doorbell_wa) {
		if (!a_work)
			ionic_adminq_poke_doorbell(&lif->adminqcq->q);
		if (lif->hwstamp_rxq && !rx_work)
			ionic_rxq_poke_doorbell(&lif->hwstamp_rxq->q);
		if (lif->hwstamp_txq && !tx_work)
			ionic_txq_poke_doorbell(&lif->hwstamp_txq->q);
	}

	return work_done;
}

void ionic_get_stats64(struct net_device *netdev,
		       struct rtnl_link_stats64 *ns)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_lif_stats *ls;

	memset(ns, 0, sizeof(*ns));
	ls = &lif->info->stats;

	ns->rx_packets = le64_to_cpu(ls->rx_ucast_packets) +
			 le64_to_cpu(ls->rx_mcast_packets) +
			 le64_to_cpu(ls->rx_bcast_packets);

	ns->tx_packets = le64_to_cpu(ls->tx_ucast_packets) +
			 le64_to_cpu(ls->tx_mcast_packets) +
			 le64_to_cpu(ls->tx_bcast_packets);

	ns->rx_bytes = le64_to_cpu(ls->rx_ucast_bytes) +
		       le64_to_cpu(ls->rx_mcast_bytes) +
		       le64_to_cpu(ls->rx_bcast_bytes);

	ns->tx_bytes = le64_to_cpu(ls->tx_ucast_bytes) +
		       le64_to_cpu(ls->tx_mcast_bytes) +
		       le64_to_cpu(ls->tx_bcast_bytes);

	ns->rx_dropped = le64_to_cpu(ls->rx_ucast_drop_packets) +
			 le64_to_cpu(ls->rx_mcast_drop_packets) +
			 le64_to_cpu(ls->rx_bcast_drop_packets);

	ns->tx_dropped = le64_to_cpu(ls->tx_ucast_drop_packets) +
			 le64_to_cpu(ls->tx_mcast_drop_packets) +
			 le64_to_cpu(ls->tx_bcast_drop_packets);

	ns->multicast = le64_to_cpu(ls->rx_mcast_packets);

	ns->rx_over_errors = le64_to_cpu(ls->rx_queue_empty);

	ns->rx_missed_errors = le64_to_cpu(ls->rx_dma_error) +
			       le64_to_cpu(ls->rx_queue_disabled) +
			       le64_to_cpu(ls->rx_desc_fetch_error) +
			       le64_to_cpu(ls->rx_desc_data_error);

	ns->tx_aborted_errors = le64_to_cpu(ls->tx_dma_error) +
				le64_to_cpu(ls->tx_queue_disabled) +
				le64_to_cpu(ls->tx_desc_fetch_error) +
				le64_to_cpu(ls->tx_desc_data_error);

	ns->rx_errors = ns->rx_over_errors +
			ns->rx_missed_errors;

	ns->tx_errors = ns->tx_aborted_errors;
}

static int ionic_addr_add(struct net_device *netdev, const u8 *addr)
{
	return ionic_lif_list_addr(netdev_priv(netdev), addr, ADD_ADDR);
}

static int ionic_addr_del(struct net_device *netdev, const u8 *addr)
{
	/* Don't delete our own address from the uc list */
	if (ether_addr_equal(addr, netdev->dev_addr))
		return 0;

	return ionic_lif_list_addr(netdev_priv(netdev), addr, DEL_ADDR);
}

void ionic_lif_rx_mode(struct ionic_lif *lif)
{
	struct net_device *netdev = lif->netdev;
	unsigned int nfilters;
	unsigned int nd_flags;
	char buf[128];
	u16 rx_mode;
	int i;
#define REMAIN(__x) (sizeof(buf) - (__x))

	mutex_lock(&lif->config_lock);

	/* grab the flags once for local use */
	nd_flags = netdev->flags;

	rx_mode = IONIC_RX_MODE_F_UNICAST;
	rx_mode |= (nd_flags & IFF_MULTICAST) ? IONIC_RX_MODE_F_MULTICAST : 0;
	rx_mode |= (nd_flags & IFF_BROADCAST) ? IONIC_RX_MODE_F_BROADCAST : 0;
	rx_mode |= (nd_flags & IFF_PROMISC) ? IONIC_RX_MODE_F_PROMISC : 0;
	rx_mode |= (nd_flags & IFF_ALLMULTI) ? IONIC_RX_MODE_F_ALLMULTI : 0;

	/* sync the filters */
	ionic_rx_filter_sync(lif);

	/* check for overflow state
	 *    if so, we track that we overflowed and enable NIC PROMISC
	 *    else if the overflow is set and not needed
	 *       we remove our overflow flag and check the netdev flags
	 *       to see if we can disable NIC PROMISC
	 */
	nfilters = le32_to_cpu(lif->identity->eth.max_ucast_filters);

	if (((lif->nucast + lif->nmcast) >= nfilters) ||
	    (lif->max_vlans && lif->nvlans >= lif->max_vlans)) {
		rx_mode |= IONIC_RX_MODE_F_PROMISC;
		rx_mode |= IONIC_RX_MODE_F_ALLMULTI;
	} else {
		if (!(nd_flags & IFF_PROMISC))
			rx_mode &= ~IONIC_RX_MODE_F_PROMISC;
		if (!(nd_flags & IFF_ALLMULTI))
			rx_mode &= ~IONIC_RX_MODE_F_ALLMULTI;
	}

	i = scnprintf(buf, sizeof(buf), "rx_mode 0x%04x -> 0x%04x:",
		      lif->rx_mode, rx_mode);
	if (rx_mode & IONIC_RX_MODE_F_UNICAST)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_UNICAST");
	if (rx_mode & IONIC_RX_MODE_F_MULTICAST)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_MULTICAST");
	if (rx_mode & IONIC_RX_MODE_F_BROADCAST)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_BROADCAST");
	if (rx_mode & IONIC_RX_MODE_F_PROMISC)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_PROMISC");
	if (rx_mode & IONIC_RX_MODE_F_ALLMULTI)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_ALLMULTI");
	if (rx_mode & IONIC_RX_MODE_F_RDMA_SNIFFER)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_RDMA_SNIFFER");
	netdev_dbg(netdev, "lif%d %s\n", lif->index, buf);

	if (lif->rx_mode != rx_mode) {
		struct ionic_admin_ctx ctx = {
			.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
			.cmd.rx_mode_set = {
				.opcode = IONIC_CMD_RX_MODE_SET,
				.lif_index = cpu_to_le16(lif->index),
			},
		};
		int err;

		ctx.cmd.rx_mode_set.rx_mode = cpu_to_le16(rx_mode);
		err = ionic_adminq_post_wait(lif, &ctx);
		if (err)
			netdev_warn(netdev, "set rx_mode 0x%04x failed: %d\n",
				    rx_mode, err);
		else
			lif->rx_mode = rx_mode;
	}

	mutex_unlock(&lif->config_lock);
}

static void ionic_ndo_set_rx_mode(struct net_device *netdev)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_deferred_work *work;

	/* Sync the kernel filter list with the driver filter list */
	__dev_uc_sync(netdev, ionic_addr_add, ionic_addr_del);
	__dev_mc_sync(netdev, ionic_addr_add, ionic_addr_del);

	/* Shove off the rest of the rxmode work to the work task
	 * which will include syncing the filters to the firmware.
	 */
	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work) {
		netdev_err(lif->netdev, "rxmode change dropped\n");
		return;
	}
	work->type = IONIC_DW_TYPE_RX_MODE;
	netdev_dbg(lif->netdev, "deferred: rx_mode\n");
	ionic_lif_deferred_enqueue(lif, work);
}

static __le64 ionic_netdev_features_to_nic(netdev_features_t features)
{
	u64 wanted = 0;

	if (features & NETIF_F_HW_VLAN_CTAG_TX)
		wanted |= IONIC_ETH_HW_VLAN_TX_TAG;
	if (features & NETIF_F_HW_VLAN_CTAG_RX)
		wanted |= IONIC_ETH_HW_VLAN_RX_STRIP;
	if (features & NETIF_F_HW_VLAN_CTAG_FILTER)
		wanted |= IONIC_ETH_HW_VLAN_RX_FILTER;
	if (features & NETIF_F_RXHASH)
		wanted |= IONIC_ETH_HW_RX_HASH;
	if (features & NETIF_F_RXCSUM)
		wanted |= IONIC_ETH_HW_RX_CSUM;
	if (features & NETIF_F_SG)
		wanted |= IONIC_ETH_HW_TX_SG;
	if (features & NETIF_F_HW_CSUM)
		wanted |= IONIC_ETH_HW_TX_CSUM;
	if (features & NETIF_F_TSO)
		wanted |= IONIC_ETH_HW_TSO;
	if (features & NETIF_F_TSO6)
		wanted |= IONIC_ETH_HW_TSO_IPV6;
	if (features & NETIF_F_TSO_ECN)
		wanted |= IONIC_ETH_HW_TSO_ECN;
	if (features & NETIF_F_GSO_GRE)
		wanted |= IONIC_ETH_HW_TSO_GRE;
	if (features & NETIF_F_GSO_GRE_CSUM)
		wanted |= IONIC_ETH_HW_TSO_GRE_CSUM;
	if (features & NETIF_F_GSO_IPXIP4)
		wanted |= IONIC_ETH_HW_TSO_IPXIP4;
	if (features & NETIF_F_GSO_IPXIP6)
		wanted |= IONIC_ETH_HW_TSO_IPXIP6;
	if (features & NETIF_F_GSO_UDP_TUNNEL)
		wanted |= IONIC_ETH_HW_TSO_UDP;
	if (features & NETIF_F_GSO_UDP_TUNNEL_CSUM)
		wanted |= IONIC_ETH_HW_TSO_UDP_CSUM;

	return cpu_to_le64(wanted);
}

static int ionic_set_nic_features(struct ionic_lif *lif,
				  netdev_features_t features)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_FEATURES,
		},
	};
	u64 vlan_flags = IONIC_ETH_HW_VLAN_TX_TAG |
			 IONIC_ETH_HW_VLAN_RX_STRIP |
			 IONIC_ETH_HW_VLAN_RX_FILTER;
	u64 old_hw_features;
	int err;

	ctx.cmd.lif_setattr.features = ionic_netdev_features_to_nic(features);

	if (lif->phc)
		ctx.cmd.lif_setattr.features |= cpu_to_le64(IONIC_ETH_HW_TIMESTAMP);

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	old_hw_features = lif->hw_features;
	lif->hw_features = le64_to_cpu(ctx.cmd.lif_setattr.features &
				       ctx.comp.lif_setattr.features);

	if ((old_hw_features ^ lif->hw_features) & IONIC_ETH_HW_RX_HASH)
		ionic_lif_rss_config(lif, lif->rss_types, NULL, NULL);

	if ((vlan_flags & le64_to_cpu(ctx.cmd.lif_setattr.features)) &&
	    !(vlan_flags & le64_to_cpu(ctx.comp.lif_setattr.features)))
		dev_info_once(lif->ionic->dev, "NIC is not supporting vlan offload, likely in SmartNIC mode\n");

	if (lif->hw_features & IONIC_ETH_HW_VLAN_TX_TAG)
		dev_dbg(dev, "feature ETH_HW_VLAN_TX_TAG\n");
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_STRIP)
		dev_dbg(dev, "feature ETH_HW_VLAN_RX_STRIP\n");
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_FILTER)
		dev_dbg(dev, "feature ETH_HW_VLAN_RX_FILTER\n");
	if (lif->hw_features & IONIC_ETH_HW_RX_HASH)
		dev_dbg(dev, "feature ETH_HW_RX_HASH\n");
	if (lif->hw_features & IONIC_ETH_HW_TX_SG)
		dev_dbg(dev, "feature ETH_HW_TX_SG\n");
	if (lif->hw_features & IONIC_ETH_HW_TX_CSUM)
		dev_dbg(dev, "feature ETH_HW_TX_CSUM\n");
	if (lif->hw_features & IONIC_ETH_HW_RX_CSUM)
		dev_dbg(dev, "feature ETH_HW_RX_CSUM\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO)
		dev_dbg(dev, "feature ETH_HW_TSO\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPV6)
		dev_dbg(dev, "feature ETH_HW_TSO_IPV6\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_ECN)
		dev_dbg(dev, "feature ETH_HW_TSO_ECN\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_GRE)
		dev_dbg(dev, "feature ETH_HW_TSO_GRE\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_GRE_CSUM)
		dev_dbg(dev, "feature ETH_HW_TSO_GRE_CSUM\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPXIP4)
		dev_dbg(dev, "feature ETH_HW_TSO_IPXIP4\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPXIP6)
		dev_dbg(dev, "feature ETH_HW_TSO_IPXIP6\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_UDP)
		dev_dbg(dev, "feature ETH_HW_TSO_UDP\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_UDP_CSUM)
		dev_dbg(dev, "feature ETH_HW_TSO_UDP_CSUM\n");
	if (lif->hw_features & IONIC_ETH_HW_TIMESTAMP)
		dev_dbg(dev, "feature ETH_HW_TIMESTAMP\n");

	return 0;
}

static int ionic_init_nic_features(struct ionic_lif *lif)
{
	struct net_device *netdev = lif->netdev;
	netdev_features_t features;
	int err;

	/* set up what we expect to support by default */
	features = NETIF_F_HW_VLAN_CTAG_TX |
		   NETIF_F_HW_VLAN_CTAG_RX |
		   NETIF_F_HW_VLAN_CTAG_FILTER |
		   NETIF_F_SG |
		   NETIF_F_HW_CSUM |
		   NETIF_F_RXCSUM |
		   NETIF_F_TSO |
		   NETIF_F_TSO6 |
		   NETIF_F_TSO_ECN |
		   NETIF_F_GSO_GRE |
		   NETIF_F_GSO_GRE_CSUM |
		   NETIF_F_GSO_IPXIP4 |
		   NETIF_F_GSO_IPXIP6 |
		   NETIF_F_GSO_UDP_TUNNEL |
		   NETIF_F_GSO_UDP_TUNNEL_CSUM;

	if (lif->nxqs > 1)
		features |= NETIF_F_RXHASH;

	err = ionic_set_nic_features(lif, features);
	if (err)
		return err;

	/* tell the netdev what we actually can support */
	netdev->features |= NETIF_F_HIGHDMA;

	if (lif->hw_features & IONIC_ETH_HW_VLAN_TX_TAG)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_TX;
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_STRIP)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_RX;
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_FILTER)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_FILTER;
	if (lif->hw_features & IONIC_ETH_HW_RX_HASH)
		netdev->hw_features |= NETIF_F_RXHASH;
	if (lif->hw_features & IONIC_ETH_HW_TX_SG)
		netdev->hw_features |= NETIF_F_SG;

	if (lif->hw_features & IONIC_ETH_HW_TX_CSUM)
		netdev->hw_enc_features |= NETIF_F_HW_CSUM;
	if (lif->hw_features & IONIC_ETH_HW_RX_CSUM)
		netdev->hw_enc_features |= NETIF_F_RXCSUM;
	if (lif->hw_features & IONIC_ETH_HW_TSO)
		netdev->hw_enc_features |= NETIF_F_TSO;
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPV6)
		netdev->hw_enc_features |= NETIF_F_TSO6;
	if (lif->hw_features & IONIC_ETH_HW_TSO_ECN)
		netdev->hw_enc_features |= NETIF_F_TSO_ECN;
	if (lif->hw_features & IONIC_ETH_HW_TSO_GRE)
		netdev->hw_enc_features |= NETIF_F_GSO_GRE;
	if (lif->hw_features & IONIC_ETH_HW_TSO_GRE_CSUM)
		netdev->hw_enc_features |= NETIF_F_GSO_GRE_CSUM;
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPXIP4)
		netdev->hw_enc_features |= NETIF_F_GSO_IPXIP4;
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPXIP6)
		netdev->hw_enc_features |= NETIF_F_GSO_IPXIP6;
	if (lif->hw_features & IONIC_ETH_HW_TSO_UDP)
		netdev->hw_enc_features |= NETIF_F_GSO_UDP_TUNNEL;
	if (lif->hw_features & IONIC_ETH_HW_TSO_UDP_CSUM)
		netdev->hw_enc_features |= NETIF_F_GSO_UDP_TUNNEL_CSUM;

	netdev->hw_features |= netdev->hw_enc_features;
	netdev->features |= netdev->hw_features;
	netdev->vlan_features |= netdev->features & ~NETIF_F_VLAN_FEATURES;

	netdev->priv_flags |= IFF_UNICAST_FLT |
			      IFF_LIVE_ADDR_CHANGE;

	netdev->xdp_features = NETDEV_XDP_ACT_BASIC    |
			       NETDEV_XDP_ACT_REDIRECT |
			       NETDEV_XDP_ACT_RX_SG    |
			       NETDEV_XDP_ACT_NDO_XMIT |
			       NETDEV_XDP_ACT_NDO_XMIT_SG;

	return 0;
}

static int ionic_set_features(struct net_device *netdev,
			      netdev_features_t features)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	int err;

	netdev_dbg(netdev, "%s: lif->features=0x%08llx new_features=0x%08llx\n",
		   __func__, (u64)lif->netdev->features, (u64)features);

	err = ionic_set_nic_features(lif, features);

	return err;
}

static int ionic_set_attr_mac(struct ionic_lif *lif, u8 *mac)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_MAC,
		},
	};

	ether_addr_copy(ctx.cmd.lif_setattr.mac, mac);
	return ionic_adminq_post_wait(lif, &ctx);
}

static int ionic_get_attr_mac(struct ionic_lif *lif, u8 *mac_addr)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_getattr = {
			.opcode = IONIC_CMD_LIF_GETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_MAC,
		},
	};
	int err;

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	ether_addr_copy(mac_addr, ctx.comp.lif_getattr.mac);
	return 0;
}

static int ionic_program_mac(struct ionic_lif *lif, u8 *mac)
{
	u8  get_mac[ETH_ALEN];
	int err;

	err = ionic_set_attr_mac(lif, mac);
	if (err)
		return err;

	err = ionic_get_attr_mac(lif, get_mac);
	if (err)
		return err;

	/* To deal with older firmware that silently ignores the set attr mac:
	 * doesn't actually change the mac and doesn't return an error, so we
	 * do the get attr to verify whether or not the set actually happened
	 */
	if (!ether_addr_equal(get_mac, mac))
		return 1;

	return 0;
}

static int ionic_set_mac_address(struct net_device *netdev, void *sa)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct sockaddr *addr = sa;
	u8 *mac;
	int err;

	mac = (u8 *)addr->sa_data;
	if (ether_addr_equal(netdev->dev_addr, mac))
		return 0;

	err = ionic_program_mac(lif, mac);
	if (err < 0)
		return err;

	if (err > 0)
		netdev_dbg(netdev, "%s: SET and GET ATTR Mac are not equal-due to old FW running\n",
			   __func__);

	err = eth_prepare_mac_addr_change(netdev, addr);
	if (err)
		return err;

	if (!is_zero_ether_addr(netdev->dev_addr)) {
		netdev_info(netdev, "deleting mac addr %pM\n",
			    netdev->dev_addr);
		ionic_lif_addr_del(netdev_priv(netdev), netdev->dev_addr);
	}

	eth_commit_mac_addr_change(netdev, addr);
	netdev_info(netdev, "updating mac addr %pM\n", mac);

	return ionic_lif_addr_add(netdev_priv(netdev), mac);
}

void ionic_stop_queues_reconfig(struct ionic_lif *lif)
{
	/* Stop and clean the queues before reconfiguration */
	netif_device_detach(lif->netdev);
	ionic_stop_queues(lif);
	ionic_txrx_deinit(lif);
}

static int ionic_start_queues_reconfig(struct ionic_lif *lif)
{
	int err;

	/* Re-init the queues after reconfiguration */

	/* The only way txrx_init can fail here is if communication
	 * with FW is suddenly broken.  There's not much we can do
	 * at this point - error messages have already been printed,
	 * so we can continue on and the user can eventually do a
	 * DOWN and UP to try to reset and clear the issue.
	 */
	err = ionic_txrx_init(lif);
	ionic_link_status_check_request(lif, CAN_NOT_SLEEP);
	netif_device_attach(lif->netdev);

	return err;
}

static bool ionic_xdp_is_valid_mtu(struct ionic_lif *lif, u32 mtu,
				   struct bpf_prog *xdp_prog)
{
	if (!xdp_prog)
		return true;

	if (mtu <= IONIC_XDP_MAX_LINEAR_MTU)
		return true;

	if (xdp_prog->aux && xdp_prog->aux->xdp_has_frags)
		return true;

	return false;
}

static int ionic_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_MTU,
			.mtu = cpu_to_le32(new_mtu),
		},
	};
	struct bpf_prog *xdp_prog;
	int err;

	xdp_prog = READ_ONCE(lif->xdp_prog);
	if (!ionic_xdp_is_valid_mtu(lif, new_mtu, xdp_prog))
		return -EINVAL;

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	/* if we're not running, nothing more to do */
	if (!netif_running(netdev)) {
		WRITE_ONCE(netdev->mtu, new_mtu);
		return 0;
	}

	mutex_lock(&lif->queue_lock);
	ionic_stop_queues_reconfig(lif);
	WRITE_ONCE(netdev->mtu, new_mtu);
	err = ionic_start_queues_reconfig(lif);
	mutex_unlock(&lif->queue_lock);

	return err;
}

static void ionic_tx_timeout_work(struct work_struct *ws)
{
	struct ionic_lif *lif = container_of(ws, struct ionic_lif, tx_timeout_work);
	int err;

	if (test_bit(IONIC_LIF_F_FW_RESET, lif->state))
		return;

	/* if we were stopped before this scheduled job was launched,
	 * don't bother the queues as they are already stopped.
	 */
	if (!netif_running(lif->netdev))
		return;

	mutex_lock(&lif->queue_lock);
	ionic_stop_queues_reconfig(lif);
	err = ionic_start_queues_reconfig(lif);
	mutex_unlock(&lif->queue_lock);

	if (err)
		dev_err(lif->ionic->dev, "%s: Restarting queues failed\n", __func__);
}

static void ionic_tx_timeout(struct net_device *netdev, unsigned int txqueue)
{
	struct ionic_lif *lif = netdev_priv(netdev);

	netdev_info(lif->netdev, "Tx Timeout triggered - txq %d\n", txqueue);
	schedule_work(&lif->tx_timeout_work);
}

static int ionic_vlan_rx_add_vid(struct net_device *netdev, __be16 proto,
				 u16 vid)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	int err;

	err = ionic_lif_vlan_add(lif, vid);
	if (err)
		return err;

	ionic_lif_rx_mode(lif);

	return 0;
}

static int ionic_vlan_rx_kill_vid(struct net_device *netdev, __be16 proto,
				  u16 vid)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	int err;

	err = ionic_lif_vlan_del(lif, vid);
	if (err)
		return err;

	ionic_lif_rx_mode(lif);

	return 0;
}

int ionic_lif_rss_config(struct ionic_lif *lif, const u16 types,
			 const u8 *key, const u32 *indir)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.attr = IONIC_LIF_ATTR_RSS,
			.rss.addr = cpu_to_le64(lif->rss_ind_tbl_pa),
		},
	};
	unsigned int i, tbl_sz;

	if (lif->hw_features & IONIC_ETH_HW_RX_HASH) {
		lif->rss_types = types;
		ctx.cmd.lif_setattr.rss.types = cpu_to_le16(types);
	}

	if (key)
		memcpy(lif->rss_hash_key, key, IONIC_RSS_HASH_KEY_SIZE);

	if (indir) {
		tbl_sz = le16_to_cpu(lif->ionic->ident.lif.eth.rss_ind_tbl_sz);
		for (i = 0; i < tbl_sz; i++)
			lif->rss_ind_tbl[i] = indir[i];
	}

	memcpy(ctx.cmd.lif_setattr.rss.key, lif->rss_hash_key,
	       IONIC_RSS_HASH_KEY_SIZE);

	return ionic_adminq_post_wait(lif, &ctx);
}

static int ionic_lif_rss_init(struct ionic_lif *lif)
{
	unsigned int tbl_sz;
	unsigned int i;

	lif->rss_types = IONIC_RSS_TYPE_IPV4     |
			 IONIC_RSS_TYPE_IPV4_TCP |
			 IONIC_RSS_TYPE_IPV4_UDP |
			 IONIC_RSS_TYPE_IPV6     |
			 IONIC_RSS_TYPE_IPV6_TCP |
			 IONIC_RSS_TYPE_IPV6_UDP;

	/* Fill indirection table with 'default' values */
	tbl_sz = le16_to_cpu(lif->ionic->ident.lif.eth.rss_ind_tbl_sz);
	for (i = 0; i < tbl_sz; i++)
		lif->rss_ind_tbl[i] = ethtool_rxfh_indir_default(i, lif->nxqs);

	return ionic_lif_rss_config(lif, lif->rss_types, NULL, NULL);
}

static void ionic_lif_rss_deinit(struct ionic_lif *lif)
{
	int tbl_sz;

	tbl_sz = le16_to_cpu(lif->ionic->ident.lif.eth.rss_ind_tbl_sz);
	memset(lif->rss_ind_tbl, 0, tbl_sz);
	memset(lif->rss_hash_key, 0, IONIC_RSS_HASH_KEY_SIZE);

	ionic_lif_rss_config(lif, 0x0, NULL, NULL);
}

static void ionic_lif_quiesce(struct ionic_lif *lif)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_STATE,
			.state = IONIC_LIF_QUIESCE,
		},
	};
	int err;

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		netdev_dbg(lif->netdev, "lif quiesce failed %d\n", err);
}

static void ionic_txrx_disable(struct ionic_lif *lif)
{
	unsigned int i;
	int err = 0;

	if (lif->txqcqs) {
		for (i = 0; i < lif->nxqs; i++)
			err = ionic_qcq_disable(lif, lif->txqcqs[i], err);
	}

	if (lif->hwstamp_txq)
		err = ionic_qcq_disable(lif, lif->hwstamp_txq, err);

	if (lif->rxqcqs) {
		for (i = 0; i < lif->nxqs; i++)
			err = ionic_qcq_disable(lif, lif->rxqcqs[i], err);
	}

	if (lif->hwstamp_rxq)
		err = ionic_qcq_disable(lif, lif->hwstamp_rxq, err);

	ionic_lif_quiesce(lif);
}

static void ionic_txrx_deinit(struct ionic_lif *lif)
{
	unsigned int i;

	if (lif->txqcqs) {
		for (i = 0; i < lif->nxqs && lif->txqcqs[i]; i++) {
			ionic_lif_qcq_deinit(lif, lif->txqcqs[i]);
			ionic_tx_flush(&lif->txqcqs[i]->cq);
			ionic_tx_empty(&lif->txqcqs[i]->q);
		}
	}

	if (lif->rxqcqs) {
		for (i = 0; i < lif->nxqs && lif->rxqcqs[i]; i++) {
			ionic_lif_qcq_deinit(lif, lif->rxqcqs[i]);
			ionic_rx_empty(&lif->rxqcqs[i]->q);
		}
	}
	lif->rx_mode = 0;

	if (lif->hwstamp_txq) {
		ionic_lif_qcq_deinit(lif, lif->hwstamp_txq);
		ionic_tx_flush(&lif->hwstamp_txq->cq);
		ionic_tx_empty(&lif->hwstamp_txq->q);
	}

	if (lif->hwstamp_rxq) {
		ionic_lif_qcq_deinit(lif, lif->hwstamp_rxq);
		ionic_rx_empty(&lif->hwstamp_rxq->q);
	}
}

void ionic_txrx_free(struct ionic_lif *lif)
{
	unsigned int i;

	if (lif->txqcqs) {
		for (i = 0; i < lif->ionic->ntxqs_per_lif && lif->txqcqs[i]; i++) {
			ionic_qcq_free(lif, lif->txqcqs[i]);
			devm_kfree(lif->ionic->dev, lif->txqcqs[i]);
			lif->txqcqs[i] = NULL;
		}
	}

	if (lif->rxqcqs) {
		for (i = 0; i < lif->ionic->nrxqs_per_lif && lif->rxqcqs[i]; i++) {
			ionic_qcq_free(lif, lif->rxqcqs[i]);
			devm_kfree(lif->ionic->dev, lif->rxqcqs[i]);
			lif->rxqcqs[i] = NULL;
		}
	}

	if (lif->hwstamp_txq) {
		ionic_qcq_free(lif, lif->hwstamp_txq);
		devm_kfree(lif->ionic->dev, lif->hwstamp_txq);
		lif->hwstamp_txq = NULL;
	}

	if (lif->hwstamp_rxq) {
		ionic_qcq_free(lif, lif->hwstamp_rxq);
		devm_kfree(lif->ionic->dev, lif->hwstamp_rxq);
		lif->hwstamp_rxq = NULL;
	}
}

static int ionic_txrx_alloc(struct ionic_lif *lif)
{
	unsigned int comp_sz, desc_sz, num_desc, sg_desc_sz;
	unsigned int flags, i;
	int err = 0;

	num_desc = lif->ntxq_descs;
	desc_sz = sizeof(struct ionic_txq_desc);
	comp_sz = sizeof(struct ionic_txq_comp);

	if (lif->qtype_info[IONIC_QTYPE_TXQ].version >= 1 &&
	    lif->qtype_info[IONIC_QTYPE_TXQ].sg_desc_sz ==
					  sizeof(struct ionic_txq_sg_desc_v1))
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc_v1);
	else
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc);

	flags = IONIC_QCQ_F_TX_STATS | IONIC_QCQ_F_SG;

	if (test_bit(IONIC_LIF_F_CMB_TX_RINGS, lif->state))
		flags |= IONIC_QCQ_F_CMB_RINGS;

	if (test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state))
		flags |= IONIC_QCQ_F_INTR;

	for (i = 0; i < lif->nxqs; i++) {
		err = ionic_qcq_alloc(lif, IONIC_QTYPE_TXQ, i, "tx", flags,
				      num_desc, desc_sz, comp_sz, sg_desc_sz,
				      sizeof(struct ionic_tx_desc_info),
				      lif->kern_pid, NULL, &lif->txqcqs[i]);
		if (err)
			goto err_out;

		if (flags & IONIC_QCQ_F_INTR) {
			ionic_intr_coal_init(lif->ionic->idev.intr_ctrl,
					     lif->txqcqs[i]->intr.index,
					     lif->tx_coalesce_hw);
			if (test_bit(IONIC_LIF_F_TX_DIM_INTR, lif->state))
				lif->txqcqs[i]->intr.dim_coal_hw = lif->tx_coalesce_hw;
		}

		ionic_debugfs_add_qcq(lif, lif->txqcqs[i]);
	}

	flags = IONIC_QCQ_F_RX_STATS | IONIC_QCQ_F_SG | IONIC_QCQ_F_INTR;

	if (test_bit(IONIC_LIF_F_CMB_RX_RINGS, lif->state))
		flags |= IONIC_QCQ_F_CMB_RINGS;

	num_desc = lif->nrxq_descs;
	desc_sz = sizeof(struct ionic_rxq_desc);
	comp_sz = sizeof(struct ionic_rxq_comp);
	sg_desc_sz = sizeof(struct ionic_rxq_sg_desc);

	if (lif->rxq_features & IONIC_Q_F_2X_CQ_DESC)
		comp_sz *= 2;

	for (i = 0; i < lif->nxqs; i++) {
		err = ionic_qcq_alloc(lif, IONIC_QTYPE_RXQ, i, "rx", flags,
				      num_desc, desc_sz, comp_sz, sg_desc_sz,
				      sizeof(struct ionic_rx_desc_info),
				      lif->kern_pid, lif->xdp_prog,
				      &lif->rxqcqs[i]);
		if (err)
			goto err_out;

		lif->rxqcqs[i]->q.features = lif->rxq_features;

		ionic_intr_coal_init(lif->ionic->idev.intr_ctrl,
				     lif->rxqcqs[i]->intr.index,
				     lif->rx_coalesce_hw);
		if (test_bit(IONIC_LIF_F_RX_DIM_INTR, lif->state))
			lif->rxqcqs[i]->intr.dim_coal_hw = lif->rx_coalesce_hw;

		if (!test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state))
			ionic_link_qcq_interrupts(lif->rxqcqs[i],
						  lif->txqcqs[i]);

		ionic_debugfs_add_qcq(lif, lif->rxqcqs[i]);
	}

	return 0;

err_out:
	ionic_txrx_free(lif);

	return err;
}

static int ionic_txrx_init(struct ionic_lif *lif)
{
	unsigned int i;
	int err;

	for (i = 0; i < lif->nxqs; i++) {
		err = ionic_lif_txq_init(lif, lif->txqcqs[i]);
		if (err)
			goto err_out;

		err = ionic_lif_rxq_init(lif, lif->rxqcqs[i]);
		if (err) {
			ionic_lif_qcq_deinit(lif, lif->txqcqs[i]);
			goto err_out;
		}
	}

	if (lif->netdev->features & NETIF_F_RXHASH)
		ionic_lif_rss_init(lif);

	ionic_lif_rx_mode(lif);

	return 0;

err_out:
	while (i--) {
		ionic_lif_qcq_deinit(lif, lif->txqcqs[i]);
		ionic_lif_qcq_deinit(lif, lif->rxqcqs[i]);
	}

	return err;
}

static int ionic_txrx_enable(struct ionic_lif *lif)
{
	int derr = 0;
	int i, err;

	ionic_xdp_rxqs_prog_update(lif);

	for (i = 0; i < lif->nxqs; i++) {
		if (!(lif->rxqcqs[i] && lif->txqcqs[i])) {
			dev_err(lif->ionic->dev, "%s: bad qcq %d\n", __func__, i);
			err = -ENXIO;
			goto err_out;
		}

		ionic_rx_fill(&lif->rxqcqs[i]->q,
			      READ_ONCE(lif->rxqcqs[i]->q.xdp_prog));
		err = ionic_qcq_enable(lif->rxqcqs[i]);
		if (err)
			goto err_out;

		err = ionic_qcq_enable(lif->txqcqs[i]);
		if (err) {
			derr = ionic_qcq_disable(lif, lif->rxqcqs[i], err);
			goto err_out;
		}
	}

	if (lif->hwstamp_rxq) {
		ionic_rx_fill(&lif->hwstamp_rxq->q, NULL);
		err = ionic_qcq_enable(lif->hwstamp_rxq);
		if (err)
			goto err_out_hwstamp_rx;
	}

	if (lif->hwstamp_txq) {
		err = ionic_qcq_enable(lif->hwstamp_txq);
		if (err)
			goto err_out_hwstamp_tx;
	}

	return 0;

err_out_hwstamp_tx:
	if (lif->hwstamp_rxq)
		derr = ionic_qcq_disable(lif, lif->hwstamp_rxq, derr);
err_out_hwstamp_rx:
	i = lif->nxqs;
err_out:
	while (i--) {
		derr = ionic_qcq_disable(lif, lif->txqcqs[i], derr);
		derr = ionic_qcq_disable(lif, lif->rxqcqs[i], derr);
	}

	ionic_xdp_rxqs_prog_update(lif);

	return err;
}

static int ionic_start_queues(struct ionic_lif *lif)
{
	int err;

	if (test_bit(IONIC_LIF_F_BROKEN, lif->state))
		return -EIO;

	if (test_bit(IONIC_LIF_F_FW_RESET, lif->state))
		return -EBUSY;

	if (test_and_set_bit(IONIC_LIF_F_UP, lif->state))
		return 0;

	err = ionic_txrx_enable(lif);
	if (err) {
		clear_bit(IONIC_LIF_F_UP, lif->state);
		return err;
	}
	netif_tx_wake_all_queues(lif->netdev);

	return 0;
}

static int ionic_open(struct net_device *netdev)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	int err;

	/* If recovering from a broken state, clear the bit and we'll try again */
	if (test_and_clear_bit(IONIC_LIF_F_BROKEN, lif->state))
		netdev_info(netdev, "clearing broken state\n");

	mutex_lock(&lif->queue_lock);

	err = ionic_txrx_alloc(lif);
	if (err)
		goto err_unlock;

	err = ionic_txrx_init(lif);
	if (err)
		goto err_txrx_free;

	err = netif_set_real_num_tx_queues(netdev, lif->nxqs);
	if (err)
		goto err_txrx_deinit;

	err = netif_set_real_num_rx_queues(netdev, lif->nxqs);
	if (err)
		goto err_txrx_deinit;

	/* don't start the queues until we have link */
	if (netif_carrier_ok(netdev)) {
		err = ionic_start_queues(lif);
		if (err)
			goto err_txrx_deinit;
	}

	/* If hardware timestamping is enabled, but the queues were freed by
	 * ionic_stop, those need to be reallocated and initialized, too.
	 */
	ionic_lif_hwstamp_recreate_queues(lif);

	mutex_unlock(&lif->queue_lock);

	return 0;

err_txrx_deinit:
	ionic_txrx_deinit(lif);
err_txrx_free:
	ionic_txrx_free(lif);
err_unlock:
	mutex_unlock(&lif->queue_lock);
	return err;
}

static void ionic_stop_queues(struct ionic_lif *lif)
{
	if (!test_and_clear_bit(IONIC_LIF_F_UP, lif->state))
		return;

	netif_tx_disable(lif->netdev);
	ionic_txrx_disable(lif);
}

static int ionic_stop(struct net_device *netdev)
{
	struct ionic_lif *lif = netdev_priv(netdev);

	if (test_bit(IONIC_LIF_F_FW_RESET, lif->state))
		return 0;

	mutex_lock(&lif->queue_lock);
	ionic_stop_queues(lif);
	ionic_txrx_deinit(lif);
	ionic_txrx_free(lif);
	mutex_unlock(&lif->queue_lock);

	return 0;
}

static int ionic_eth_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	struct ionic_lif *lif = netdev_priv(netdev);

	switch (cmd) {
	case SIOCSHWTSTAMP:
		return ionic_lif_hwstamp_set(lif, ifr);
	case SIOCGHWTSTAMP:
		return ionic_lif_hwstamp_get(lif, ifr);
	default:
		return -EOPNOTSUPP;
	}
}

static int ionic_get_vf_config(struct net_device *netdev,
			       int vf, struct ifla_vf_info *ivf)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret = 0;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_read(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		struct ionic_vf *vfdata = &ionic->vfs[vf];

		ivf->vf		  = vf;
		ivf->qos	  = 0;
		ivf->vlan         = le16_to_cpu(vfdata->vlanid);
		ivf->spoofchk     = vfdata->spoofchk;
		ivf->linkstate    = vfdata->linkstate;
		ivf->max_tx_rate  = le32_to_cpu(vfdata->maxrate);
		ivf->trusted      = vfdata->trusted;
		ether_addr_copy(ivf->mac, vfdata->macaddr);
	}

	up_read(&ionic->vf_op_lock);
	return ret;
}

static int ionic_get_vf_stats(struct net_device *netdev, int vf,
			      struct ifla_vf_stats *vf_stats)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	struct ionic_lif_stats *vs;
	int ret = 0;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_read(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		memset(vf_stats, 0, sizeof(*vf_stats));
		vs = &ionic->vfs[vf].stats;

		vf_stats->rx_packets = le64_to_cpu(vs->rx_ucast_packets);
		vf_stats->tx_packets = le64_to_cpu(vs->tx_ucast_packets);
		vf_stats->rx_bytes   = le64_to_cpu(vs->rx_ucast_bytes);
		vf_stats->tx_bytes   = le64_to_cpu(vs->tx_ucast_bytes);
		vf_stats->broadcast  = le64_to_cpu(vs->rx_bcast_packets);
		vf_stats->multicast  = le64_to_cpu(vs->rx_mcast_packets);
		vf_stats->rx_dropped = le64_to_cpu(vs->rx_ucast_drop_packets) +
				       le64_to_cpu(vs->rx_mcast_drop_packets) +
				       le64_to_cpu(vs->rx_bcast_drop_packets);
		vf_stats->tx_dropped = le64_to_cpu(vs->tx_ucast_drop_packets) +
				       le64_to_cpu(vs->tx_mcast_drop_packets) +
				       le64_to_cpu(vs->tx_bcast_drop_packets);
	}

	up_read(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_mac(struct net_device *netdev, int vf, u8 *mac)
{
	struct ionic_vf_setattr_cmd vfc = { .attr = IONIC_VF_ATTR_MAC };
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret;

	if (!(is_zero_ether_addr(mac) || is_valid_ether_addr(mac)))
		return -EINVAL;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		ether_addr_copy(vfc.macaddr, mac);
		dev_dbg(ionic->dev, "%s: vf %d macaddr %pM\n",
			__func__, vf, vfc.macaddr);

		ret = ionic_set_vf_config(ionic, vf, &vfc);
		if (!ret)
			ether_addr_copy(ionic->vfs[vf].macaddr, mac);
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_vlan(struct net_device *netdev, int vf, u16 vlan,
			     u8 qos, __be16 proto)
{
	struct ionic_vf_setattr_cmd vfc = { .attr = IONIC_VF_ATTR_VLAN };
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret;

	/* until someday when we support qos */
	if (qos)
		return -EINVAL;

	if (vlan > 4095)
		return -EINVAL;

	if (proto != htons(ETH_P_8021Q))
		return -EPROTONOSUPPORT;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		vfc.vlanid = cpu_to_le16(vlan);
		dev_dbg(ionic->dev, "%s: vf %d vlan %d\n",
			__func__, vf, le16_to_cpu(vfc.vlanid));

		ret = ionic_set_vf_config(ionic, vf, &vfc);
		if (!ret)
			ionic->vfs[vf].vlanid = cpu_to_le16(vlan);
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_rate(struct net_device *netdev, int vf,
			     int tx_min, int tx_max)
{
	struct ionic_vf_setattr_cmd vfc = { .attr = IONIC_VF_ATTR_RATE };
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret;

	/* setting the min just seems silly */
	if (tx_min)
		return -EINVAL;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		vfc.maxrate = cpu_to_le32(tx_max);
		dev_dbg(ionic->dev, "%s: vf %d maxrate %d\n",
			__func__, vf, le32_to_cpu(vfc.maxrate));

		ret = ionic_set_vf_config(ionic, vf, &vfc);
		if (!ret)
			ionic->vfs[vf].maxrate = cpu_to_le32(tx_max);
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_spoofchk(struct net_device *netdev, int vf, bool set)
{
	struct ionic_vf_setattr_cmd vfc = { .attr = IONIC_VF_ATTR_SPOOFCHK };
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		vfc.spoofchk = set;
		dev_dbg(ionic->dev, "%s: vf %d spoof %d\n",
			__func__, vf, vfc.spoofchk);

		ret = ionic_set_vf_config(ionic, vf, &vfc);
		if (!ret)
			ionic->vfs[vf].spoofchk = set;
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_trust(struct net_device *netdev, int vf, bool set)
{
	struct ionic_vf_setattr_cmd vfc = { .attr = IONIC_VF_ATTR_TRUST };
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		vfc.trust = set;
		dev_dbg(ionic->dev, "%s: vf %d trust %d\n",
			__func__, vf, vfc.trust);

		ret = ionic_set_vf_config(ionic, vf, &vfc);
		if (!ret)
			ionic->vfs[vf].trusted = set;
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_link_state(struct net_device *netdev, int vf, int set)
{
	struct ionic_vf_setattr_cmd vfc = { .attr = IONIC_VF_ATTR_LINKSTATE };
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	u8 vfls;
	int ret;

	switch (set) {
	case IFLA_VF_LINK_STATE_ENABLE:
		vfls = IONIC_VF_LINK_STATUS_UP;
		break;
	case IFLA_VF_LINK_STATE_DISABLE:
		vfls = IONIC_VF_LINK_STATUS_DOWN;
		break;
	case IFLA_VF_LINK_STATE_AUTO:
		vfls = IONIC_VF_LINK_STATUS_AUTO;
		break;
	default:
		return -EINVAL;
	}

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		vfc.linkstate = vfls;
		dev_dbg(ionic->dev, "%s: vf %d linkstate %d\n",
			__func__, vf, vfc.linkstate);

		ret = ionic_set_vf_config(ionic, vf, &vfc);
		if (!ret)
			ionic->vfs[vf].linkstate = set;
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static void ionic_vf_attr_replay(struct ionic_lif *lif)
{
	struct ionic_vf_setattr_cmd vfc = { };
	struct ionic *ionic = lif->ionic;
	struct ionic_vf *v;
	int i;

	if (!ionic->vfs)
		return;

	down_read(&ionic->vf_op_lock);

	for (i = 0; i < ionic->num_vfs; i++) {
		v = &ionic->vfs[i];

		if (v->stats_pa) {
			vfc.attr = IONIC_VF_ATTR_STATSADDR;
			vfc.stats_pa = cpu_to_le64(v->stats_pa);
			ionic_set_vf_config(ionic, i, &vfc);
			vfc.stats_pa = 0;
		}

		if (!is_zero_ether_addr(v->macaddr)) {
			vfc.attr = IONIC_VF_ATTR_MAC;
			ether_addr_copy(vfc.macaddr, v->macaddr);
			ionic_set_vf_config(ionic, i, &vfc);
			eth_zero_addr(vfc.macaddr);
		}

		if (v->vlanid) {
			vfc.attr = IONIC_VF_ATTR_VLAN;
			vfc.vlanid = v->vlanid;
			ionic_set_vf_config(ionic, i, &vfc);
			vfc.vlanid = 0;
		}

		if (v->maxrate) {
			vfc.attr = IONIC_VF_ATTR_RATE;
			vfc.maxrate = v->maxrate;
			ionic_set_vf_config(ionic, i, &vfc);
			vfc.maxrate = 0;
		}

		if (v->spoofchk) {
			vfc.attr = IONIC_VF_ATTR_SPOOFCHK;
			vfc.spoofchk = v->spoofchk;
			ionic_set_vf_config(ionic, i, &vfc);
			vfc.spoofchk = 0;
		}

		if (v->trusted) {
			vfc.attr = IONIC_VF_ATTR_TRUST;
			vfc.trust = v->trusted;
			ionic_set_vf_config(ionic, i, &vfc);
			vfc.trust = 0;
		}

		if (v->linkstate) {
			vfc.attr = IONIC_VF_ATTR_LINKSTATE;
			vfc.linkstate = v->linkstate;
			ionic_set_vf_config(ionic, i, &vfc);
			vfc.linkstate = 0;
		}
	}

	up_read(&ionic->vf_op_lock);

	ionic_vf_start(ionic);
}

static void ionic_unregister_rxq_info(struct ionic_queue *q)
{
	struct xdp_rxq_info *xi;

	if (!q->xdp_rxq_info)
		return;

	xi = q->xdp_rxq_info;
	q->xdp_rxq_info = NULL;

	xdp_rxq_info_unreg(xi);
	kfree(xi);
}

static int ionic_register_rxq_info(struct ionic_queue *q, unsigned int napi_id)
{
	struct xdp_rxq_info *rxq_info;
	int err;

	rxq_info = kzalloc(sizeof(*rxq_info), GFP_KERNEL);
	if (!rxq_info)
		return -ENOMEM;

	err = xdp_rxq_info_reg(rxq_info, q->lif->netdev, q->index, napi_id);
	if (err) {
		netdev_err(q->lif->netdev, "q%d xdp_rxq_info_reg failed, err %d\n",
			   q->index, err);
		goto err_out;
	}

	err = xdp_rxq_info_reg_mem_model(rxq_info, MEM_TYPE_PAGE_POOL, q->page_pool);
	if (err) {
		netdev_err(q->lif->netdev, "q%d xdp_rxq_info_reg_mem_model failed, err %d\n",
			   q->index, err);
		xdp_rxq_info_unreg(rxq_info);
		goto err_out;
	}

	q->xdp_rxq_info = rxq_info;

	return 0;

err_out:
	kfree(rxq_info);
	return err;
}

static void ionic_xdp_rxqs_prog_update(struct ionic_lif *lif)
{
	struct bpf_prog *xdp_prog;
	unsigned int i;

	if (!lif->rxqcqs)
		return;

	xdp_prog = READ_ONCE(lif->xdp_prog);
	for (i = 0; i < lif->ionic->nrxqs_per_lif && lif->rxqcqs[i]; i++) {
		struct ionic_queue *q = &lif->rxqcqs[i]->q;

		WRITE_ONCE(q->xdp_prog, xdp_prog);
	}
}

static int ionic_xdp_config(struct net_device *netdev, struct netdev_bpf *bpf)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct bpf_prog *old_prog;
	u32 maxfs;

	if (test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state)) {
#define XDP_ERR_SPLIT "XDP not available with split Tx/Rx interrupts"
		NL_SET_ERR_MSG_MOD(bpf->extack, XDP_ERR_SPLIT);
		netdev_info(lif->netdev, XDP_ERR_SPLIT);
		return -EOPNOTSUPP;
	}

	if (!ionic_xdp_is_valid_mtu(lif, netdev->mtu, bpf->prog)) {
#define XDP_ERR_MTU "MTU is too large for XDP without frags support"
		NL_SET_ERR_MSG_MOD(bpf->extack, XDP_ERR_MTU);
		netdev_info(lif->netdev, XDP_ERR_MTU);
		return -EINVAL;
	}

	maxfs = __le32_to_cpu(lif->identity->eth.max_frame_size) - VLAN_ETH_HLEN;
	if (bpf->prog && !(bpf->prog->aux && bpf->prog->aux->xdp_has_frags))
		maxfs = min_t(u32, maxfs, IONIC_XDP_MAX_LINEAR_MTU);
	netdev->max_mtu = maxfs;

	if (!netif_running(netdev)) {
		old_prog = xchg(&lif->xdp_prog, bpf->prog);
	} else if (lif->xdp_prog && bpf->prog) {
		old_prog = xchg(&lif->xdp_prog, bpf->prog);
		ionic_xdp_rxqs_prog_update(lif);
	} else {
		struct ionic_queue_params qparams;

		ionic_init_queue_params(lif, &qparams);
		qparams.xdp_prog = bpf->prog;
		mutex_lock(&lif->queue_lock);
		ionic_reconfigure_queues(lif, &qparams);
		old_prog = xchg(&lif->xdp_prog, bpf->prog);
		mutex_unlock(&lif->queue_lock);
	}

	if (old_prog)
		bpf_prog_put(old_prog);

	return 0;
}

static int ionic_xdp(struct net_device *netdev, struct netdev_bpf *bpf)
{
	switch (bpf->command) {
	case XDP_SETUP_PROG:
		return ionic_xdp_config(netdev, bpf);
	default:
		return -EINVAL;
	}
}

static const struct net_device_ops ionic_netdev_ops = {
	.ndo_open               = ionic_open,
	.ndo_stop               = ionic_stop,
	.ndo_eth_ioctl		= ionic_eth_ioctl,
	.ndo_start_xmit		= ionic_start_xmit,
	.ndo_bpf		= ionic_xdp,
	.ndo_xdp_xmit		= ionic_xdp_xmit,
	.ndo_get_stats64	= ionic_get_stats64,
	.ndo_set_rx_mode	= ionic_ndo_set_rx_mode,
	.ndo_set_features	= ionic_set_features,
	.ndo_set_mac_address	= ionic_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_tx_timeout         = ionic_tx_timeout,
	.ndo_change_mtu         = ionic_change_mtu,
	.ndo_vlan_rx_add_vid    = ionic_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid   = ionic_vlan_rx_kill_vid,
	.ndo_set_vf_vlan	= ionic_set_vf_vlan,
	.ndo_set_vf_trust	= ionic_set_vf_trust,
	.ndo_set_vf_mac		= ionic_set_vf_mac,
	.ndo_set_vf_rate	= ionic_set_vf_rate,
	.ndo_set_vf_spoofchk	= ionic_set_vf_spoofchk,
	.ndo_get_vf_config	= ionic_get_vf_config,
	.ndo_set_vf_link_state	= ionic_set_vf_link_state,
	.ndo_get_vf_stats       = ionic_get_vf_stats,
};

static int ionic_cmb_reconfig(struct ionic_lif *lif,
			      struct ionic_queue_params *qparam)
{
	struct ionic_queue_params start_qparams;
	int err = 0;

	/* When changing CMB queue parameters, we're using limited
	 * on-device memory and don't have extra memory to use for
	 * duplicate allocations, so we free it all first then
	 * re-allocate with the new parameters.
	 */

	/* Checkpoint for possible unwind */
	ionic_init_queue_params(lif, &start_qparams);

	/* Stop and free the queues */
	ionic_stop_queues_reconfig(lif);
	ionic_txrx_free(lif);

	/* Set up new qparams */
	ionic_set_queue_params(lif, qparam);

	if (netif_running(lif->netdev)) {
		/* Alloc and start the new configuration */
		err = ionic_txrx_alloc(lif);
		if (err) {
			dev_warn(lif->ionic->dev,
				 "CMB reconfig failed, restoring values: %d\n", err);

			/* Back out the changes */
			ionic_set_queue_params(lif, &start_qparams);
			err = ionic_txrx_alloc(lif);
			if (err) {
				dev_err(lif->ionic->dev,
					"CMB restore failed: %d\n", err);
				goto err_out;
			}
		}

		err = ionic_start_queues_reconfig(lif);
		if (err) {
			dev_err(lif->ionic->dev,
				"CMB reconfig failed: %d\n", err);
			goto err_out;
		}
	}

err_out:
	/* This was detached in ionic_stop_queues_reconfig() */
	netif_device_attach(lif->netdev);

	return err;
}

static void ionic_swap_queues(struct ionic_qcq *a, struct ionic_qcq *b)
{
	/* only swapping the queues and napi, not flags or other stuff */
	swap(a->napi,         b->napi);

	if (a->q.type == IONIC_QTYPE_RXQ) {
		swap(a->q.page_pool, b->q.page_pool);
		a->q.page_pool->p.napi = &a->napi;
		if (b->q.page_pool)  /* is NULL when increasing queue count */
			b->q.page_pool->p.napi = &b->napi;
	}

	swap(a->q.features,   b->q.features);
	swap(a->q.num_descs,  b->q.num_descs);
	swap(a->q.desc_size,  b->q.desc_size);
	swap(a->q.base,       b->q.base);
	swap(a->q.base_pa,    b->q.base_pa);
	swap(a->q.info,       b->q.info);
	swap(a->q.xdp_prog,   b->q.xdp_prog);
	swap(a->q.xdp_rxq_info, b->q.xdp_rxq_info);
	swap(a->q.partner,    b->q.partner);
	swap(a->q_base,       b->q_base);
	swap(a->q_base_pa,    b->q_base_pa);
	swap(a->q_size,       b->q_size);

	swap(a->q.sg_desc_size, b->q.sg_desc_size);
	swap(a->q.sg_base,    b->q.sg_base);
	swap(a->q.sg_base_pa, b->q.sg_base_pa);
	swap(a->sg_base,      b->sg_base);
	swap(a->sg_base_pa,   b->sg_base_pa);
	swap(a->sg_size,      b->sg_size);

	swap(a->cq.num_descs, b->cq.num_descs);
	swap(a->cq.desc_size, b->cq.desc_size);
	swap(a->cq.base,      b->cq.base);
	swap(a->cq.base_pa,   b->cq.base_pa);
	swap(a->cq_base,      b->cq_base);
	swap(a->cq_base_pa,   b->cq_base_pa);
	swap(a->cq_size,      b->cq_size);

	ionic_debugfs_del_qcq(a);
	ionic_debugfs_add_qcq(a->q.lif, a);
}

int ionic_reconfigure_queues(struct ionic_lif *lif,
			     struct ionic_queue_params *qparam)
{
	unsigned int comp_sz, desc_sz, num_desc, sg_desc_sz;
	struct ionic_qcq **tx_qcqs = NULL;
	struct ionic_qcq **rx_qcqs = NULL;
	unsigned int flags, i;
	int err = 0;

	/* Are we changing q params while CMB is on */
	if ((test_bit(IONIC_LIF_F_CMB_TX_RINGS, lif->state) && qparam->cmb_tx) ||
	    (test_bit(IONIC_LIF_F_CMB_RX_RINGS, lif->state) && qparam->cmb_rx))
		return ionic_cmb_reconfig(lif, qparam);

	/* allocate temporary qcq arrays to hold new queue structs */
	if (qparam->nxqs != lif->nxqs || qparam->ntxq_descs != lif->ntxq_descs) {
		tx_qcqs = devm_kcalloc(lif->ionic->dev, lif->ionic->ntxqs_per_lif,
				       sizeof(struct ionic_qcq *), GFP_KERNEL);
		if (!tx_qcqs) {
			err = -ENOMEM;
			goto err_out;
		}
	}
	if (qparam->nxqs != lif->nxqs ||
	    qparam->nrxq_descs != lif->nrxq_descs ||
	    qparam->rxq_features != lif->rxq_features ||
	    qparam->xdp_prog != lif->xdp_prog) {
		rx_qcqs = devm_kcalloc(lif->ionic->dev, lif->ionic->nrxqs_per_lif,
				       sizeof(struct ionic_qcq *), GFP_KERNEL);
		if (!rx_qcqs) {
			err = -ENOMEM;
			goto err_out;
		}
	}

	/* allocate new desc_info and rings, but leave the interrupt setup
	 * until later so as to not mess with the still-running queues
	 */
	if (tx_qcqs) {
		num_desc = qparam->ntxq_descs;
		desc_sz = sizeof(struct ionic_txq_desc);
		comp_sz = sizeof(struct ionic_txq_comp);

		if (lif->qtype_info[IONIC_QTYPE_TXQ].version >= 1 &&
		    lif->qtype_info[IONIC_QTYPE_TXQ].sg_desc_sz ==
		    sizeof(struct ionic_txq_sg_desc_v1))
			sg_desc_sz = sizeof(struct ionic_txq_sg_desc_v1);
		else
			sg_desc_sz = sizeof(struct ionic_txq_sg_desc);

		for (i = 0; i < qparam->nxqs; i++) {
			/* If missing, short placeholder qcq needed for swap */
			if (!lif->txqcqs[i]) {
				flags = IONIC_QCQ_F_TX_STATS | IONIC_QCQ_F_SG;
				err = ionic_qcq_alloc(lif, IONIC_QTYPE_TXQ, i, "tx", flags,
						      4, desc_sz, comp_sz, sg_desc_sz,
						      sizeof(struct ionic_tx_desc_info),
						      lif->kern_pid, NULL, &lif->txqcqs[i]);
				if (err)
					goto err_out;
			}

			flags = lif->txqcqs[i]->flags & ~IONIC_QCQ_F_INTR;
			err = ionic_qcq_alloc(lif, IONIC_QTYPE_TXQ, i, "tx", flags,
					      num_desc, desc_sz, comp_sz, sg_desc_sz,
					      sizeof(struct ionic_tx_desc_info),
					      lif->kern_pid, NULL, &tx_qcqs[i]);
			if (err)
				goto err_out;
		}
	}

	if (rx_qcqs) {
		num_desc = qparam->nrxq_descs;
		desc_sz = sizeof(struct ionic_rxq_desc);
		comp_sz = sizeof(struct ionic_rxq_comp);
		sg_desc_sz = sizeof(struct ionic_rxq_sg_desc);

		if (qparam->rxq_features & IONIC_Q_F_2X_CQ_DESC)
			comp_sz *= 2;

		for (i = 0; i < qparam->nxqs; i++) {
			/* If missing, short placeholder qcq needed for swap */
			if (!lif->rxqcqs[i]) {
				flags = IONIC_QCQ_F_RX_STATS | IONIC_QCQ_F_SG;
				err = ionic_qcq_alloc(lif, IONIC_QTYPE_RXQ, i, "rx", flags,
						      4, desc_sz, comp_sz, sg_desc_sz,
						      sizeof(struct ionic_rx_desc_info),
						      lif->kern_pid, NULL, &lif->rxqcqs[i]);
				if (err)
					goto err_out;
			}

			flags = lif->rxqcqs[i]->flags & ~IONIC_QCQ_F_INTR;
			err = ionic_qcq_alloc(lif, IONIC_QTYPE_RXQ, i, "rx", flags,
					      num_desc, desc_sz, comp_sz, sg_desc_sz,
					      sizeof(struct ionic_rx_desc_info),
					      lif->kern_pid, qparam->xdp_prog, &rx_qcqs[i]);
			if (err)
				goto err_out;

			rx_qcqs[i]->q.features = qparam->rxq_features;
			rx_qcqs[i]->q.xdp_prog = qparam->xdp_prog;
		}
	}

	/* stop and clean the queues */
	ionic_stop_queues_reconfig(lif);

	if (qparam->nxqs != lif->nxqs) {
		err = netif_set_real_num_tx_queues(lif->netdev, qparam->nxqs);
		if (err)
			goto err_out_reinit_unlock;
		err = netif_set_real_num_rx_queues(lif->netdev, qparam->nxqs);
		if (err) {
			netif_set_real_num_tx_queues(lif->netdev, lif->nxqs);
			goto err_out_reinit_unlock;
		}
	}

	/* swap new desc_info and rings, keeping existing interrupt config */
	if (tx_qcqs) {
		lif->ntxq_descs = qparam->ntxq_descs;
		for (i = 0; i < qparam->nxqs; i++)
			ionic_swap_queues(lif->txqcqs[i], tx_qcqs[i]);
	}

	if (rx_qcqs) {
		lif->nrxq_descs = qparam->nrxq_descs;
		for (i = 0; i < qparam->nxqs; i++)
			ionic_swap_queues(lif->rxqcqs[i], rx_qcqs[i]);
	}

	/* if we need to change the interrupt layout, this is the time */
	if (qparam->intr_split != test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state) ||
	    qparam->nxqs != lif->nxqs) {
		if (qparam->intr_split) {
			set_bit(IONIC_LIF_F_SPLIT_INTR, lif->state);
		} else {
			clear_bit(IONIC_LIF_F_SPLIT_INTR, lif->state);
			lif->tx_coalesce_usecs = lif->rx_coalesce_usecs;
			lif->tx_coalesce_hw = lif->rx_coalesce_hw;
		}

		/* Clear existing interrupt assignments.  We check for NULL here
		 * because we're checking the whole array for potential qcqs, not
		 * just those qcqs that have just been set up.
		 */
		for (i = 0; i < lif->ionic->ntxqs_per_lif; i++) {
			if (lif->txqcqs[i])
				ionic_qcq_intr_free(lif, lif->txqcqs[i]);
			if (lif->rxqcqs[i])
				ionic_qcq_intr_free(lif, lif->rxqcqs[i]);
		}

		/* re-assign the interrupts */
		for (i = 0; i < qparam->nxqs; i++) {
			lif->rxqcqs[i]->flags |= IONIC_QCQ_F_INTR;
			err = ionic_alloc_qcq_interrupt(lif, lif->rxqcqs[i]);
			ionic_intr_coal_init(lif->ionic->idev.intr_ctrl,
					     lif->rxqcqs[i]->intr.index,
					     lif->rx_coalesce_hw);

			if (qparam->intr_split) {
				lif->txqcqs[i]->flags |= IONIC_QCQ_F_INTR;
				err = ionic_alloc_qcq_interrupt(lif, lif->txqcqs[i]);
				ionic_intr_coal_init(lif->ionic->idev.intr_ctrl,
						     lif->txqcqs[i]->intr.index,
						     lif->tx_coalesce_hw);
				if (test_bit(IONIC_LIF_F_TX_DIM_INTR, lif->state))
					lif->txqcqs[i]->intr.dim_coal_hw = lif->tx_coalesce_hw;
			} else {
				lif->txqcqs[i]->flags &= ~IONIC_QCQ_F_INTR;
				ionic_link_qcq_interrupts(lif->rxqcqs[i], lif->txqcqs[i]);
			}
		}
	}

	/* now we can rework the debugfs mappings */
	if (tx_qcqs) {
		for (i = 0; i < qparam->nxqs; i++) {
			ionic_debugfs_del_qcq(lif->txqcqs[i]);
			ionic_debugfs_add_qcq(lif, lif->txqcqs[i]);
		}
	}

	if (rx_qcqs) {
		for (i = 0; i < qparam->nxqs; i++) {
			ionic_debugfs_del_qcq(lif->rxqcqs[i]);
			ionic_debugfs_add_qcq(lif, lif->rxqcqs[i]);
		}
	}

	swap(lif->nxqs, qparam->nxqs);
	swap(lif->rxq_features, qparam->rxq_features);

err_out_reinit_unlock:
	/* re-init the queues, but don't lose an error code */
	if (err)
		ionic_start_queues_reconfig(lif);
	else
		err = ionic_start_queues_reconfig(lif);

err_out:
	/* free old allocs without cleaning intr */
	for (i = 0; i < qparam->nxqs; i++) {
		if (tx_qcqs && tx_qcqs[i]) {
			tx_qcqs[i]->flags &= ~IONIC_QCQ_F_INTR;
			ionic_qcq_free(lif, tx_qcqs[i]);
			devm_kfree(lif->ionic->dev, tx_qcqs[i]);
			tx_qcqs[i] = NULL;
		}
		if (rx_qcqs && rx_qcqs[i]) {
			rx_qcqs[i]->flags &= ~IONIC_QCQ_F_INTR;
			ionic_qcq_free(lif, rx_qcqs[i]);
			devm_kfree(lif->ionic->dev, rx_qcqs[i]);
			rx_qcqs[i] = NULL;
		}
	}

	/* free q array */
	if (rx_qcqs) {
		devm_kfree(lif->ionic->dev, rx_qcqs);
		rx_qcqs = NULL;
	}
	if (tx_qcqs) {
		devm_kfree(lif->ionic->dev, tx_qcqs);
		tx_qcqs = NULL;
	}

	/* clean the unused dma and info allocations when new set is smaller
	 * than the full array, but leave the qcq shells in place
	 */
	for (i = lif->nxqs; i < lif->ionic->ntxqs_per_lif; i++) {
		if (lif->txqcqs && lif->txqcqs[i]) {
			lif->txqcqs[i]->flags &= ~IONIC_QCQ_F_INTR;
			ionic_qcq_free(lif, lif->txqcqs[i]);
		}

		if (lif->rxqcqs && lif->rxqcqs[i]) {
			lif->rxqcqs[i]->flags &= ~IONIC_QCQ_F_INTR;
			ionic_qcq_free(lif, lif->rxqcqs[i]);
		}
	}

	if (err)
		netdev_info(lif->netdev, "%s: failed %d\n", __func__, err);

	return err;
}

static int ionic_affinity_masks_alloc(struct ionic *ionic)
{
	cpumask_var_t *affinity_masks;
	int nintrs = ionic->nintrs;
	int i;

	affinity_masks = kcalloc(nintrs, sizeof(cpumask_var_t), GFP_KERNEL);
	if (!affinity_masks)
		return -ENOMEM;

	for (i = 0; i < nintrs; i++) {
		if (!zalloc_cpumask_var_node(&affinity_masks[i], GFP_KERNEL,
					     dev_to_node(ionic->dev)))
			goto err_out;
	}

	ionic->affinity_masks = affinity_masks;

	return 0;

err_out:
	for (--i; i >= 0; i--)
		free_cpumask_var(affinity_masks[i]);
	kfree(affinity_masks);

	return -ENOMEM;
}

static void ionic_affinity_masks_free(struct ionic *ionic)
{
	int i;

	for (i = 0; i < ionic->nintrs; i++)
		free_cpumask_var(ionic->affinity_masks[i]);
	kfree(ionic->affinity_masks);
	ionic->affinity_masks = NULL;
}

int ionic_lif_alloc(struct ionic *ionic)
{
	struct device *dev = ionic->dev;
	union ionic_lif_identity *lid;
	struct net_device *netdev;
	struct ionic_lif *lif;
	int tbl_sz;
	int err;

	lid = kzalloc(sizeof(*lid), GFP_KERNEL);
	if (!lid)
		return -ENOMEM;

	netdev = alloc_etherdev_mqs(sizeof(*lif),
				    ionic->ntxqs_per_lif, ionic->ntxqs_per_lif);
	if (!netdev) {
		dev_err(dev, "Cannot allocate netdev, aborting\n");
		err = -ENOMEM;
		goto err_out_free_lid;
	}

	SET_NETDEV_DEV(netdev, dev);

	lif = netdev_priv(netdev);
	lif->netdev = netdev;
	ionic->lif = lif;
	lif->ionic = ionic;
	netdev->netdev_ops = &ionic_netdev_ops;
	ionic_ethtool_set_ops(netdev);

	netdev->watchdog_timeo = 5 * HZ;
	netif_carrier_off(netdev);

	lif->identity = lid;
	lif->lif_type = IONIC_LIF_TYPE_CLASSIC;
	err = ionic_lif_identify(ionic, lif->lif_type, lif->identity);
	if (err) {
		dev_err(ionic->dev, "Cannot identify type %d: %d\n",
			lif->lif_type, err);
		goto err_out_free_netdev;
	}
	lif->netdev->min_mtu = max_t(unsigned int, ETH_MIN_MTU,
				     le32_to_cpu(lif->identity->eth.min_frame_size));
	lif->netdev->max_mtu =
		le32_to_cpu(lif->identity->eth.max_frame_size) - VLAN_ETH_HLEN;

	lif->neqs = ionic->neqs_per_lif;
	lif->nxqs = ionic->ntxqs_per_lif;

	lif->index = 0;

	if (is_kdump_kernel()) {
		lif->ntxq_descs = IONIC_MIN_TXRX_DESC;
		lif->nrxq_descs = IONIC_MIN_TXRX_DESC;
	} else {
		lif->ntxq_descs = IONIC_DEF_TXRX_DESC;
		lif->nrxq_descs = IONIC_DEF_TXRX_DESC;
	}

	/* Convert the default coalesce value to actual hw resolution */
	lif->rx_coalesce_usecs = IONIC_ITR_COAL_USEC_DEFAULT;
	lif->rx_coalesce_hw = ionic_coal_usec_to_hw(lif->ionic,
						    lif->rx_coalesce_usecs);
	lif->tx_coalesce_usecs = lif->rx_coalesce_usecs;
	lif->tx_coalesce_hw = lif->rx_coalesce_hw;
	set_bit(IONIC_LIF_F_RX_DIM_INTR, lif->state);
	set_bit(IONIC_LIF_F_TX_DIM_INTR, lif->state);

	snprintf(lif->name, sizeof(lif->name), "lif%u", lif->index);

	mutex_init(&lif->queue_lock);
	mutex_init(&lif->config_lock);

	spin_lock_init(&lif->adminq_lock);

	spin_lock_init(&lif->deferred.lock);
	INIT_LIST_HEAD(&lif->deferred.list);
	INIT_WORK(&lif->deferred.work, ionic_lif_deferred_work);

	/* allocate lif info */
	lif->info_sz = ALIGN(sizeof(*lif->info), PAGE_SIZE);
	lif->info = dma_alloc_coherent(dev, lif->info_sz,
				       &lif->info_pa, GFP_KERNEL);
	if (!lif->info) {
		dev_err(dev, "Failed to allocate lif info, aborting\n");
		err = -ENOMEM;
		goto err_out_free_mutex;
	}

	ionic_debugfs_add_lif(lif);

	err = ionic_affinity_masks_alloc(ionic);
	if (err)
		goto err_out_free_lif_info;

	/* allocate control queues and txrx queue arrays */
	ionic_lif_queue_identify(lif);
	err = ionic_qcqs_alloc(lif);
	if (err)
		goto err_out_free_affinity_masks;

	/* allocate rss indirection table */
	tbl_sz = le16_to_cpu(lif->ionic->ident.lif.eth.rss_ind_tbl_sz);
	lif->rss_ind_tbl_sz = sizeof(*lif->rss_ind_tbl) * tbl_sz;
	lif->rss_ind_tbl = dma_alloc_coherent(dev, lif->rss_ind_tbl_sz,
					      &lif->rss_ind_tbl_pa,
					      GFP_KERNEL);

	if (!lif->rss_ind_tbl) {
		err = -ENOMEM;
		dev_err(dev, "Failed to allocate rss indirection table, aborting\n");
		goto err_out_free_qcqs;
	}
	netdev_rss_key_fill(lif->rss_hash_key, IONIC_RSS_HASH_KEY_SIZE);

	ionic_lif_alloc_phc(lif);

	return 0;

err_out_free_qcqs:
	ionic_qcqs_free(lif);
err_out_free_affinity_masks:
	ionic_affinity_masks_free(lif->ionic);
err_out_free_lif_info:
	dma_free_coherent(dev, lif->info_sz, lif->info, lif->info_pa);
	lif->info = NULL;
	lif->info_pa = 0;
err_out_free_mutex:
	mutex_destroy(&lif->config_lock);
	mutex_destroy(&lif->queue_lock);
err_out_free_netdev:
	free_netdev(lif->netdev);
	lif = NULL;
err_out_free_lid:
	kfree(lid);

	return err;
}

static void ionic_lif_reset(struct ionic_lif *lif)
{
	struct ionic_dev *idev = &lif->ionic->idev;

	if (!ionic_is_fw_running(idev))
		return;

	mutex_lock(&lif->ionic->dev_cmd_lock);
	ionic_dev_cmd_lif_reset(idev, lif->index);
	ionic_dev_cmd_wait(lif->ionic, DEVCMD_TIMEOUT);
	mutex_unlock(&lif->ionic->dev_cmd_lock);
}

static void ionic_lif_handle_fw_down(struct ionic_lif *lif)
{
	struct ionic *ionic = lif->ionic;

	if (test_and_set_bit(IONIC_LIF_F_FW_RESET, lif->state))
		return;

	dev_info(ionic->dev, "FW Down: Stopping LIFs\n");

	netif_device_detach(lif->netdev);

	mutex_lock(&lif->queue_lock);
	if (test_bit(IONIC_LIF_F_UP, lif->state)) {
		dev_info(ionic->dev, "Surprise FW stop, stopping queues\n");
		ionic_stop_queues(lif);
	}

	if (netif_running(lif->netdev)) {
		ionic_txrx_deinit(lif);
		ionic_txrx_free(lif);
	}
	ionic_lif_deinit(lif);
	ionic_reset(ionic);
	ionic_qcqs_free(lif);

	mutex_unlock(&lif->queue_lock);

	clear_bit(IONIC_LIF_F_FW_STOPPING, lif->state);
	dev_info(ionic->dev, "FW Down: LIFs stopped\n");
}

int ionic_restart_lif(struct ionic_lif *lif)
{
	struct ionic *ionic = lif->ionic;
	int err;

	mutex_lock(&lif->queue_lock);

	if (test_and_clear_bit(IONIC_LIF_F_BROKEN, lif->state))
		dev_info(ionic->dev, "FW Up: clearing broken state\n");

	err = ionic_qcqs_alloc(lif);
	if (err)
		goto err_unlock;

	err = ionic_lif_init(lif);
	if (err)
		goto err_qcqs_free;

	ionic_vf_attr_replay(lif);

	if (lif->registered)
		ionic_lif_set_netdev_info(lif);

	ionic_rx_filter_replay(lif);

	if (netif_running(lif->netdev)) {
		err = ionic_txrx_alloc(lif);
		if (err)
			goto err_lifs_deinit;

		err = ionic_txrx_init(lif);
		if (err)
			goto err_txrx_free;
	}

	mutex_unlock(&lif->queue_lock);

	clear_bit(IONIC_LIF_F_FW_RESET, lif->state);
	ionic_link_status_check_request(lif, CAN_SLEEP);
	netif_device_attach(lif->netdev);
	ionic_queue_doorbell_check(ionic, IONIC_NAPI_DEADLINE);

	return 0;

err_txrx_free:
	ionic_txrx_free(lif);
err_lifs_deinit:
	ionic_lif_deinit(lif);
err_qcqs_free:
	ionic_qcqs_free(lif);
err_unlock:
	mutex_unlock(&lif->queue_lock);

	return err;
}

static void ionic_lif_handle_fw_up(struct ionic_lif *lif)
{
	struct ionic *ionic = lif->ionic;
	int err;

	if (!test_bit(IONIC_LIF_F_FW_RESET, lif->state))
		return;

	dev_info(ionic->dev, "FW Up: restarting LIFs\n");

	/* This is a little different from what happens at
	 * probe time because the LIF already exists so we
	 * just need to reanimate it.
	 */
	ionic_init_devinfo(ionic);
	ionic_reset(ionic);
	err = ionic_identify(ionic);
	if (err)
		goto err_out;
	err = ionic_port_identify(ionic);
	if (err)
		goto err_out;
	err = ionic_port_init(ionic);
	if (err)
		goto err_out;

	err = ionic_restart_lif(lif);
	if (err)
		goto err_out;

	dev_info(ionic->dev, "FW Up: LIFs restarted\n");

	/* restore the hardware timestamping queues */
	ionic_lif_hwstamp_replay(lif);

	return;

err_out:
	dev_err(ionic->dev, "FW Up: LIFs restart failed - err %d\n", err);
}

void ionic_lif_free(struct ionic_lif *lif)
{
	struct device *dev = lif->ionic->dev;

	ionic_lif_free_phc(lif);

	/* free rss indirection table */
	dma_free_coherent(dev, lif->rss_ind_tbl_sz, lif->rss_ind_tbl,
			  lif->rss_ind_tbl_pa);
	lif->rss_ind_tbl = NULL;
	lif->rss_ind_tbl_pa = 0;

	/* free queues */
	ionic_qcqs_free(lif);
	if (!test_bit(IONIC_LIF_F_FW_RESET, lif->state))
		ionic_lif_reset(lif);

	ionic_affinity_masks_free(lif->ionic);

	/* free lif info */
	kfree(lif->identity);
	dma_free_coherent(dev, lif->info_sz, lif->info, lif->info_pa);
	lif->info = NULL;
	lif->info_pa = 0;

	mutex_destroy(&lif->config_lock);
	mutex_destroy(&lif->queue_lock);

	/* free netdev & lif */
	ionic_debugfs_del_lif(lif);
	free_netdev(lif->netdev);
}

void ionic_lif_deinit(struct ionic_lif *lif)
{
	if (!test_and_clear_bit(IONIC_LIF_F_INITED, lif->state))
		return;

	if (!test_bit(IONIC_LIF_F_FW_RESET, lif->state)) {
		cancel_work_sync(&lif->deferred.work);
		cancel_work_sync(&lif->tx_timeout_work);
		ionic_rx_filters_deinit(lif);
		if (lif->netdev->features & NETIF_F_RXHASH)
			ionic_lif_rss_deinit(lif);
	}

	napi_disable(&lif->adminqcq->napi);
	ionic_lif_qcq_deinit(lif, lif->notifyqcq);
	ionic_lif_qcq_deinit(lif, lif->adminqcq);

	ionic_bus_unmap_dbpage(lif->ionic, lif->kern_dbpage);
	lif->kern_dbpage = NULL;

	ionic_lif_reset(lif);
}

static int ionic_lif_adminq_init(struct ionic_lif *lif)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_q_init_comp comp;
	struct ionic_dev *idev;
	struct ionic_qcq *qcq;
	struct ionic_queue *q;
	int err;

	idev = &lif->ionic->idev;
	qcq = lif->adminqcq;
	q = &qcq->q;

	mutex_lock(&lif->ionic->dev_cmd_lock);
	ionic_dev_cmd_adminq_init(idev, qcq, lif->index, qcq->intr.index);
	err = ionic_dev_cmd_wait(lif->ionic, DEVCMD_TIMEOUT);
	ionic_dev_cmd_comp(idev, (union ionic_dev_cmd_comp *)&comp);
	mutex_unlock(&lif->ionic->dev_cmd_lock);
	if (err) {
		netdev_err(lif->netdev, "adminq init failed %d\n", err);
		return err;
	}

	q->hw_type = comp.hw_type;
	q->hw_index = le32_to_cpu(comp.hw_index);
	q->dbval = IONIC_DBELL_QID(q->hw_index);

	dev_dbg(dev, "adminq->hw_type %d\n", q->hw_type);
	dev_dbg(dev, "adminq->hw_index %d\n", q->hw_index);

	q->dbell_deadline = IONIC_ADMIN_DOORBELL_DEADLINE;
	q->dbell_jiffies = jiffies;

	netif_napi_add(lif->netdev, &qcq->napi, ionic_adminq_napi);

	napi_enable(&qcq->napi);

	if (qcq->flags & IONIC_QCQ_F_INTR) {
		irq_set_affinity_hint(qcq->intr.vector,
				      *qcq->intr.affinity_mask);
		ionic_intr_mask(idev->intr_ctrl, qcq->intr.index,
				IONIC_INTR_MASK_CLEAR);
	}

	qcq->flags |= IONIC_QCQ_F_INITED;

	return 0;
}

static int ionic_lif_notifyq_init(struct ionic_lif *lif)
{
	struct ionic_qcq *qcq = lif->notifyqcq;
	struct device *dev = lif->ionic->dev;
	struct ionic_queue *q = &qcq->q;
	int err;

	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_init = {
			.opcode = IONIC_CMD_Q_INIT,
			.lif_index = cpu_to_le16(lif->index),
			.type = q->type,
			.ver = lif->qtype_info[q->type].version,
			.index = cpu_to_le32(q->index),
			.flags = cpu_to_le16(IONIC_QINIT_F_IRQ |
					     IONIC_QINIT_F_ENA),
			.intr_index = cpu_to_le16(lif->adminqcq->intr.index),
			.pid = cpu_to_le16(q->pid),
			.ring_size = ilog2(q->num_descs),
			.ring_base = cpu_to_le64(q->base_pa),
		}
	};

	dev_dbg(dev, "notifyq_init.pid %d\n", ctx.cmd.q_init.pid);
	dev_dbg(dev, "notifyq_init.index %d\n", ctx.cmd.q_init.index);
	dev_dbg(dev, "notifyq_init.ring_base 0x%llx\n", ctx.cmd.q_init.ring_base);
	dev_dbg(dev, "notifyq_init.ring_size %d\n", ctx.cmd.q_init.ring_size);

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	lif->last_eid = 0;
	q->hw_type = ctx.comp.q_init.hw_type;
	q->hw_index = le32_to_cpu(ctx.comp.q_init.hw_index);
	q->dbval = IONIC_DBELL_QID(q->hw_index);

	dev_dbg(dev, "notifyq->hw_type %d\n", q->hw_type);
	dev_dbg(dev, "notifyq->hw_index %d\n", q->hw_index);

	/* preset the callback info */
	q->admin_info[0].ctx = lif;

	qcq->flags |= IONIC_QCQ_F_INITED;

	return 0;
}

static int ionic_station_set(struct ionic_lif *lif)
{
	struct net_device *netdev = lif->netdev;
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_getattr = {
			.opcode = IONIC_CMD_LIF_GETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_MAC,
		},
	};
	u8 mac_address[ETH_ALEN];
	struct sockaddr addr;
	int err;

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;
	netdev_dbg(lif->netdev, "found initial MAC addr %pM\n",
		   ctx.comp.lif_getattr.mac);
	ether_addr_copy(mac_address, ctx.comp.lif_getattr.mac);

	if (is_zero_ether_addr(mac_address)) {
		eth_hw_addr_random(netdev);
		netdev_dbg(netdev, "Random Mac generated: %pM\n", netdev->dev_addr);
		ether_addr_copy(mac_address, netdev->dev_addr);

		err = ionic_program_mac(lif, mac_address);
		if (err < 0)
			return err;

		if (err > 0) {
			netdev_dbg(netdev, "%s:SET/GET ATTR Mac are not same-due to old FW running\n",
				   __func__);
			return 0;
		}
	}

	if (!is_zero_ether_addr(netdev->dev_addr)) {
		/* If the netdev mac is non-zero and doesn't match the default
		 * device address, it was set by something earlier and we're
		 * likely here again after a fw-upgrade reset.  We need to be
		 * sure the netdev mac is in our filter list.
		 */
		if (!ether_addr_equal(mac_address, netdev->dev_addr))
			ionic_lif_addr_add(lif, netdev->dev_addr);
	} else {
		/* Update the netdev mac with the device's mac */
		ether_addr_copy(addr.sa_data, mac_address);
		addr.sa_family = AF_INET;
		err = eth_prepare_mac_addr_change(netdev, &addr);
		if (err) {
			netdev_warn(lif->netdev, "ignoring bad MAC addr from NIC %pM - err %d\n",
				    addr.sa_data, err);
			return 0;
		}

		eth_commit_mac_addr_change(netdev, &addr);
	}

	netdev_dbg(lif->netdev, "adding station MAC addr %pM\n",
		   netdev->dev_addr);
	ionic_lif_addr_add(lif, netdev->dev_addr);

	return 0;
}

int ionic_lif_init(struct ionic_lif *lif)
{
	struct ionic_dev *idev = &lif->ionic->idev;
	struct device *dev = lif->ionic->dev;
	struct ionic_lif_init_comp comp;
	int dbpage_num;
	int err;

	mutex_lock(&lif->ionic->dev_cmd_lock);
	ionic_dev_cmd_lif_init(idev, lif->index, lif->info_pa);
	err = ionic_dev_cmd_wait(lif->ionic, DEVCMD_TIMEOUT);
	ionic_dev_cmd_comp(idev, (union ionic_dev_cmd_comp *)&comp);
	mutex_unlock(&lif->ionic->dev_cmd_lock);
	if (err)
		return err;

	lif->hw_index = le16_to_cpu(comp.hw_index);

	/* now that we have the hw_index we can figure out our doorbell page */
	lif->dbid_count = le32_to_cpu(lif->ionic->ident.dev.ndbpgs_per_lif);
	if (!lif->dbid_count) {
		dev_err(dev, "No doorbell pages, aborting\n");
		return -EINVAL;
	}

	lif->kern_pid = 0;
	dbpage_num = ionic_db_page_num(lif, lif->kern_pid);
	lif->kern_dbpage = ionic_bus_map_dbpage(lif->ionic, dbpage_num);
	if (!lif->kern_dbpage) {
		dev_err(dev, "Cannot map dbpage, aborting\n");
		return -ENOMEM;
	}

	err = ionic_lif_adminq_init(lif);
	if (err)
		goto err_out_adminq_deinit;

	if (lif->ionic->nnqs_per_lif) {
		err = ionic_lif_notifyq_init(lif);
		if (err)
			goto err_out_notifyq_deinit;
	}

	if (test_bit(IONIC_LIF_F_FW_RESET, lif->state))
		err = ionic_set_nic_features(lif, lif->netdev->features);
	else
		err = ionic_init_nic_features(lif);
	if (err)
		goto err_out_notifyq_deinit;

	if (!test_bit(IONIC_LIF_F_FW_RESET, lif->state)) {
		err = ionic_rx_filters_init(lif);
		if (err)
			goto err_out_notifyq_deinit;
	}

	err = ionic_station_set(lif);
	if (err)
		goto err_out_notifyq_deinit;

	lif->rx_copybreak = IONIC_RX_COPYBREAK_DEFAULT;
	lif->doorbell_wa = ionic_doorbell_wa(lif->ionic);

	set_bit(IONIC_LIF_F_INITED, lif->state);

	INIT_WORK(&lif->tx_timeout_work, ionic_tx_timeout_work);

	return 0;

err_out_notifyq_deinit:
	napi_disable(&lif->adminqcq->napi);
	ionic_lif_qcq_deinit(lif, lif->notifyqcq);
err_out_adminq_deinit:
	ionic_lif_qcq_deinit(lif, lif->adminqcq);
	ionic_lif_reset(lif);
	ionic_bus_unmap_dbpage(lif->ionic, lif->kern_dbpage);
	lif->kern_dbpage = NULL;

	return err;
}

static void ionic_lif_set_netdev_info(struct ionic_lif *lif)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_NAME,
		},
	};

	strscpy(ctx.cmd.lif_setattr.name, netdev_name(lif->netdev),
		sizeof(ctx.cmd.lif_setattr.name));

	ionic_adminq_post_wait(lif, &ctx);
}

static struct ionic_lif *ionic_netdev_lif(struct net_device *netdev)
{
	if (!netdev || netdev->netdev_ops->ndo_start_xmit != ionic_start_xmit)
		return NULL;

	return netdev_priv(netdev);
}

static int ionic_lif_notify(struct notifier_block *nb,
			    unsigned long event, void *info)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(info);
	struct ionic *ionic = container_of(nb, struct ionic, nb);
	struct ionic_lif *lif = ionic_netdev_lif(ndev);

	if (!lif || lif->ionic != ionic)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_CHANGENAME:
		ionic_lif_set_netdev_info(lif);
		break;
	}

	return NOTIFY_DONE;
}

int ionic_lif_register(struct ionic_lif *lif)
{
	int err;

	ionic_lif_register_phc(lif);

	lif->ionic->nb.notifier_call = ionic_lif_notify;

	err = register_netdevice_notifier(&lif->ionic->nb);
	if (err)
		lif->ionic->nb.notifier_call = NULL;

	/* only register LIF0 for now */
	err = register_netdev(lif->netdev);
	if (err) {
		dev_err(lif->ionic->dev, "Cannot register net device: %d, aborting\n", err);
		ionic_lif_unregister(lif);
		return err;
	}

	ionic_link_status_check_request(lif, CAN_SLEEP);
	lif->registered = true;
	ionic_lif_set_netdev_info(lif);

	return 0;
}

void ionic_lif_unregister(struct ionic_lif *lif)
{
	if (lif->ionic->nb.notifier_call) {
		unregister_netdevice_notifier(&lif->ionic->nb);
		lif->ionic->nb.notifier_call = NULL;
	}

	if (lif->netdev->reg_state == NETREG_REGISTERED)
		unregister_netdev(lif->netdev);

	ionic_lif_unregister_phc(lif);

	lif->registered = false;
}

static void ionic_lif_queue_identify(struct ionic_lif *lif)
{
	union ionic_q_identity __iomem *q_ident;
	struct ionic *ionic = lif->ionic;
	struct ionic_dev *idev;
	u16 max_frags;
	int qtype;
	int err;

	idev = &lif->ionic->idev;
	q_ident = (union ionic_q_identity __iomem *)&idev->dev_cmd_regs->data;

	for (qtype = 0; qtype < ARRAY_SIZE(ionic_qtype_versions); qtype++) {
		struct ionic_qtype_info *qti = &lif->qtype_info[qtype];

		/* filter out the ones we know about */
		switch (qtype) {
		case IONIC_QTYPE_ADMINQ:
		case IONIC_QTYPE_NOTIFYQ:
		case IONIC_QTYPE_RXQ:
		case IONIC_QTYPE_TXQ:
			break;
		default:
			continue;
		}

		memset(qti, 0, sizeof(*qti));

		mutex_lock(&ionic->dev_cmd_lock);
		ionic_dev_cmd_queue_identify(idev, lif->lif_type, qtype,
					     ionic_qtype_versions[qtype]);
		err = ionic_dev_cmd_wait(ionic, DEVCMD_TIMEOUT);
		if (!err) {
			qti->version   = readb(&q_ident->version);
			qti->supported = readb(&q_ident->supported);
			qti->features  = readq(&q_ident->features);
			qti->desc_sz   = readw(&q_ident->desc_sz);
			qti->comp_sz   = readw(&q_ident->comp_sz);
			qti->sg_desc_sz   = readw(&q_ident->sg_desc_sz);
			qti->max_sg_elems = readw(&q_ident->max_sg_elems);
			qti->sg_desc_stride = readw(&q_ident->sg_desc_stride);
		}
		mutex_unlock(&ionic->dev_cmd_lock);

		if (err == -EINVAL) {
			dev_err(ionic->dev, "qtype %d not supported\n", qtype);
			continue;
		} else if (err == -EIO) {
			dev_err(ionic->dev, "q_ident failed, not supported on older FW\n");
			return;
		} else if (err) {
			dev_err(ionic->dev, "q_ident failed, qtype %d: %d\n",
				qtype, err);
			return;
		}

		dev_dbg(ionic->dev, " qtype[%d].version = %d\n",
			qtype, qti->version);
		dev_dbg(ionic->dev, " qtype[%d].supported = 0x%02x\n",
			qtype, qti->supported);
		dev_dbg(ionic->dev, " qtype[%d].features = 0x%04llx\n",
			qtype, qti->features);
		dev_dbg(ionic->dev, " qtype[%d].desc_sz = %d\n",
			qtype, qti->desc_sz);
		dev_dbg(ionic->dev, " qtype[%d].comp_sz = %d\n",
			qtype, qti->comp_sz);
		dev_dbg(ionic->dev, " qtype[%d].sg_desc_sz = %d\n",
			qtype, qti->sg_desc_sz);
		dev_dbg(ionic->dev, " qtype[%d].max_sg_elems = %d\n",
			qtype, qti->max_sg_elems);
		dev_dbg(ionic->dev, " qtype[%d].sg_desc_stride = %d\n",
			qtype, qti->sg_desc_stride);

		if (qtype == IONIC_QTYPE_TXQ)
			max_frags = IONIC_TX_MAX_FRAGS;
		else if (qtype == IONIC_QTYPE_RXQ)
			max_frags = IONIC_RX_MAX_FRAGS;
		else
			max_frags = 1;

		qti->max_sg_elems = min_t(u16, max_frags - 1, MAX_SKB_FRAGS);
		dev_dbg(ionic->dev, "qtype %d max_sg_elems %d\n",
			qtype, qti->max_sg_elems);
	}
}

int ionic_lif_identify(struct ionic *ionic, u8 lif_type,
		       union ionic_lif_identity *lid)
{
	struct ionic_dev *idev = &ionic->idev;
	size_t sz;
	int err;

	sz = min(sizeof(*lid), sizeof(idev->dev_cmd_regs->data));

	mutex_lock(&ionic->dev_cmd_lock);
	ionic_dev_cmd_lif_identify(idev, lif_type, IONIC_IDENTITY_VERSION_1);
	err = ionic_dev_cmd_wait(ionic, DEVCMD_TIMEOUT);
	memcpy_fromio(lid, &idev->dev_cmd_regs->data, sz);
	mutex_unlock(&ionic->dev_cmd_lock);
	if (err)
		return (err);

	dev_dbg(ionic->dev, "capabilities 0x%llx\n",
		le64_to_cpu(lid->capabilities));

	dev_dbg(ionic->dev, "eth.max_ucast_filters %d\n",
		le32_to_cpu(lid->eth.max_ucast_filters));
	dev_dbg(ionic->dev, "eth.max_mcast_filters %d\n",
		le32_to_cpu(lid->eth.max_mcast_filters));
	dev_dbg(ionic->dev, "eth.features 0x%llx\n",
		le64_to_cpu(lid->eth.config.features));
	dev_dbg(ionic->dev, "eth.queue_count[IONIC_QTYPE_ADMINQ] %d\n",
		le32_to_cpu(lid->eth.config.queue_count[IONIC_QTYPE_ADMINQ]));
	dev_dbg(ionic->dev, "eth.queue_count[IONIC_QTYPE_NOTIFYQ] %d\n",
		le32_to_cpu(lid->eth.config.queue_count[IONIC_QTYPE_NOTIFYQ]));
	dev_dbg(ionic->dev, "eth.queue_count[IONIC_QTYPE_RXQ] %d\n",
		le32_to_cpu(lid->eth.config.queue_count[IONIC_QTYPE_RXQ]));
	dev_dbg(ionic->dev, "eth.queue_count[IONIC_QTYPE_TXQ] %d\n",
		le32_to_cpu(lid->eth.config.queue_count[IONIC_QTYPE_TXQ]));
	dev_dbg(ionic->dev, "eth.config.name %s\n", lid->eth.config.name);
	dev_dbg(ionic->dev, "eth.config.mac %pM\n", lid->eth.config.mac);
	dev_dbg(ionic->dev, "eth.config.mtu %d\n",
		le32_to_cpu(lid->eth.config.mtu));

	return 0;
}

int ionic_lif_size(struct ionic *ionic)
{
	struct ionic_identity *ident = &ionic->ident;
	unsigned int nintrs, dev_nintrs;
	union ionic_lif_config *lc;
	unsigned int ntxqs_per_lif;
	unsigned int nrxqs_per_lif;
	unsigned int neqs_per_lif;
	unsigned int nnqs_per_lif;
	unsigned int nxqs, neqs;
	unsigned int min_intrs;
	int err;

	/* retrieve basic values from FW */
	lc = &ident->lif.eth.config;
	dev_nintrs = le32_to_cpu(ident->dev.nintrs);
	neqs_per_lif = le32_to_cpu(ident->lif.rdma.eq_qtype.qid_count);
	nnqs_per_lif = le32_to_cpu(lc->queue_count[IONIC_QTYPE_NOTIFYQ]);
	ntxqs_per_lif = le32_to_cpu(lc->queue_count[IONIC_QTYPE_TXQ]);
	nrxqs_per_lif = le32_to_cpu(lc->queue_count[IONIC_QTYPE_RXQ]);

	/* limit values to play nice with kdump */
	if (is_kdump_kernel()) {
		dev_nintrs = 2;
		neqs_per_lif = 0;
		nnqs_per_lif = 0;
		ntxqs_per_lif = 1;
		nrxqs_per_lif = 1;
	}

	/* reserve last queue id for hardware timestamping */
	if (lc->features & cpu_to_le64(IONIC_ETH_HW_TIMESTAMP)) {
		if (ntxqs_per_lif <= 1 || nrxqs_per_lif <= 1) {
			lc->features &= cpu_to_le64(~IONIC_ETH_HW_TIMESTAMP);
		} else {
			ntxqs_per_lif -= 1;
			nrxqs_per_lif -= 1;
		}
	}

	nxqs = min(ntxqs_per_lif, nrxqs_per_lif);
	nxqs = min(nxqs, num_online_cpus());
	neqs = min(neqs_per_lif, num_online_cpus());

try_again:
	/* interrupt usage:
	 *    1 for master lif adminq/notifyq
	 *    1 for each CPU for master lif TxRx queue pairs
	 *    whatever's left is for RDMA queues
	 */
	nintrs = 1 + nxqs + neqs;
	min_intrs = 2;  /* adminq + 1 TxRx queue pair */

	if (nintrs > dev_nintrs)
		goto try_fewer;

	err = ionic_bus_alloc_irq_vectors(ionic, nintrs);
	if (err < 0 && err != -ENOSPC) {
		dev_err(ionic->dev, "Can't get intrs from OS: %d\n", err);
		return err;
	}
	if (err == -ENOSPC)
		goto try_fewer;

	if (err != nintrs) {
		ionic_bus_free_irq_vectors(ionic);
		goto try_fewer;
	}

	ionic->nnqs_per_lif = nnqs_per_lif;
	ionic->neqs_per_lif = neqs;
	ionic->ntxqs_per_lif = nxqs;
	ionic->nrxqs_per_lif = nxqs;
	ionic->nintrs = nintrs;

	ionic_debugfs_add_sizes(ionic);

	return 0;

try_fewer:
	if (nnqs_per_lif > 1) {
		nnqs_per_lif >>= 1;
		goto try_again;
	}
	if (neqs > 1) {
		neqs >>= 1;
		goto try_again;
	}
	if (nxqs > 1) {
		nxqs >>= 1;
		goto try_again;
	}
	dev_err(ionic->dev, "Can't get minimum %d intrs from OS\n", min_intrs);
	return -ENOSPC;
}
