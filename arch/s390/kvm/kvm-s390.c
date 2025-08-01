// SPDX-License-Identifier: GPL-2.0
/*
 * hosting IBM Z kernel virtual machines (s390x)
 *
 * Copyright IBM Corp. 2008, 2020
 *
 *    Author(s): Carsten Otte <cotte@de.ibm.com>
 *               Christian Borntraeger <borntraeger@de.ibm.com>
 *               Christian Ehrhardt <ehrhardt@de.ibm.com>
 *               Jason J. Herne <jjherne@us.ibm.com>
 */

#define KMSG_COMPONENT "kvm-s390"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kvm.h>
#include <linux/kvm_host.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/cpufeature.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <linux/bitmap.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/pgtable.h>
#include <linux/mmu_notifier.h>

#include <asm/access-regs.h>
#include <asm/asm-offsets.h>
#include <asm/lowcore.h>
#include <asm/machine.h>
#include <asm/stp.h>
#include <asm/gmap.h>
#include <asm/gmap_helpers.h>
#include <asm/nmi.h>
#include <asm/isc.h>
#include <asm/sclp.h>
#include <asm/cpacf.h>
#include <asm/timex.h>
#include <asm/asm.h>
#include <asm/fpu.h>
#include <asm/ap.h>
#include <asm/uv.h>
#include "kvm-s390.h"
#include "gaccess.h"
#include "pci.h"

#define CREATE_TRACE_POINTS
#include "trace.h"
#include "trace-s390.h"

#define MEM_OP_MAX_SIZE 65536	/* Maximum transfer size for KVM_S390_MEM_OP */
#define LOCAL_IRQS 32
#define VCPU_IRQS_MAX_BUF (sizeof(struct kvm_s390_irq) * \
			   (KVM_MAX_VCPUS + LOCAL_IRQS))

const struct _kvm_stats_desc kvm_vm_stats_desc[] = {
	KVM_GENERIC_VM_STATS(),
	STATS_DESC_COUNTER(VM, inject_io),
	STATS_DESC_COUNTER(VM, inject_float_mchk),
	STATS_DESC_COUNTER(VM, inject_pfault_done),
	STATS_DESC_COUNTER(VM, inject_service_signal),
	STATS_DESC_COUNTER(VM, inject_virtio),
	STATS_DESC_COUNTER(VM, aen_forward),
	STATS_DESC_COUNTER(VM, gmap_shadow_reuse),
	STATS_DESC_COUNTER(VM, gmap_shadow_create),
	STATS_DESC_COUNTER(VM, gmap_shadow_r1_entry),
	STATS_DESC_COUNTER(VM, gmap_shadow_r2_entry),
	STATS_DESC_COUNTER(VM, gmap_shadow_r3_entry),
	STATS_DESC_COUNTER(VM, gmap_shadow_sg_entry),
	STATS_DESC_COUNTER(VM, gmap_shadow_pg_entry),
};

const struct kvm_stats_header kvm_vm_stats_header = {
	.name_size = KVM_STATS_NAME_SIZE,
	.num_desc = ARRAY_SIZE(kvm_vm_stats_desc),
	.id_offset = sizeof(struct kvm_stats_header),
	.desc_offset = sizeof(struct kvm_stats_header) + KVM_STATS_NAME_SIZE,
	.data_offset = sizeof(struct kvm_stats_header) + KVM_STATS_NAME_SIZE +
		       sizeof(kvm_vm_stats_desc),
};

const struct _kvm_stats_desc kvm_vcpu_stats_desc[] = {
	KVM_GENERIC_VCPU_STATS(),
	STATS_DESC_COUNTER(VCPU, exit_userspace),
	STATS_DESC_COUNTER(VCPU, exit_null),
	STATS_DESC_COUNTER(VCPU, exit_external_request),
	STATS_DESC_COUNTER(VCPU, exit_io_request),
	STATS_DESC_COUNTER(VCPU, exit_external_interrupt),
	STATS_DESC_COUNTER(VCPU, exit_stop_request),
	STATS_DESC_COUNTER(VCPU, exit_validity),
	STATS_DESC_COUNTER(VCPU, exit_instruction),
	STATS_DESC_COUNTER(VCPU, exit_pei),
	STATS_DESC_COUNTER(VCPU, halt_no_poll_steal),
	STATS_DESC_COUNTER(VCPU, instruction_lctl),
	STATS_DESC_COUNTER(VCPU, instruction_lctlg),
	STATS_DESC_COUNTER(VCPU, instruction_stctl),
	STATS_DESC_COUNTER(VCPU, instruction_stctg),
	STATS_DESC_COUNTER(VCPU, exit_program_interruption),
	STATS_DESC_COUNTER(VCPU, exit_instr_and_program),
	STATS_DESC_COUNTER(VCPU, exit_operation_exception),
	STATS_DESC_COUNTER(VCPU, deliver_ckc),
	STATS_DESC_COUNTER(VCPU, deliver_cputm),
	STATS_DESC_COUNTER(VCPU, deliver_external_call),
	STATS_DESC_COUNTER(VCPU, deliver_emergency_signal),
	STATS_DESC_COUNTER(VCPU, deliver_service_signal),
	STATS_DESC_COUNTER(VCPU, deliver_virtio),
	STATS_DESC_COUNTER(VCPU, deliver_stop_signal),
	STATS_DESC_COUNTER(VCPU, deliver_prefix_signal),
	STATS_DESC_COUNTER(VCPU, deliver_restart_signal),
	STATS_DESC_COUNTER(VCPU, deliver_program),
	STATS_DESC_COUNTER(VCPU, deliver_io),
	STATS_DESC_COUNTER(VCPU, deliver_machine_check),
	STATS_DESC_COUNTER(VCPU, exit_wait_state),
	STATS_DESC_COUNTER(VCPU, inject_ckc),
	STATS_DESC_COUNTER(VCPU, inject_cputm),
	STATS_DESC_COUNTER(VCPU, inject_external_call),
	STATS_DESC_COUNTER(VCPU, inject_emergency_signal),
	STATS_DESC_COUNTER(VCPU, inject_mchk),
	STATS_DESC_COUNTER(VCPU, inject_pfault_init),
	STATS_DESC_COUNTER(VCPU, inject_program),
	STATS_DESC_COUNTER(VCPU, inject_restart),
	STATS_DESC_COUNTER(VCPU, inject_set_prefix),
	STATS_DESC_COUNTER(VCPU, inject_stop_signal),
	STATS_DESC_COUNTER(VCPU, instruction_epsw),
	STATS_DESC_COUNTER(VCPU, instruction_gs),
	STATS_DESC_COUNTER(VCPU, instruction_io_other),
	STATS_DESC_COUNTER(VCPU, instruction_lpsw),
	STATS_DESC_COUNTER(VCPU, instruction_lpswe),
	STATS_DESC_COUNTER(VCPU, instruction_lpswey),
	STATS_DESC_COUNTER(VCPU, instruction_pfmf),
	STATS_DESC_COUNTER(VCPU, instruction_ptff),
	STATS_DESC_COUNTER(VCPU, instruction_sck),
	STATS_DESC_COUNTER(VCPU, instruction_sckpf),
	STATS_DESC_COUNTER(VCPU, instruction_stidp),
	STATS_DESC_COUNTER(VCPU, instruction_spx),
	STATS_DESC_COUNTER(VCPU, instruction_stpx),
	STATS_DESC_COUNTER(VCPU, instruction_stap),
	STATS_DESC_COUNTER(VCPU, instruction_iske),
	STATS_DESC_COUNTER(VCPU, instruction_ri),
	STATS_DESC_COUNTER(VCPU, instruction_rrbe),
	STATS_DESC_COUNTER(VCPU, instruction_sske),
	STATS_DESC_COUNTER(VCPU, instruction_ipte_interlock),
	STATS_DESC_COUNTER(VCPU, instruction_stsi),
	STATS_DESC_COUNTER(VCPU, instruction_stfl),
	STATS_DESC_COUNTER(VCPU, instruction_tb),
	STATS_DESC_COUNTER(VCPU, instruction_tpi),
	STATS_DESC_COUNTER(VCPU, instruction_tprot),
	STATS_DESC_COUNTER(VCPU, instruction_tsch),
	STATS_DESC_COUNTER(VCPU, instruction_sie),
	STATS_DESC_COUNTER(VCPU, instruction_essa),
	STATS_DESC_COUNTER(VCPU, instruction_sthyi),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_sense),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_sense_running),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_external_call),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_emergency),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_cond_emergency),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_start),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_stop),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_stop_store_status),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_store_status),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_store_adtl_status),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_arch),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_prefix),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_restart),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_init_cpu_reset),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_cpu_reset),
	STATS_DESC_COUNTER(VCPU, instruction_sigp_unknown),
	STATS_DESC_COUNTER(VCPU, instruction_diagnose_10),
	STATS_DESC_COUNTER(VCPU, instruction_diagnose_44),
	STATS_DESC_COUNTER(VCPU, instruction_diagnose_9c),
	STATS_DESC_COUNTER(VCPU, diag_9c_ignored),
	STATS_DESC_COUNTER(VCPU, diag_9c_forward),
	STATS_DESC_COUNTER(VCPU, instruction_diagnose_258),
	STATS_DESC_COUNTER(VCPU, instruction_diagnose_308),
	STATS_DESC_COUNTER(VCPU, instruction_diagnose_500),
	STATS_DESC_COUNTER(VCPU, instruction_diagnose_other),
	STATS_DESC_COUNTER(VCPU, pfault_sync)
};

const struct kvm_stats_header kvm_vcpu_stats_header = {
	.name_size = KVM_STATS_NAME_SIZE,
	.num_desc = ARRAY_SIZE(kvm_vcpu_stats_desc),
	.id_offset = sizeof(struct kvm_stats_header),
	.desc_offset = sizeof(struct kvm_stats_header) + KVM_STATS_NAME_SIZE,
	.data_offset = sizeof(struct kvm_stats_header) + KVM_STATS_NAME_SIZE +
		       sizeof(kvm_vcpu_stats_desc),
};

/* allow nested virtualization in KVM (if enabled by user space) */
static int nested;
module_param(nested, int, S_IRUGO);
MODULE_PARM_DESC(nested, "Nested virtualization support");

/* allow 1m huge page guest backing, if !nested */
static int hpage;
module_param(hpage, int, 0444);
MODULE_PARM_DESC(hpage, "1m huge page backing support");

/* maximum percentage of steal time for polling.  >100 is treated like 100 */
static u8 halt_poll_max_steal = 10;
module_param(halt_poll_max_steal, byte, 0644);
MODULE_PARM_DESC(halt_poll_max_steal, "Maximum percentage of steal time to allow polling");

/* if set to true, the GISA will be initialized and used if available */
static bool use_gisa  = true;
module_param(use_gisa, bool, 0644);
MODULE_PARM_DESC(use_gisa, "Use the GISA if the host supports it.");

/* maximum diag9c forwarding per second */
unsigned int diag9c_forwarding_hz;
module_param(diag9c_forwarding_hz, uint, 0644);
MODULE_PARM_DESC(diag9c_forwarding_hz, "Maximum diag9c forwarding per second, 0 to turn off");

/*
 * allow asynchronous deinit for protected guests; enable by default since
 * the feature is opt-in anyway
 */
static int async_destroy = 1;
module_param(async_destroy, int, 0444);
MODULE_PARM_DESC(async_destroy, "Asynchronous destroy for protected guests");

/*
 * For now we handle at most 16 double words as this is what the s390 base
 * kernel handles and stores in the prefix page. If we ever need to go beyond
 * this, this requires changes to code, but the external uapi can stay.
 */
#define SIZE_INTERNAL 16

/*
 * Base feature mask that defines default mask for facilities. Consists of the
 * defines in FACILITIES_KVM and the non-hypervisor managed bits.
 */
static unsigned long kvm_s390_fac_base[SIZE_INTERNAL] = { FACILITIES_KVM };
/*
 * Extended feature mask. Consists of the defines in FACILITIES_KVM_CPUMODEL
 * and defines the facilities that can be enabled via a cpu model.
 */
static unsigned long kvm_s390_fac_ext[SIZE_INTERNAL] = { FACILITIES_KVM_CPUMODEL };

static unsigned long kvm_s390_fac_size(void)
{
	BUILD_BUG_ON(SIZE_INTERNAL > S390_ARCH_FAC_MASK_SIZE_U64);
	BUILD_BUG_ON(SIZE_INTERNAL > S390_ARCH_FAC_LIST_SIZE_U64);
	BUILD_BUG_ON(SIZE_INTERNAL * sizeof(unsigned long) >
		sizeof(stfle_fac_list));

	return SIZE_INTERNAL;
}

/* available cpu features supported by kvm */
static DECLARE_BITMAP(kvm_s390_available_cpu_feat, KVM_S390_VM_CPU_FEAT_NR_BITS);
/* available subfunctions indicated via query / "test bit" */
static struct kvm_s390_vm_cpu_subfunc kvm_s390_available_subfunc;

static struct gmap_notifier gmap_notifier;
static struct gmap_notifier vsie_gmap_notifier;
debug_info_t *kvm_s390_dbf;
debug_info_t *kvm_s390_dbf_uv;

/* Section: not file related */
/* forward declarations */
static void kvm_gmap_notifier(struct gmap *gmap, unsigned long start,
			      unsigned long end);
static int sca_switch_to_extended(struct kvm *kvm);

static void kvm_clock_sync_scb(struct kvm_s390_sie_block *scb, u64 delta)
{
	u8 delta_idx = 0;

	/*
	 * The TOD jumps by delta, we have to compensate this by adding
	 * -delta to the epoch.
	 */
	delta = -delta;

	/* sign-extension - we're adding to signed values below */
	if ((s64)delta < 0)
		delta_idx = -1;

	scb->epoch += delta;
	if (scb->ecd & ECD_MEF) {
		scb->epdx += delta_idx;
		if (scb->epoch < delta)
			scb->epdx += 1;
	}
}

/*
 * This callback is executed during stop_machine(). All CPUs are therefore
 * temporarily stopped. In order not to change guest behavior, we have to
 * disable preemption whenever we touch the epoch of kvm and the VCPUs,
 * so a CPU won't be stopped while calculating with the epoch.
 */
static int kvm_clock_sync(struct notifier_block *notifier, unsigned long val,
			  void *v)
{
	struct kvm *kvm;
	struct kvm_vcpu *vcpu;
	unsigned long i;
	unsigned long long *delta = v;

	list_for_each_entry(kvm, &vm_list, vm_list) {
		kvm_for_each_vcpu(i, vcpu, kvm) {
			kvm_clock_sync_scb(vcpu->arch.sie_block, *delta);
			if (i == 0) {
				kvm->arch.epoch = vcpu->arch.sie_block->epoch;
				kvm->arch.epdx = vcpu->arch.sie_block->epdx;
			}
			if (vcpu->arch.cputm_enabled)
				vcpu->arch.cputm_start += *delta;
			if (vcpu->arch.vsie_block)
				kvm_clock_sync_scb(vcpu->arch.vsie_block,
						   *delta);
		}
	}
	return NOTIFY_OK;
}

static struct notifier_block kvm_clock_notifier = {
	.notifier_call = kvm_clock_sync,
};

static void allow_cpu_feat(unsigned long nr)
{
	set_bit_inv(nr, kvm_s390_available_cpu_feat);
}

static inline int plo_test_bit(unsigned char nr)
{
	unsigned long function = (unsigned long)nr | 0x100;
	int cc;

	asm volatile(
		"	lgr	0,%[function]\n"
		/* Parameter registers are ignored for "test bit" */
		"	plo	0,0,0,0(0)\n"
		CC_IPM(cc)
		: CC_OUT(cc, cc)
		: [function] "d" (function)
		: CC_CLOBBER_LIST("0"));
	return CC_TRANSFORM(cc) == 0;
}

static __always_inline void pfcr_query(u8 (*query)[16])
{
	asm volatile(
		"	lghi	0,0\n"
		"	.insn   rsy,0xeb0000000016,0,0,%[query]\n"
		: [query] "=QS" (*query)
		:
		: "cc", "0");
}

static __always_inline void __sortl_query(u8 (*query)[32])
{
	asm volatile(
		"	lghi	0,0\n"
		"	la	1,%[query]\n"
		/* Parameter registers are ignored */
		"	.insn	rre,0xb9380000,2,4\n"
		: [query] "=R" (*query)
		:
		: "cc", "0", "1");
}

static __always_inline void __dfltcc_query(u8 (*query)[32])
{
	asm volatile(
		"	lghi	0,0\n"
		"	la	1,%[query]\n"
		/* Parameter registers are ignored */
		"	.insn	rrf,0xb9390000,2,4,6,0\n"
		: [query] "=R" (*query)
		:
		: "cc", "0", "1");
}

static void __init kvm_s390_cpu_feat_init(void)
{
	int i;

	for (i = 0; i < 256; ++i) {
		if (plo_test_bit(i))
			kvm_s390_available_subfunc.plo[i >> 3] |= 0x80 >> (i & 7);
	}

	if (test_facility(28)) /* TOD-clock steering */
		ptff(kvm_s390_available_subfunc.ptff,
		     sizeof(kvm_s390_available_subfunc.ptff),
		     PTFF_QAF);

	if (test_facility(17)) { /* MSA */
		__cpacf_query(CPACF_KMAC, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.kmac);
		__cpacf_query(CPACF_KMC, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.kmc);
		__cpacf_query(CPACF_KM, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.km);
		__cpacf_query(CPACF_KIMD, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.kimd);
		__cpacf_query(CPACF_KLMD, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.klmd);
	}
	if (test_facility(76)) /* MSA3 */
		__cpacf_query(CPACF_PCKMO, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.pckmo);
	if (test_facility(77)) { /* MSA4 */
		__cpacf_query(CPACF_KMCTR, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.kmctr);
		__cpacf_query(CPACF_KMF, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.kmf);
		__cpacf_query(CPACF_KMO, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.kmo);
		__cpacf_query(CPACF_PCC, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.pcc);
	}
	if (test_facility(57)) /* MSA5 */
		__cpacf_query(CPACF_PRNO, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.ppno);

	if (test_facility(146)) /* MSA8 */
		__cpacf_query(CPACF_KMA, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.kma);

	if (test_facility(155)) /* MSA9 */
		__cpacf_query(CPACF_KDSA, (cpacf_mask_t *)
			      kvm_s390_available_subfunc.kdsa);

	if (test_facility(150)) /* SORTL */
		__sortl_query(&kvm_s390_available_subfunc.sortl);

	if (test_facility(151)) /* DFLTCC */
		__dfltcc_query(&kvm_s390_available_subfunc.dfltcc);

	if (test_facility(201))	/* PFCR */
		pfcr_query(&kvm_s390_available_subfunc.pfcr);

	if (machine_has_esop())
		allow_cpu_feat(KVM_S390_VM_CPU_FEAT_ESOP);
	/*
	 * We need SIE support, ESOP (PROT_READ protection for gmap_shadow),
	 * 64bit SCAO (SCA passthrough) and IDTE (for gmap_shadow unshadowing).
	 */
	if (!sclp.has_sief2 || !machine_has_esop() || !sclp.has_64bscao ||
	    !test_facility(3) || !nested)
		return;
	allow_cpu_feat(KVM_S390_VM_CPU_FEAT_SIEF2);
	if (sclp.has_64bscao)
		allow_cpu_feat(KVM_S390_VM_CPU_FEAT_64BSCAO);
	if (sclp.has_siif)
		allow_cpu_feat(KVM_S390_VM_CPU_FEAT_SIIF);
	if (sclp.has_gpere)
		allow_cpu_feat(KVM_S390_VM_CPU_FEAT_GPERE);
	if (sclp.has_gsls)
		allow_cpu_feat(KVM_S390_VM_CPU_FEAT_GSLS);
	if (sclp.has_ib)
		allow_cpu_feat(KVM_S390_VM_CPU_FEAT_IB);
	if (sclp.has_cei)
		allow_cpu_feat(KVM_S390_VM_CPU_FEAT_CEI);
	if (sclp.has_ibs)
		allow_cpu_feat(KVM_S390_VM_CPU_FEAT_IBS);
	if (sclp.has_kss)
		allow_cpu_feat(KVM_S390_VM_CPU_FEAT_KSS);
	/*
	 * KVM_S390_VM_CPU_FEAT_SKEY: Wrong shadow of PTE.I bits will make
	 * all skey handling functions read/set the skey from the PGSTE
	 * instead of the real storage key.
	 *
	 * KVM_S390_VM_CPU_FEAT_CMMA: Wrong shadow of PTE.I bits will make
	 * pages being detected as preserved although they are resident.
	 *
	 * KVM_S390_VM_CPU_FEAT_PFMFI: Wrong shadow of PTE.I bits will
	 * have the same effect as for KVM_S390_VM_CPU_FEAT_SKEY.
	 *
	 * For KVM_S390_VM_CPU_FEAT_SKEY, KVM_S390_VM_CPU_FEAT_CMMA and
	 * KVM_S390_VM_CPU_FEAT_PFMFI, all PTE.I and PGSTE bits have to be
	 * correctly shadowed. We can do that for the PGSTE but not for PTE.I.
	 *
	 * KVM_S390_VM_CPU_FEAT_SIGPIF: Wrong SCB addresses in the SCA. We
	 * cannot easily shadow the SCA because of the ipte lock.
	 */
}

static int __init __kvm_s390_init(void)
{
	int rc = -ENOMEM;

	kvm_s390_dbf = debug_register("kvm-trace", 32, 1, 7 * sizeof(long));
	if (!kvm_s390_dbf)
		return -ENOMEM;

	kvm_s390_dbf_uv = debug_register("kvm-uv", 32, 1, 7 * sizeof(long));
	if (!kvm_s390_dbf_uv)
		goto err_kvm_uv;

	if (debug_register_view(kvm_s390_dbf, &debug_sprintf_view) ||
	    debug_register_view(kvm_s390_dbf_uv, &debug_sprintf_view))
		goto err_debug_view;

	kvm_s390_cpu_feat_init();

	/* Register floating interrupt controller interface. */
	rc = kvm_register_device_ops(&kvm_flic_ops, KVM_DEV_TYPE_FLIC);
	if (rc) {
		pr_err("A FLIC registration call failed with rc=%d\n", rc);
		goto err_flic;
	}

	if (IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM)) {
		rc = kvm_s390_pci_init();
		if (rc) {
			pr_err("Unable to allocate AIFT for PCI\n");
			goto err_pci;
		}
	}

	rc = kvm_s390_gib_init(GAL_ISC);
	if (rc)
		goto err_gib;

	gmap_notifier.notifier_call = kvm_gmap_notifier;
	gmap_register_pte_notifier(&gmap_notifier);
	vsie_gmap_notifier.notifier_call = kvm_s390_vsie_gmap_notifier;
	gmap_register_pte_notifier(&vsie_gmap_notifier);
	atomic_notifier_chain_register(&s390_epoch_delta_notifier,
				       &kvm_clock_notifier);

	return 0;

err_gib:
	if (IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM))
		kvm_s390_pci_exit();
err_pci:
err_flic:
err_debug_view:
	debug_unregister(kvm_s390_dbf_uv);
err_kvm_uv:
	debug_unregister(kvm_s390_dbf);
	return rc;
}

static void __kvm_s390_exit(void)
{
	gmap_unregister_pte_notifier(&gmap_notifier);
	gmap_unregister_pte_notifier(&vsie_gmap_notifier);
	atomic_notifier_chain_unregister(&s390_epoch_delta_notifier,
					 &kvm_clock_notifier);

	kvm_s390_gib_destroy();
	if (IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM))
		kvm_s390_pci_exit();
	debug_unregister(kvm_s390_dbf);
	debug_unregister(kvm_s390_dbf_uv);
}

/* Section: device related */
long kvm_arch_dev_ioctl(struct file *filp,
			unsigned int ioctl, unsigned long arg)
{
	if (ioctl == KVM_S390_ENABLE_SIE)
		return s390_enable_sie();
	return -EINVAL;
}

int kvm_vm_ioctl_check_extension(struct kvm *kvm, long ext)
{
	int r;

	switch (ext) {
	case KVM_CAP_S390_PSW:
	case KVM_CAP_S390_GMAP:
	case KVM_CAP_SYNC_MMU:
#ifdef CONFIG_KVM_S390_UCONTROL
	case KVM_CAP_S390_UCONTROL:
#endif
	case KVM_CAP_ASYNC_PF:
	case KVM_CAP_SYNC_REGS:
	case KVM_CAP_ONE_REG:
	case KVM_CAP_ENABLE_CAP:
	case KVM_CAP_S390_CSS_SUPPORT:
	case KVM_CAP_IOEVENTFD:
	case KVM_CAP_S390_IRQCHIP:
	case KVM_CAP_VM_ATTRIBUTES:
	case KVM_CAP_MP_STATE:
	case KVM_CAP_IMMEDIATE_EXIT:
	case KVM_CAP_S390_INJECT_IRQ:
	case KVM_CAP_S390_USER_SIGP:
	case KVM_CAP_S390_USER_STSI:
	case KVM_CAP_S390_SKEYS:
	case KVM_CAP_S390_IRQ_STATE:
	case KVM_CAP_S390_USER_INSTR0:
	case KVM_CAP_S390_CMMA_MIGRATION:
	case KVM_CAP_S390_AIS:
	case KVM_CAP_S390_AIS_MIGRATION:
	case KVM_CAP_S390_VCPU_RESETS:
	case KVM_CAP_SET_GUEST_DEBUG:
	case KVM_CAP_S390_DIAG318:
	case KVM_CAP_IRQFD_RESAMPLE:
		r = 1;
		break;
	case KVM_CAP_SET_GUEST_DEBUG2:
		r = KVM_GUESTDBG_VALID_MASK;
		break;
	case KVM_CAP_S390_HPAGE_1M:
		r = 0;
		if (hpage && !(kvm && kvm_is_ucontrol(kvm)))
			r = 1;
		break;
	case KVM_CAP_S390_MEM_OP:
		r = MEM_OP_MAX_SIZE;
		break;
	case KVM_CAP_S390_MEM_OP_EXTENSION:
		/*
		 * Flag bits indicating which extensions are supported.
		 * If r > 0, the base extension must also be supported/indicated,
		 * in order to maintain backwards compatibility.
		 */
		r = KVM_S390_MEMOP_EXTENSION_CAP_BASE |
		    KVM_S390_MEMOP_EXTENSION_CAP_CMPXCHG;
		break;
	case KVM_CAP_NR_VCPUS:
	case KVM_CAP_MAX_VCPUS:
	case KVM_CAP_MAX_VCPU_ID:
		r = KVM_S390_BSCA_CPU_SLOTS;
		if (!kvm_s390_use_sca_entries())
			r = KVM_MAX_VCPUS;
		else if (sclp.has_esca && sclp.has_64bscao)
			r = KVM_S390_ESCA_CPU_SLOTS;
		if (ext == KVM_CAP_NR_VCPUS)
			r = min_t(unsigned int, num_online_cpus(), r);
		break;
	case KVM_CAP_S390_COW:
		r = machine_has_esop();
		break;
	case KVM_CAP_S390_VECTOR_REGISTERS:
		r = test_facility(129);
		break;
	case KVM_CAP_S390_RI:
		r = test_facility(64);
		break;
	case KVM_CAP_S390_GS:
		r = test_facility(133);
		break;
	case KVM_CAP_S390_BPB:
		r = test_facility(82);
		break;
	case KVM_CAP_S390_PROTECTED_ASYNC_DISABLE:
		r = async_destroy && is_prot_virt_host();
		break;
	case KVM_CAP_S390_PROTECTED:
		r = is_prot_virt_host();
		break;
	case KVM_CAP_S390_PROTECTED_DUMP: {
		u64 pv_cmds_dump[] = {
			BIT_UVC_CMD_DUMP_INIT,
			BIT_UVC_CMD_DUMP_CONFIG_STOR_STATE,
			BIT_UVC_CMD_DUMP_CPU,
			BIT_UVC_CMD_DUMP_COMPLETE,
		};
		int i;

		r = is_prot_virt_host();

		for (i = 0; i < ARRAY_SIZE(pv_cmds_dump); i++) {
			if (!test_bit_inv(pv_cmds_dump[i],
					  (unsigned long *)&uv_info.inst_calls_list)) {
				r = 0;
				break;
			}
		}
		break;
	}
	case KVM_CAP_S390_ZPCI_OP:
		r = kvm_s390_pci_interp_allowed();
		break;
	case KVM_CAP_S390_CPU_TOPOLOGY:
		r = test_facility(11);
		break;
	default:
		r = 0;
	}
	return r;
}

void kvm_arch_sync_dirty_log(struct kvm *kvm, struct kvm_memory_slot *memslot)
{
	int i;
	gfn_t cur_gfn, last_gfn;
	unsigned long gaddr, vmaddr;
	struct gmap *gmap = kvm->arch.gmap;
	DECLARE_BITMAP(bitmap, _PAGE_ENTRIES);

	/* Loop over all guest segments */
	cur_gfn = memslot->base_gfn;
	last_gfn = memslot->base_gfn + memslot->npages;
	for (; cur_gfn <= last_gfn; cur_gfn += _PAGE_ENTRIES) {
		gaddr = gfn_to_gpa(cur_gfn);
		vmaddr = gfn_to_hva_memslot(memslot, cur_gfn);
		if (kvm_is_error_hva(vmaddr))
			continue;

		bitmap_zero(bitmap, _PAGE_ENTRIES);
		gmap_sync_dirty_log_pmd(gmap, bitmap, gaddr, vmaddr);
		for (i = 0; i < _PAGE_ENTRIES; i++) {
			if (test_bit(i, bitmap))
				mark_page_dirty(kvm, cur_gfn + i);
		}

		if (fatal_signal_pending(current))
			return;
		cond_resched();
	}
}

/* Section: vm related */
static void sca_del_vcpu(struct kvm_vcpu *vcpu);

/*
 * Get (and clear) the dirty memory log for a memory slot.
 */
int kvm_vm_ioctl_get_dirty_log(struct kvm *kvm,
			       struct kvm_dirty_log *log)
{
	int r;
	unsigned long n;
	struct kvm_memory_slot *memslot;
	int is_dirty;

	if (kvm_is_ucontrol(kvm))
		return -EINVAL;

	mutex_lock(&kvm->slots_lock);

	r = -EINVAL;
	if (log->slot >= KVM_USER_MEM_SLOTS)
		goto out;

	r = kvm_get_dirty_log(kvm, log, &is_dirty, &memslot);
	if (r)
		goto out;

	/* Clear the dirty log */
	if (is_dirty) {
		n = kvm_dirty_bitmap_bytes(memslot);
		memset(memslot->dirty_bitmap, 0, n);
	}
	r = 0;
out:
	mutex_unlock(&kvm->slots_lock);
	return r;
}

static void icpt_operexc_on_all_vcpus(struct kvm *kvm)
{
	unsigned long i;
	struct kvm_vcpu *vcpu;

	kvm_for_each_vcpu(i, vcpu, kvm) {
		kvm_s390_sync_request(KVM_REQ_ICPT_OPEREXC, vcpu);
	}
}

int kvm_vm_ioctl_enable_cap(struct kvm *kvm, struct kvm_enable_cap *cap)
{
	int r;

	if (cap->flags)
		return -EINVAL;

	switch (cap->cap) {
	case KVM_CAP_S390_IRQCHIP:
		VM_EVENT(kvm, 3, "%s", "ENABLE: CAP_S390_IRQCHIP");
		kvm->arch.use_irqchip = 1;
		r = 0;
		break;
	case KVM_CAP_S390_USER_SIGP:
		VM_EVENT(kvm, 3, "%s", "ENABLE: CAP_S390_USER_SIGP");
		kvm->arch.user_sigp = 1;
		r = 0;
		break;
	case KVM_CAP_S390_VECTOR_REGISTERS:
		mutex_lock(&kvm->lock);
		if (kvm->created_vcpus) {
			r = -EBUSY;
		} else if (cpu_has_vx()) {
			set_kvm_facility(kvm->arch.model.fac_mask, 129);
			set_kvm_facility(kvm->arch.model.fac_list, 129);
			if (test_facility(134)) {
				set_kvm_facility(kvm->arch.model.fac_mask, 134);
				set_kvm_facility(kvm->arch.model.fac_list, 134);
			}
			if (test_facility(135)) {
				set_kvm_facility(kvm->arch.model.fac_mask, 135);
				set_kvm_facility(kvm->arch.model.fac_list, 135);
			}
			if (test_facility(148)) {
				set_kvm_facility(kvm->arch.model.fac_mask, 148);
				set_kvm_facility(kvm->arch.model.fac_list, 148);
			}
			if (test_facility(152)) {
				set_kvm_facility(kvm->arch.model.fac_mask, 152);
				set_kvm_facility(kvm->arch.model.fac_list, 152);
			}
			if (test_facility(192)) {
				set_kvm_facility(kvm->arch.model.fac_mask, 192);
				set_kvm_facility(kvm->arch.model.fac_list, 192);
			}
			if (test_facility(198)) {
				set_kvm_facility(kvm->arch.model.fac_mask, 198);
				set_kvm_facility(kvm->arch.model.fac_list, 198);
			}
			if (test_facility(199)) {
				set_kvm_facility(kvm->arch.model.fac_mask, 199);
				set_kvm_facility(kvm->arch.model.fac_list, 199);
			}
			r = 0;
		} else
			r = -EINVAL;
		mutex_unlock(&kvm->lock);
		VM_EVENT(kvm, 3, "ENABLE: CAP_S390_VECTOR_REGISTERS %s",
			 r ? "(not available)" : "(success)");
		break;
	case KVM_CAP_S390_RI:
		r = -EINVAL;
		mutex_lock(&kvm->lock);
		if (kvm->created_vcpus) {
			r = -EBUSY;
		} else if (test_facility(64)) {
			set_kvm_facility(kvm->arch.model.fac_mask, 64);
			set_kvm_facility(kvm->arch.model.fac_list, 64);
			r = 0;
		}
		mutex_unlock(&kvm->lock);
		VM_EVENT(kvm, 3, "ENABLE: CAP_S390_RI %s",
			 r ? "(not available)" : "(success)");
		break;
	case KVM_CAP_S390_AIS:
		mutex_lock(&kvm->lock);
		if (kvm->created_vcpus) {
			r = -EBUSY;
		} else {
			set_kvm_facility(kvm->arch.model.fac_mask, 72);
			set_kvm_facility(kvm->arch.model.fac_list, 72);
			r = 0;
		}
		mutex_unlock(&kvm->lock);
		VM_EVENT(kvm, 3, "ENABLE: AIS %s",
			 r ? "(not available)" : "(success)");
		break;
	case KVM_CAP_S390_GS:
		r = -EINVAL;
		mutex_lock(&kvm->lock);
		if (kvm->created_vcpus) {
			r = -EBUSY;
		} else if (test_facility(133)) {
			set_kvm_facility(kvm->arch.model.fac_mask, 133);
			set_kvm_facility(kvm->arch.model.fac_list, 133);
			r = 0;
		}
		mutex_unlock(&kvm->lock);
		VM_EVENT(kvm, 3, "ENABLE: CAP_S390_GS %s",
			 r ? "(not available)" : "(success)");
		break;
	case KVM_CAP_S390_HPAGE_1M:
		mutex_lock(&kvm->lock);
		if (kvm->created_vcpus)
			r = -EBUSY;
		else if (!hpage || kvm->arch.use_cmma || kvm_is_ucontrol(kvm))
			r = -EINVAL;
		else {
			r = 0;
			mmap_write_lock(kvm->mm);
			kvm->mm->context.allow_gmap_hpage_1m = 1;
			mmap_write_unlock(kvm->mm);
			/*
			 * We might have to create fake 4k page
			 * tables. To avoid that the hardware works on
			 * stale PGSTEs, we emulate these instructions.
			 */
			kvm->arch.use_skf = 0;
			kvm->arch.use_pfmfi = 0;
		}
		mutex_unlock(&kvm->lock);
		VM_EVENT(kvm, 3, "ENABLE: CAP_S390_HPAGE %s",
			 r ? "(not available)" : "(success)");
		break;
	case KVM_CAP_S390_USER_STSI:
		VM_EVENT(kvm, 3, "%s", "ENABLE: CAP_S390_USER_STSI");
		kvm->arch.user_stsi = 1;
		r = 0;
		break;
	case KVM_CAP_S390_USER_INSTR0:
		VM_EVENT(kvm, 3, "%s", "ENABLE: CAP_S390_USER_INSTR0");
		kvm->arch.user_instr0 = 1;
		icpt_operexc_on_all_vcpus(kvm);
		r = 0;
		break;
	case KVM_CAP_S390_CPU_TOPOLOGY:
		r = -EINVAL;
		mutex_lock(&kvm->lock);
		if (kvm->created_vcpus) {
			r = -EBUSY;
		} else if (test_facility(11)) {
			set_kvm_facility(kvm->arch.model.fac_mask, 11);
			set_kvm_facility(kvm->arch.model.fac_list, 11);
			r = 0;
		}
		mutex_unlock(&kvm->lock);
		VM_EVENT(kvm, 3, "ENABLE: CAP_S390_CPU_TOPOLOGY %s",
			 r ? "(not available)" : "(success)");
		break;
	default:
		r = -EINVAL;
		break;
	}
	return r;
}

static int kvm_s390_get_mem_control(struct kvm *kvm, struct kvm_device_attr *attr)
{
	int ret;

	switch (attr->attr) {
	case KVM_S390_VM_MEM_LIMIT_SIZE:
		ret = 0;
		VM_EVENT(kvm, 3, "QUERY: max guest memory: %lu bytes",
			 kvm->arch.mem_limit);
		if (put_user(kvm->arch.mem_limit, (u64 __user *)attr->addr))
			ret = -EFAULT;
		break;
	default:
		ret = -ENXIO;
		break;
	}
	return ret;
}

static int kvm_s390_set_mem_control(struct kvm *kvm, struct kvm_device_attr *attr)
{
	int ret;
	unsigned int idx;
	switch (attr->attr) {
	case KVM_S390_VM_MEM_ENABLE_CMMA:
		ret = -ENXIO;
		if (!sclp.has_cmma)
			break;

		VM_EVENT(kvm, 3, "%s", "ENABLE: CMMA support");
		mutex_lock(&kvm->lock);
		if (kvm->created_vcpus)
			ret = -EBUSY;
		else if (kvm->mm->context.allow_gmap_hpage_1m)
			ret = -EINVAL;
		else {
			kvm->arch.use_cmma = 1;
			/* Not compatible with cmma. */
			kvm->arch.use_pfmfi = 0;
			ret = 0;
		}
		mutex_unlock(&kvm->lock);
		break;
	case KVM_S390_VM_MEM_CLR_CMMA:
		ret = -ENXIO;
		if (!sclp.has_cmma)
			break;
		ret = -EINVAL;
		if (!kvm->arch.use_cmma)
			break;

		VM_EVENT(kvm, 3, "%s", "RESET: CMMA states");
		mutex_lock(&kvm->lock);
		idx = srcu_read_lock(&kvm->srcu);
		s390_reset_cmma(kvm->arch.gmap->mm);
		srcu_read_unlock(&kvm->srcu, idx);
		mutex_unlock(&kvm->lock);
		ret = 0;
		break;
	case KVM_S390_VM_MEM_LIMIT_SIZE: {
		unsigned long new_limit;

		if (kvm_is_ucontrol(kvm))
			return -EINVAL;

		if (get_user(new_limit, (u64 __user *)attr->addr))
			return -EFAULT;

		if (kvm->arch.mem_limit != KVM_S390_NO_MEM_LIMIT &&
		    new_limit > kvm->arch.mem_limit)
			return -E2BIG;

		if (!new_limit)
			return -EINVAL;

		/* gmap_create takes last usable address */
		if (new_limit != KVM_S390_NO_MEM_LIMIT)
			new_limit -= 1;

		ret = -EBUSY;
		mutex_lock(&kvm->lock);
		if (!kvm->created_vcpus) {
			/* gmap_create will round the limit up */
			struct gmap *new = gmap_create(current->mm, new_limit);

			if (!new) {
				ret = -ENOMEM;
			} else {
				gmap_remove(kvm->arch.gmap);
				new->private = kvm;
				kvm->arch.gmap = new;
				ret = 0;
			}
		}
		mutex_unlock(&kvm->lock);
		VM_EVENT(kvm, 3, "SET: max guest address: %lu", new_limit);
		VM_EVENT(kvm, 3, "New guest asce: 0x%p",
			 (void *) kvm->arch.gmap->asce);
		break;
	}
	default:
		ret = -ENXIO;
		break;
	}
	return ret;
}

static void kvm_s390_vcpu_crypto_setup(struct kvm_vcpu *vcpu);

void kvm_s390_vcpu_crypto_reset_all(struct kvm *kvm)
{
	struct kvm_vcpu *vcpu;
	unsigned long i;

	kvm_s390_vcpu_block_all(kvm);

	kvm_for_each_vcpu(i, vcpu, kvm) {
		kvm_s390_vcpu_crypto_setup(vcpu);
		/* recreate the shadow crycb by leaving the VSIE handler */
		kvm_s390_sync_request(KVM_REQ_VSIE_RESTART, vcpu);
	}

	kvm_s390_vcpu_unblock_all(kvm);
}

static int kvm_s390_vm_set_crypto(struct kvm *kvm, struct kvm_device_attr *attr)
{
	mutex_lock(&kvm->lock);
	switch (attr->attr) {
	case KVM_S390_VM_CRYPTO_ENABLE_AES_KW:
		if (!test_kvm_facility(kvm, 76)) {
			mutex_unlock(&kvm->lock);
			return -EINVAL;
		}
		get_random_bytes(
			kvm->arch.crypto.crycb->aes_wrapping_key_mask,
			sizeof(kvm->arch.crypto.crycb->aes_wrapping_key_mask));
		kvm->arch.crypto.aes_kw = 1;
		VM_EVENT(kvm, 3, "%s", "ENABLE: AES keywrapping support");
		break;
	case KVM_S390_VM_CRYPTO_ENABLE_DEA_KW:
		if (!test_kvm_facility(kvm, 76)) {
			mutex_unlock(&kvm->lock);
			return -EINVAL;
		}
		get_random_bytes(
			kvm->arch.crypto.crycb->dea_wrapping_key_mask,
			sizeof(kvm->arch.crypto.crycb->dea_wrapping_key_mask));
		kvm->arch.crypto.dea_kw = 1;
		VM_EVENT(kvm, 3, "%s", "ENABLE: DEA keywrapping support");
		break;
	case KVM_S390_VM_CRYPTO_DISABLE_AES_KW:
		if (!test_kvm_facility(kvm, 76)) {
			mutex_unlock(&kvm->lock);
			return -EINVAL;
		}
		kvm->arch.crypto.aes_kw = 0;
		memset(kvm->arch.crypto.crycb->aes_wrapping_key_mask, 0,
			sizeof(kvm->arch.crypto.crycb->aes_wrapping_key_mask));
		VM_EVENT(kvm, 3, "%s", "DISABLE: AES keywrapping support");
		break;
	case KVM_S390_VM_CRYPTO_DISABLE_DEA_KW:
		if (!test_kvm_facility(kvm, 76)) {
			mutex_unlock(&kvm->lock);
			return -EINVAL;
		}
		kvm->arch.crypto.dea_kw = 0;
		memset(kvm->arch.crypto.crycb->dea_wrapping_key_mask, 0,
			sizeof(kvm->arch.crypto.crycb->dea_wrapping_key_mask));
		VM_EVENT(kvm, 3, "%s", "DISABLE: DEA keywrapping support");
		break;
	case KVM_S390_VM_CRYPTO_ENABLE_APIE:
		if (!ap_instructions_available()) {
			mutex_unlock(&kvm->lock);
			return -EOPNOTSUPP;
		}
		kvm->arch.crypto.apie = 1;
		break;
	case KVM_S390_VM_CRYPTO_DISABLE_APIE:
		if (!ap_instructions_available()) {
			mutex_unlock(&kvm->lock);
			return -EOPNOTSUPP;
		}
		kvm->arch.crypto.apie = 0;
		break;
	default:
		mutex_unlock(&kvm->lock);
		return -ENXIO;
	}

	kvm_s390_vcpu_crypto_reset_all(kvm);
	mutex_unlock(&kvm->lock);
	return 0;
}

static void kvm_s390_vcpu_pci_setup(struct kvm_vcpu *vcpu)
{
	/* Only set the ECB bits after guest requests zPCI interpretation */
	if (!vcpu->kvm->arch.use_zpci_interp)
		return;

	vcpu->arch.sie_block->ecb2 |= ECB2_ZPCI_LSI;
	vcpu->arch.sie_block->ecb3 |= ECB3_AISII + ECB3_AISI;
}

void kvm_s390_vcpu_pci_enable_interp(struct kvm *kvm)
{
	struct kvm_vcpu *vcpu;
	unsigned long i;

	lockdep_assert_held(&kvm->lock);

	if (!kvm_s390_pci_interp_allowed())
		return;

	/*
	 * If host is configured for PCI and the necessary facilities are
	 * available, turn on interpretation for the life of this guest
	 */
	kvm->arch.use_zpci_interp = 1;

	kvm_s390_vcpu_block_all(kvm);

	kvm_for_each_vcpu(i, vcpu, kvm) {
		kvm_s390_vcpu_pci_setup(vcpu);
		kvm_s390_sync_request(KVM_REQ_VSIE_RESTART, vcpu);
	}

	kvm_s390_vcpu_unblock_all(kvm);
}

static void kvm_s390_sync_request_broadcast(struct kvm *kvm, int req)
{
	unsigned long cx;
	struct kvm_vcpu *vcpu;

	kvm_for_each_vcpu(cx, vcpu, kvm)
		kvm_s390_sync_request(req, vcpu);
}

/*
 * Must be called with kvm->srcu held to avoid races on memslots, and with
 * kvm->slots_lock to avoid races with ourselves and kvm_s390_vm_stop_migration.
 */
static int kvm_s390_vm_start_migration(struct kvm *kvm)
{
	struct kvm_memory_slot *ms;
	struct kvm_memslots *slots;
	unsigned long ram_pages = 0;
	int bkt;

	/* migration mode already enabled */
	if (kvm->arch.migration_mode)
		return 0;
	slots = kvm_memslots(kvm);
	if (!slots || kvm_memslots_empty(slots))
		return -EINVAL;

	if (!kvm->arch.use_cmma) {
		kvm->arch.migration_mode = 1;
		return 0;
	}
	/* mark all the pages in active slots as dirty */
	kvm_for_each_memslot(ms, bkt, slots) {
		if (!ms->dirty_bitmap)
			return -EINVAL;
		/*
		 * The second half of the bitmap is only used on x86,
		 * and would be wasted otherwise, so we put it to good
		 * use here to keep track of the state of the storage
		 * attributes.
		 */
		memset(kvm_second_dirty_bitmap(ms), 0xff, kvm_dirty_bitmap_bytes(ms));
		ram_pages += ms->npages;
	}
	atomic64_set(&kvm->arch.cmma_dirty_pages, ram_pages);
	kvm->arch.migration_mode = 1;
	kvm_s390_sync_request_broadcast(kvm, KVM_REQ_START_MIGRATION);
	return 0;
}

/*
 * Must be called with kvm->slots_lock to avoid races with ourselves and
 * kvm_s390_vm_start_migration.
 */
static int kvm_s390_vm_stop_migration(struct kvm *kvm)
{
	/* migration mode already disabled */
	if (!kvm->arch.migration_mode)
		return 0;
	kvm->arch.migration_mode = 0;
	if (kvm->arch.use_cmma)
		kvm_s390_sync_request_broadcast(kvm, KVM_REQ_STOP_MIGRATION);
	return 0;
}

static int kvm_s390_vm_set_migration(struct kvm *kvm,
				     struct kvm_device_attr *attr)
{
	int res = -ENXIO;

	mutex_lock(&kvm->slots_lock);
	switch (attr->attr) {
	case KVM_S390_VM_MIGRATION_START:
		res = kvm_s390_vm_start_migration(kvm);
		break;
	case KVM_S390_VM_MIGRATION_STOP:
		res = kvm_s390_vm_stop_migration(kvm);
		break;
	default:
		break;
	}
	mutex_unlock(&kvm->slots_lock);

	return res;
}

static int kvm_s390_vm_get_migration(struct kvm *kvm,
				     struct kvm_device_attr *attr)
{
	u64 mig = kvm->arch.migration_mode;

	if (attr->attr != KVM_S390_VM_MIGRATION_STATUS)
		return -ENXIO;

	if (copy_to_user((void __user *)attr->addr, &mig, sizeof(mig)))
		return -EFAULT;
	return 0;
}

static void __kvm_s390_set_tod_clock(struct kvm *kvm, const struct kvm_s390_vm_tod_clock *gtod);

static int kvm_s390_set_tod_ext(struct kvm *kvm, struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_tod_clock gtod;

	if (copy_from_user(&gtod, (void __user *)attr->addr, sizeof(gtod)))
		return -EFAULT;

	if (!test_kvm_facility(kvm, 139) && gtod.epoch_idx)
		return -EINVAL;
	__kvm_s390_set_tod_clock(kvm, &gtod);

	VM_EVENT(kvm, 3, "SET: TOD extension: 0x%x, TOD base: 0x%llx",
		gtod.epoch_idx, gtod.tod);

	return 0;
}

static int kvm_s390_set_tod_high(struct kvm *kvm, struct kvm_device_attr *attr)
{
	u8 gtod_high;

	if (copy_from_user(&gtod_high, (void __user *)attr->addr,
					   sizeof(gtod_high)))
		return -EFAULT;

	if (gtod_high != 0)
		return -EINVAL;
	VM_EVENT(kvm, 3, "SET: TOD extension: 0x%x", gtod_high);

	return 0;
}

static int kvm_s390_set_tod_low(struct kvm *kvm, struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_tod_clock gtod = { 0 };

	if (copy_from_user(&gtod.tod, (void __user *)attr->addr,
			   sizeof(gtod.tod)))
		return -EFAULT;

	__kvm_s390_set_tod_clock(kvm, &gtod);
	VM_EVENT(kvm, 3, "SET: TOD base: 0x%llx", gtod.tod);
	return 0;
}

static int kvm_s390_set_tod(struct kvm *kvm, struct kvm_device_attr *attr)
{
	int ret;

	if (attr->flags)
		return -EINVAL;

	mutex_lock(&kvm->lock);
	/*
	 * For protected guests, the TOD is managed by the ultravisor, so trying
	 * to change it will never bring the expected results.
	 */
	if (kvm_s390_pv_is_protected(kvm)) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}

	switch (attr->attr) {
	case KVM_S390_VM_TOD_EXT:
		ret = kvm_s390_set_tod_ext(kvm, attr);
		break;
	case KVM_S390_VM_TOD_HIGH:
		ret = kvm_s390_set_tod_high(kvm, attr);
		break;
	case KVM_S390_VM_TOD_LOW:
		ret = kvm_s390_set_tod_low(kvm, attr);
		break;
	default:
		ret = -ENXIO;
		break;
	}

out_unlock:
	mutex_unlock(&kvm->lock);
	return ret;
}

static void kvm_s390_get_tod_clock(struct kvm *kvm,
				   struct kvm_s390_vm_tod_clock *gtod)
{
	union tod_clock clk;

	preempt_disable();

	store_tod_clock_ext(&clk);

	gtod->tod = clk.tod + kvm->arch.epoch;
	gtod->epoch_idx = 0;
	if (test_kvm_facility(kvm, 139)) {
		gtod->epoch_idx = clk.ei + kvm->arch.epdx;
		if (gtod->tod < clk.tod)
			gtod->epoch_idx += 1;
	}

	preempt_enable();
}

static int kvm_s390_get_tod_ext(struct kvm *kvm, struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_tod_clock gtod;

	memset(&gtod, 0, sizeof(gtod));
	kvm_s390_get_tod_clock(kvm, &gtod);
	if (copy_to_user((void __user *)attr->addr, &gtod, sizeof(gtod)))
		return -EFAULT;

	VM_EVENT(kvm, 3, "QUERY: TOD extension: 0x%x, TOD base: 0x%llx",
		gtod.epoch_idx, gtod.tod);
	return 0;
}

static int kvm_s390_get_tod_high(struct kvm *kvm, struct kvm_device_attr *attr)
{
	u8 gtod_high = 0;

	if (copy_to_user((void __user *)attr->addr, &gtod_high,
					 sizeof(gtod_high)))
		return -EFAULT;
	VM_EVENT(kvm, 3, "QUERY: TOD extension: 0x%x", gtod_high);

	return 0;
}

static int kvm_s390_get_tod_low(struct kvm *kvm, struct kvm_device_attr *attr)
{
	u64 gtod;

	gtod = kvm_s390_get_tod_clock_fast(kvm);
	if (copy_to_user((void __user *)attr->addr, &gtod, sizeof(gtod)))
		return -EFAULT;
	VM_EVENT(kvm, 3, "QUERY: TOD base: 0x%llx", gtod);

	return 0;
}

static int kvm_s390_get_tod(struct kvm *kvm, struct kvm_device_attr *attr)
{
	int ret;

	if (attr->flags)
		return -EINVAL;

	switch (attr->attr) {
	case KVM_S390_VM_TOD_EXT:
		ret = kvm_s390_get_tod_ext(kvm, attr);
		break;
	case KVM_S390_VM_TOD_HIGH:
		ret = kvm_s390_get_tod_high(kvm, attr);
		break;
	case KVM_S390_VM_TOD_LOW:
		ret = kvm_s390_get_tod_low(kvm, attr);
		break;
	default:
		ret = -ENXIO;
		break;
	}
	return ret;
}

static int kvm_s390_set_processor(struct kvm *kvm, struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_cpu_processor *proc;
	u16 lowest_ibc, unblocked_ibc;
	int ret = 0;

	mutex_lock(&kvm->lock);
	if (kvm->created_vcpus) {
		ret = -EBUSY;
		goto out;
	}
	proc = kzalloc(sizeof(*proc), GFP_KERNEL_ACCOUNT);
	if (!proc) {
		ret = -ENOMEM;
		goto out;
	}
	if (!copy_from_user(proc, (void __user *)attr->addr,
			    sizeof(*proc))) {
		kvm->arch.model.cpuid = proc->cpuid;
		lowest_ibc = sclp.ibc >> 16 & 0xfff;
		unblocked_ibc = sclp.ibc & 0xfff;
		if (lowest_ibc && proc->ibc) {
			if (proc->ibc > unblocked_ibc)
				kvm->arch.model.ibc = unblocked_ibc;
			else if (proc->ibc < lowest_ibc)
				kvm->arch.model.ibc = lowest_ibc;
			else
				kvm->arch.model.ibc = proc->ibc;
		}
		memcpy(kvm->arch.model.fac_list, proc->fac_list,
		       S390_ARCH_FAC_LIST_SIZE_BYTE);
		VM_EVENT(kvm, 3, "SET: guest ibc: 0x%4.4x, guest cpuid: 0x%16.16llx",
			 kvm->arch.model.ibc,
			 kvm->arch.model.cpuid);
		VM_EVENT(kvm, 3, "SET: guest faclist: 0x%16.16llx.%16.16llx.%16.16llx",
			 kvm->arch.model.fac_list[0],
			 kvm->arch.model.fac_list[1],
			 kvm->arch.model.fac_list[2]);
	} else
		ret = -EFAULT;
	kfree(proc);
out:
	mutex_unlock(&kvm->lock);
	return ret;
}

static int kvm_s390_set_processor_feat(struct kvm *kvm,
				       struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_cpu_feat data;

	if (copy_from_user(&data, (void __user *)attr->addr, sizeof(data)))
		return -EFAULT;
	if (!bitmap_subset((unsigned long *) data.feat,
			   kvm_s390_available_cpu_feat,
			   KVM_S390_VM_CPU_FEAT_NR_BITS))
		return -EINVAL;

	mutex_lock(&kvm->lock);
	if (kvm->created_vcpus) {
		mutex_unlock(&kvm->lock);
		return -EBUSY;
	}
	bitmap_from_arr64(kvm->arch.cpu_feat, data.feat, KVM_S390_VM_CPU_FEAT_NR_BITS);
	mutex_unlock(&kvm->lock);
	VM_EVENT(kvm, 3, "SET: guest feat: 0x%16.16llx.0x%16.16llx.0x%16.16llx",
			 data.feat[0],
			 data.feat[1],
			 data.feat[2]);
	return 0;
}

static int kvm_s390_set_processor_subfunc(struct kvm *kvm,
					  struct kvm_device_attr *attr)
{
	mutex_lock(&kvm->lock);
	if (kvm->created_vcpus) {
		mutex_unlock(&kvm->lock);
		return -EBUSY;
	}

	if (copy_from_user(&kvm->arch.model.subfuncs, (void __user *)attr->addr,
			   sizeof(struct kvm_s390_vm_cpu_subfunc))) {
		mutex_unlock(&kvm->lock);
		return -EFAULT;
	}
	mutex_unlock(&kvm->lock);

	VM_EVENT(kvm, 3, "SET: guest PLO    subfunc 0x%16.16lx.%16.16lx.%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.plo)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.plo)[1],
		 ((unsigned long *) &kvm->arch.model.subfuncs.plo)[2],
		 ((unsigned long *) &kvm->arch.model.subfuncs.plo)[3]);
	VM_EVENT(kvm, 3, "SET: guest PTFF   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.ptff)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.ptff)[1]);
	VM_EVENT(kvm, 3, "SET: guest KMAC   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmac)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmac)[1]);
	VM_EVENT(kvm, 3, "SET: guest KMC    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmc)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmc)[1]);
	VM_EVENT(kvm, 3, "SET: guest KM     subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.km)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.km)[1]);
	VM_EVENT(kvm, 3, "SET: guest KIMD   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kimd)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kimd)[1]);
	VM_EVENT(kvm, 3, "SET: guest KLMD   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.klmd)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.klmd)[1]);
	VM_EVENT(kvm, 3, "SET: guest PCKMO  subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.pckmo)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.pckmo)[1]);
	VM_EVENT(kvm, 3, "SET: guest KMCTR  subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmctr)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmctr)[1]);
	VM_EVENT(kvm, 3, "SET: guest KMF    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmf)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmf)[1]);
	VM_EVENT(kvm, 3, "SET: guest KMO    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmo)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmo)[1]);
	VM_EVENT(kvm, 3, "SET: guest PCC    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.pcc)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.pcc)[1]);
	VM_EVENT(kvm, 3, "SET: guest PPNO   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.ppno)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.ppno)[1]);
	VM_EVENT(kvm, 3, "SET: guest KMA    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kma)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kma)[1]);
	VM_EVENT(kvm, 3, "SET: guest KDSA   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kdsa)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kdsa)[1]);
	VM_EVENT(kvm, 3, "SET: guest SORTL  subfunc 0x%16.16lx.%16.16lx.%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.sortl)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.sortl)[1],
		 ((unsigned long *) &kvm->arch.model.subfuncs.sortl)[2],
		 ((unsigned long *) &kvm->arch.model.subfuncs.sortl)[3]);
	VM_EVENT(kvm, 3, "SET: guest DFLTCC subfunc 0x%16.16lx.%16.16lx.%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.dfltcc)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.dfltcc)[1],
		 ((unsigned long *) &kvm->arch.model.subfuncs.dfltcc)[2],
		 ((unsigned long *) &kvm->arch.model.subfuncs.dfltcc)[3]);
	VM_EVENT(kvm, 3, "GET: guest PFCR   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.pfcr)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.pfcr)[1]);

	return 0;
}

#define KVM_S390_VM_CPU_UV_FEAT_GUEST_MASK	\
(						\
	((struct kvm_s390_vm_cpu_uv_feat){	\
		.ap = 1,			\
		.ap_intr = 1,			\
	})					\
	.feat					\
)

static int kvm_s390_set_uv_feat(struct kvm *kvm, struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_cpu_uv_feat __user *ptr = (void __user *)attr->addr;
	unsigned long data, filter;

	filter = uv_info.uv_feature_indications & KVM_S390_VM_CPU_UV_FEAT_GUEST_MASK;
	if (get_user(data, &ptr->feat))
		return -EFAULT;
	if (!bitmap_subset(&data, &filter, KVM_S390_VM_CPU_UV_FEAT_NR_BITS))
		return -EINVAL;

	mutex_lock(&kvm->lock);
	if (kvm->created_vcpus) {
		mutex_unlock(&kvm->lock);
		return -EBUSY;
	}
	kvm->arch.model.uv_feat_guest.feat = data;
	mutex_unlock(&kvm->lock);

	VM_EVENT(kvm, 3, "SET: guest UV-feat: 0x%16.16lx", data);

	return 0;
}

static int kvm_s390_set_cpu_model(struct kvm *kvm, struct kvm_device_attr *attr)
{
	int ret = -ENXIO;

	switch (attr->attr) {
	case KVM_S390_VM_CPU_PROCESSOR:
		ret = kvm_s390_set_processor(kvm, attr);
		break;
	case KVM_S390_VM_CPU_PROCESSOR_FEAT:
		ret = kvm_s390_set_processor_feat(kvm, attr);
		break;
	case KVM_S390_VM_CPU_PROCESSOR_SUBFUNC:
		ret = kvm_s390_set_processor_subfunc(kvm, attr);
		break;
	case KVM_S390_VM_CPU_PROCESSOR_UV_FEAT_GUEST:
		ret = kvm_s390_set_uv_feat(kvm, attr);
		break;
	}
	return ret;
}

static int kvm_s390_get_processor(struct kvm *kvm, struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_cpu_processor *proc;
	int ret = 0;

	proc = kzalloc(sizeof(*proc), GFP_KERNEL_ACCOUNT);
	if (!proc) {
		ret = -ENOMEM;
		goto out;
	}
	proc->cpuid = kvm->arch.model.cpuid;
	proc->ibc = kvm->arch.model.ibc;
	memcpy(&proc->fac_list, kvm->arch.model.fac_list,
	       S390_ARCH_FAC_LIST_SIZE_BYTE);
	VM_EVENT(kvm, 3, "GET: guest ibc: 0x%4.4x, guest cpuid: 0x%16.16llx",
		 kvm->arch.model.ibc,
		 kvm->arch.model.cpuid);
	VM_EVENT(kvm, 3, "GET: guest faclist: 0x%16.16llx.%16.16llx.%16.16llx",
		 kvm->arch.model.fac_list[0],
		 kvm->arch.model.fac_list[1],
		 kvm->arch.model.fac_list[2]);
	if (copy_to_user((void __user *)attr->addr, proc, sizeof(*proc)))
		ret = -EFAULT;
	kfree(proc);
out:
	return ret;
}

static int kvm_s390_get_machine(struct kvm *kvm, struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_cpu_machine *mach;
	int ret = 0;

	mach = kzalloc(sizeof(*mach), GFP_KERNEL_ACCOUNT);
	if (!mach) {
		ret = -ENOMEM;
		goto out;
	}
	get_cpu_id((struct cpuid *) &mach->cpuid);
	mach->ibc = sclp.ibc;
	memcpy(&mach->fac_mask, kvm->arch.model.fac_mask,
	       S390_ARCH_FAC_LIST_SIZE_BYTE);
	memcpy((unsigned long *)&mach->fac_list, stfle_fac_list,
	       sizeof(stfle_fac_list));
	VM_EVENT(kvm, 3, "GET: host ibc:  0x%4.4x, host cpuid:  0x%16.16llx",
		 kvm->arch.model.ibc,
		 kvm->arch.model.cpuid);
	VM_EVENT(kvm, 3, "GET: host facmask:  0x%16.16llx.%16.16llx.%16.16llx",
		 mach->fac_mask[0],
		 mach->fac_mask[1],
		 mach->fac_mask[2]);
	VM_EVENT(kvm, 3, "GET: host faclist:  0x%16.16llx.%16.16llx.%16.16llx",
		 mach->fac_list[0],
		 mach->fac_list[1],
		 mach->fac_list[2]);
	if (copy_to_user((void __user *)attr->addr, mach, sizeof(*mach)))
		ret = -EFAULT;
	kfree(mach);
out:
	return ret;
}

static int kvm_s390_get_processor_feat(struct kvm *kvm,
				       struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_cpu_feat data;

	bitmap_to_arr64(data.feat, kvm->arch.cpu_feat, KVM_S390_VM_CPU_FEAT_NR_BITS);
	if (copy_to_user((void __user *)attr->addr, &data, sizeof(data)))
		return -EFAULT;
	VM_EVENT(kvm, 3, "GET: guest feat: 0x%16.16llx.0x%16.16llx.0x%16.16llx",
			 data.feat[0],
			 data.feat[1],
			 data.feat[2]);
	return 0;
}

static int kvm_s390_get_machine_feat(struct kvm *kvm,
				     struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_cpu_feat data;

	bitmap_to_arr64(data.feat, kvm_s390_available_cpu_feat, KVM_S390_VM_CPU_FEAT_NR_BITS);
	if (copy_to_user((void __user *)attr->addr, &data, sizeof(data)))
		return -EFAULT;
	VM_EVENT(kvm, 3, "GET: host feat:  0x%16.16llx.0x%16.16llx.0x%16.16llx",
			 data.feat[0],
			 data.feat[1],
			 data.feat[2]);
	return 0;
}

static int kvm_s390_get_processor_subfunc(struct kvm *kvm,
					  struct kvm_device_attr *attr)
{
	if (copy_to_user((void __user *)attr->addr, &kvm->arch.model.subfuncs,
	    sizeof(struct kvm_s390_vm_cpu_subfunc)))
		return -EFAULT;

	VM_EVENT(kvm, 3, "GET: guest PLO    subfunc 0x%16.16lx.%16.16lx.%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.plo)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.plo)[1],
		 ((unsigned long *) &kvm->arch.model.subfuncs.plo)[2],
		 ((unsigned long *) &kvm->arch.model.subfuncs.plo)[3]);
	VM_EVENT(kvm, 3, "GET: guest PTFF   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.ptff)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.ptff)[1]);
	VM_EVENT(kvm, 3, "GET: guest KMAC   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmac)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmac)[1]);
	VM_EVENT(kvm, 3, "GET: guest KMC    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmc)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmc)[1]);
	VM_EVENT(kvm, 3, "GET: guest KM     subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.km)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.km)[1]);
	VM_EVENT(kvm, 3, "GET: guest KIMD   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kimd)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kimd)[1]);
	VM_EVENT(kvm, 3, "GET: guest KLMD   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.klmd)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.klmd)[1]);
	VM_EVENT(kvm, 3, "GET: guest PCKMO  subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.pckmo)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.pckmo)[1]);
	VM_EVENT(kvm, 3, "GET: guest KMCTR  subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmctr)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmctr)[1]);
	VM_EVENT(kvm, 3, "GET: guest KMF    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmf)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmf)[1]);
	VM_EVENT(kvm, 3, "GET: guest KMO    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmo)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kmo)[1]);
	VM_EVENT(kvm, 3, "GET: guest PCC    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.pcc)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.pcc)[1]);
	VM_EVENT(kvm, 3, "GET: guest PPNO   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.ppno)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.ppno)[1]);
	VM_EVENT(kvm, 3, "GET: guest KMA    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kma)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kma)[1]);
	VM_EVENT(kvm, 3, "GET: guest KDSA   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.kdsa)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.kdsa)[1]);
	VM_EVENT(kvm, 3, "GET: guest SORTL  subfunc 0x%16.16lx.%16.16lx.%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.sortl)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.sortl)[1],
		 ((unsigned long *) &kvm->arch.model.subfuncs.sortl)[2],
		 ((unsigned long *) &kvm->arch.model.subfuncs.sortl)[3]);
	VM_EVENT(kvm, 3, "GET: guest DFLTCC subfunc 0x%16.16lx.%16.16lx.%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm->arch.model.subfuncs.dfltcc)[0],
		 ((unsigned long *) &kvm->arch.model.subfuncs.dfltcc)[1],
		 ((unsigned long *) &kvm->arch.model.subfuncs.dfltcc)[2],
		 ((unsigned long *) &kvm->arch.model.subfuncs.dfltcc)[3]);
	VM_EVENT(kvm, 3, "GET: guest PFCR   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.pfcr)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.pfcr)[1]);

	return 0;
}

static int kvm_s390_get_machine_subfunc(struct kvm *kvm,
					struct kvm_device_attr *attr)
{
	if (copy_to_user((void __user *)attr->addr, &kvm_s390_available_subfunc,
	    sizeof(struct kvm_s390_vm_cpu_subfunc)))
		return -EFAULT;

	VM_EVENT(kvm, 3, "GET: host  PLO    subfunc 0x%16.16lx.%16.16lx.%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.plo)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.plo)[1],
		 ((unsigned long *) &kvm_s390_available_subfunc.plo)[2],
		 ((unsigned long *) &kvm_s390_available_subfunc.plo)[3]);
	VM_EVENT(kvm, 3, "GET: host  PTFF   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.ptff)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.ptff)[1]);
	VM_EVENT(kvm, 3, "GET: host  KMAC   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.kmac)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.kmac)[1]);
	VM_EVENT(kvm, 3, "GET: host  KMC    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.kmc)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.kmc)[1]);
	VM_EVENT(kvm, 3, "GET: host  KM     subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.km)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.km)[1]);
	VM_EVENT(kvm, 3, "GET: host  KIMD   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.kimd)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.kimd)[1]);
	VM_EVENT(kvm, 3, "GET: host  KLMD   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.klmd)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.klmd)[1]);
	VM_EVENT(kvm, 3, "GET: host  PCKMO  subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.pckmo)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.pckmo)[1]);
	VM_EVENT(kvm, 3, "GET: host  KMCTR  subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.kmctr)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.kmctr)[1]);
	VM_EVENT(kvm, 3, "GET: host  KMF    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.kmf)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.kmf)[1]);
	VM_EVENT(kvm, 3, "GET: host  KMO    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.kmo)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.kmo)[1]);
	VM_EVENT(kvm, 3, "GET: host  PCC    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.pcc)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.pcc)[1]);
	VM_EVENT(kvm, 3, "GET: host  PPNO   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.ppno)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.ppno)[1]);
	VM_EVENT(kvm, 3, "GET: host  KMA    subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.kma)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.kma)[1]);
	VM_EVENT(kvm, 3, "GET: host  KDSA   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.kdsa)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.kdsa)[1]);
	VM_EVENT(kvm, 3, "GET: host  SORTL  subfunc 0x%16.16lx.%16.16lx.%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.sortl)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.sortl)[1],
		 ((unsigned long *) &kvm_s390_available_subfunc.sortl)[2],
		 ((unsigned long *) &kvm_s390_available_subfunc.sortl)[3]);
	VM_EVENT(kvm, 3, "GET: host  DFLTCC subfunc 0x%16.16lx.%16.16lx.%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.dfltcc)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.dfltcc)[1],
		 ((unsigned long *) &kvm_s390_available_subfunc.dfltcc)[2],
		 ((unsigned long *) &kvm_s390_available_subfunc.dfltcc)[3]);
	VM_EVENT(kvm, 3, "GET: host  PFCR   subfunc 0x%16.16lx.%16.16lx",
		 ((unsigned long *) &kvm_s390_available_subfunc.pfcr)[0],
		 ((unsigned long *) &kvm_s390_available_subfunc.pfcr)[1]);

	return 0;
}

static int kvm_s390_get_processor_uv_feat(struct kvm *kvm, struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_cpu_uv_feat __user *dst = (void __user *)attr->addr;
	unsigned long feat = kvm->arch.model.uv_feat_guest.feat;

	if (put_user(feat, &dst->feat))
		return -EFAULT;
	VM_EVENT(kvm, 3, "GET: guest UV-feat: 0x%16.16lx", feat);

	return 0;
}

static int kvm_s390_get_machine_uv_feat(struct kvm *kvm, struct kvm_device_attr *attr)
{
	struct kvm_s390_vm_cpu_uv_feat __user *dst = (void __user *)attr->addr;
	unsigned long feat;

	BUILD_BUG_ON(sizeof(*dst) != sizeof(uv_info.uv_feature_indications));

	feat = uv_info.uv_feature_indications & KVM_S390_VM_CPU_UV_FEAT_GUEST_MASK;
	if (put_user(feat, &dst->feat))
		return -EFAULT;
	VM_EVENT(kvm, 3, "GET: guest UV-feat: 0x%16.16lx", feat);

	return 0;
}

static int kvm_s390_get_cpu_model(struct kvm *kvm, struct kvm_device_attr *attr)
{
	int ret = -ENXIO;

	switch (attr->attr) {
	case KVM_S390_VM_CPU_PROCESSOR:
		ret = kvm_s390_get_processor(kvm, attr);
		break;
	case KVM_S390_VM_CPU_MACHINE:
		ret = kvm_s390_get_machine(kvm, attr);
		break;
	case KVM_S390_VM_CPU_PROCESSOR_FEAT:
		ret = kvm_s390_get_processor_feat(kvm, attr);
		break;
	case KVM_S390_VM_CPU_MACHINE_FEAT:
		ret = kvm_s390_get_machine_feat(kvm, attr);
		break;
	case KVM_S390_VM_CPU_PROCESSOR_SUBFUNC:
		ret = kvm_s390_get_processor_subfunc(kvm, attr);
		break;
	case KVM_S390_VM_CPU_MACHINE_SUBFUNC:
		ret = kvm_s390_get_machine_subfunc(kvm, attr);
		break;
	case KVM_S390_VM_CPU_PROCESSOR_UV_FEAT_GUEST:
		ret = kvm_s390_get_processor_uv_feat(kvm, attr);
		break;
	case KVM_S390_VM_CPU_MACHINE_UV_FEAT_GUEST:
		ret = kvm_s390_get_machine_uv_feat(kvm, attr);
		break;
	}
	return ret;
}

/**
 * kvm_s390_update_topology_change_report - update CPU topology change report
 * @kvm: guest KVM description
 * @val: set or clear the MTCR bit
 *
 * Updates the Multiprocessor Topology-Change-Report bit to signal
 * the guest with a topology change.
 * This is only relevant if the topology facility is present.
 *
 * The SCA version, bsca or esca, doesn't matter as offset is the same.
 */
static void kvm_s390_update_topology_change_report(struct kvm *kvm, bool val)
{
	union sca_utility new, old;
	struct bsca_block *sca;

	read_lock(&kvm->arch.sca_lock);
	sca = kvm->arch.sca;
	old = READ_ONCE(sca->utility);
	do {
		new = old;
		new.mtcr = val;
	} while (!try_cmpxchg(&sca->utility.val, &old.val, new.val));
	read_unlock(&kvm->arch.sca_lock);
}

static int kvm_s390_set_topo_change_indication(struct kvm *kvm,
					       struct kvm_device_attr *attr)
{
	if (!test_kvm_facility(kvm, 11))
		return -ENXIO;

	kvm_s390_update_topology_change_report(kvm, !!attr->attr);
	return 0;
}

static int kvm_s390_get_topo_change_indication(struct kvm *kvm,
					       struct kvm_device_attr *attr)
{
	u8 topo;

	if (!test_kvm_facility(kvm, 11))
		return -ENXIO;

	read_lock(&kvm->arch.sca_lock);
	topo = ((struct bsca_block *)kvm->arch.sca)->utility.mtcr;
	read_unlock(&kvm->arch.sca_lock);

	return put_user(topo, (u8 __user *)attr->addr);
}

static int kvm_s390_vm_set_attr(struct kvm *kvm, struct kvm_device_attr *attr)
{
	int ret;

	switch (attr->group) {
	case KVM_S390_VM_MEM_CTRL:
		ret = kvm_s390_set_mem_control(kvm, attr);
		break;
	case KVM_S390_VM_TOD:
		ret = kvm_s390_set_tod(kvm, attr);
		break;
	case KVM_S390_VM_CPU_MODEL:
		ret = kvm_s390_set_cpu_model(kvm, attr);
		break;
	case KVM_S390_VM_CRYPTO:
		ret = kvm_s390_vm_set_crypto(kvm, attr);
		break;
	case KVM_S390_VM_MIGRATION:
		ret = kvm_s390_vm_set_migration(kvm, attr);
		break;
	case KVM_S390_VM_CPU_TOPOLOGY:
		ret = kvm_s390_set_topo_change_indication(kvm, attr);
		break;
	default:
		ret = -ENXIO;
		break;
	}

	return ret;
}

static int kvm_s390_vm_get_attr(struct kvm *kvm, struct kvm_device_attr *attr)
{
	int ret;

	switch (attr->group) {
	case KVM_S390_VM_MEM_CTRL:
		ret = kvm_s390_get_mem_control(kvm, attr);
		break;
	case KVM_S390_VM_TOD:
		ret = kvm_s390_get_tod(kvm, attr);
		break;
	case KVM_S390_VM_CPU_MODEL:
		ret = kvm_s390_get_cpu_model(kvm, attr);
		break;
	case KVM_S390_VM_MIGRATION:
		ret = kvm_s390_vm_get_migration(kvm, attr);
		break;
	case KVM_S390_VM_CPU_TOPOLOGY:
		ret = kvm_s390_get_topo_change_indication(kvm, attr);
		break;
	default:
		ret = -ENXIO;
		break;
	}

	return ret;
}

static int kvm_s390_vm_has_attr(struct kvm *kvm, struct kvm_device_attr *attr)
{
	int ret;

	switch (attr->group) {
	case KVM_S390_VM_MEM_CTRL:
		switch (attr->attr) {
		case KVM_S390_VM_MEM_ENABLE_CMMA:
		case KVM_S390_VM_MEM_CLR_CMMA:
			ret = sclp.has_cmma ? 0 : -ENXIO;
			break;
		case KVM_S390_VM_MEM_LIMIT_SIZE:
			ret = 0;
			break;
		default:
			ret = -ENXIO;
			break;
		}
		break;
	case KVM_S390_VM_TOD:
		switch (attr->attr) {
		case KVM_S390_VM_TOD_LOW:
		case KVM_S390_VM_TOD_HIGH:
			ret = 0;
			break;
		default:
			ret = -ENXIO;
			break;
		}
		break;
	case KVM_S390_VM_CPU_MODEL:
		switch (attr->attr) {
		case KVM_S390_VM_CPU_PROCESSOR:
		case KVM_S390_VM_CPU_MACHINE:
		case KVM_S390_VM_CPU_PROCESSOR_FEAT:
		case KVM_S390_VM_CPU_MACHINE_FEAT:
		case KVM_S390_VM_CPU_MACHINE_SUBFUNC:
		case KVM_S390_VM_CPU_PROCESSOR_SUBFUNC:
		case KVM_S390_VM_CPU_MACHINE_UV_FEAT_GUEST:
		case KVM_S390_VM_CPU_PROCESSOR_UV_FEAT_GUEST:
			ret = 0;
			break;
		default:
			ret = -ENXIO;
			break;
		}
		break;
	case KVM_S390_VM_CRYPTO:
		switch (attr->attr) {
		case KVM_S390_VM_CRYPTO_ENABLE_AES_KW:
		case KVM_S390_VM_CRYPTO_ENABLE_DEA_KW:
		case KVM_S390_VM_CRYPTO_DISABLE_AES_KW:
		case KVM_S390_VM_CRYPTO_DISABLE_DEA_KW:
			ret = 0;
			break;
		case KVM_S390_VM_CRYPTO_ENABLE_APIE:
		case KVM_S390_VM_CRYPTO_DISABLE_APIE:
			ret = ap_instructions_available() ? 0 : -ENXIO;
			break;
		default:
			ret = -ENXIO;
			break;
		}
		break;
	case KVM_S390_VM_MIGRATION:
		ret = 0;
		break;
	case KVM_S390_VM_CPU_TOPOLOGY:
		ret = test_kvm_facility(kvm, 11) ? 0 : -ENXIO;
		break;
	default:
		ret = -ENXIO;
		break;
	}

	return ret;
}

static int kvm_s390_get_skeys(struct kvm *kvm, struct kvm_s390_skeys *args)
{
	uint8_t *keys;
	uint64_t hva;
	int srcu_idx, i, r = 0;

	if (args->flags != 0)
		return -EINVAL;

	/* Is this guest using storage keys? */
	if (!mm_uses_skeys(current->mm))
		return KVM_S390_GET_SKEYS_NONE;

	/* Enforce sane limit on memory allocation */
	if (args->count < 1 || args->count > KVM_S390_SKEYS_MAX)
		return -EINVAL;

	keys = kvmalloc_array(args->count, sizeof(uint8_t), GFP_KERNEL_ACCOUNT);
	if (!keys)
		return -ENOMEM;

	mmap_read_lock(current->mm);
	srcu_idx = srcu_read_lock(&kvm->srcu);
	for (i = 0; i < args->count; i++) {
		hva = gfn_to_hva(kvm, args->start_gfn + i);
		if (kvm_is_error_hva(hva)) {
			r = -EFAULT;
			break;
		}

		r = get_guest_storage_key(current->mm, hva, &keys[i]);
		if (r)
			break;
	}
	srcu_read_unlock(&kvm->srcu, srcu_idx);
	mmap_read_unlock(current->mm);

	if (!r) {
		r = copy_to_user((uint8_t __user *)args->skeydata_addr, keys,
				 sizeof(uint8_t) * args->count);
		if (r)
			r = -EFAULT;
	}

	kvfree(keys);
	return r;
}

static int kvm_s390_set_skeys(struct kvm *kvm, struct kvm_s390_skeys *args)
{
	uint8_t *keys;
	uint64_t hva;
	int srcu_idx, i, r = 0;
	bool unlocked;

	if (args->flags != 0)
		return -EINVAL;

	/* Enforce sane limit on memory allocation */
	if (args->count < 1 || args->count > KVM_S390_SKEYS_MAX)
		return -EINVAL;

	keys = kvmalloc_array(args->count, sizeof(uint8_t), GFP_KERNEL_ACCOUNT);
	if (!keys)
		return -ENOMEM;

	r = copy_from_user(keys, (uint8_t __user *)args->skeydata_addr,
			   sizeof(uint8_t) * args->count);
	if (r) {
		r = -EFAULT;
		goto out;
	}

	/* Enable storage key handling for the guest */
	r = s390_enable_skey();
	if (r)
		goto out;

	i = 0;
	mmap_read_lock(current->mm);
	srcu_idx = srcu_read_lock(&kvm->srcu);
        while (i < args->count) {
		unlocked = false;
		hva = gfn_to_hva(kvm, args->start_gfn + i);
		if (kvm_is_error_hva(hva)) {
			r = -EFAULT;
			break;
		}

		/* Lowest order bit is reserved */
		if (keys[i] & 0x01) {
			r = -EINVAL;
			break;
		}

		r = set_guest_storage_key(current->mm, hva, keys[i], 0);
		if (r) {
			r = fixup_user_fault(current->mm, hva,
					     FAULT_FLAG_WRITE, &unlocked);
			if (r)
				break;
		}
		if (!r)
			i++;
	}
	srcu_read_unlock(&kvm->srcu, srcu_idx);
	mmap_read_unlock(current->mm);
out:
	kvfree(keys);
	return r;
}

/*
 * Base address and length must be sent at the start of each block, therefore
 * it's cheaper to send some clean data, as long as it's less than the size of
 * two longs.
 */
#define KVM_S390_MAX_BIT_DISTANCE (2 * sizeof(void *))
/* for consistency */
#define KVM_S390_CMMA_SIZE_MAX ((u32)KVM_S390_SKEYS_MAX)

static int kvm_s390_peek_cmma(struct kvm *kvm, struct kvm_s390_cmma_log *args,
			      u8 *res, unsigned long bufsize)
{
	unsigned long pgstev, hva, cur_gfn = args->start_gfn;

	args->count = 0;
	while (args->count < bufsize) {
		hva = gfn_to_hva(kvm, cur_gfn);
		/*
		 * We return an error if the first value was invalid, but we
		 * return successfully if at least one value was copied.
		 */
		if (kvm_is_error_hva(hva))
			return args->count ? 0 : -EFAULT;
		if (get_pgste(kvm->mm, hva, &pgstev) < 0)
			pgstev = 0;
		res[args->count++] = (pgstev >> 24) & 0x43;
		cur_gfn++;
	}

	return 0;
}

static struct kvm_memory_slot *gfn_to_memslot_approx(struct kvm_memslots *slots,
						     gfn_t gfn)
{
	return ____gfn_to_memslot(slots, gfn, true);
}

static unsigned long kvm_s390_next_dirty_cmma(struct kvm_memslots *slots,
					      unsigned long cur_gfn)
{
	struct kvm_memory_slot *ms = gfn_to_memslot_approx(slots, cur_gfn);
	unsigned long ofs = cur_gfn - ms->base_gfn;
	struct rb_node *mnode = &ms->gfn_node[slots->node_idx];

	if (ms->base_gfn + ms->npages <= cur_gfn) {
		mnode = rb_next(mnode);
		/* If we are above the highest slot, wrap around */
		if (!mnode)
			mnode = rb_first(&slots->gfn_tree);

		ms = container_of(mnode, struct kvm_memory_slot, gfn_node[slots->node_idx]);
		ofs = 0;
	}

	if (cur_gfn < ms->base_gfn)
		ofs = 0;

	ofs = find_next_bit(kvm_second_dirty_bitmap(ms), ms->npages, ofs);
	while (ofs >= ms->npages && (mnode = rb_next(mnode))) {
		ms = container_of(mnode, struct kvm_memory_slot, gfn_node[slots->node_idx]);
		ofs = find_first_bit(kvm_second_dirty_bitmap(ms), ms->npages);
	}
	return ms->base_gfn + ofs;
}

static int kvm_s390_get_cmma(struct kvm *kvm, struct kvm_s390_cmma_log *args,
			     u8 *res, unsigned long bufsize)
{
	unsigned long mem_end, cur_gfn, next_gfn, hva, pgstev;
	struct kvm_memslots *slots = kvm_memslots(kvm);
	struct kvm_memory_slot *ms;

	if (unlikely(kvm_memslots_empty(slots)))
		return 0;

	cur_gfn = kvm_s390_next_dirty_cmma(slots, args->start_gfn);
	ms = gfn_to_memslot(kvm, cur_gfn);
	args->count = 0;
	args->start_gfn = cur_gfn;
	if (!ms)
		return 0;
	next_gfn = kvm_s390_next_dirty_cmma(slots, cur_gfn + 1);
	mem_end = kvm_s390_get_gfn_end(slots);

	while (args->count < bufsize) {
		hva = gfn_to_hva(kvm, cur_gfn);
		if (kvm_is_error_hva(hva))
			return 0;
		/* Decrement only if we actually flipped the bit to 0 */
		if (test_and_clear_bit(cur_gfn - ms->base_gfn, kvm_second_dirty_bitmap(ms)))
			atomic64_dec(&kvm->arch.cmma_dirty_pages);
		if (get_pgste(kvm->mm, hva, &pgstev) < 0)
			pgstev = 0;
		/* Save the value */
		res[args->count++] = (pgstev >> 24) & 0x43;
		/* If the next bit is too far away, stop. */
		if (next_gfn > cur_gfn + KVM_S390_MAX_BIT_DISTANCE)
			return 0;
		/* If we reached the previous "next", find the next one */
		if (cur_gfn == next_gfn)
			next_gfn = kvm_s390_next_dirty_cmma(slots, cur_gfn + 1);
		/* Reached the end of memory or of the buffer, stop */
		if ((next_gfn >= mem_end) ||
		    (next_gfn - args->start_gfn >= bufsize))
			return 0;
		cur_gfn++;
		/* Reached the end of the current memslot, take the next one. */
		if (cur_gfn - ms->base_gfn >= ms->npages) {
			ms = gfn_to_memslot(kvm, cur_gfn);
			if (!ms)
				return 0;
		}
	}
	return 0;
}

/*
 * This function searches for the next page with dirty CMMA attributes, and
 * saves the attributes in the buffer up to either the end of the buffer or
 * until a block of at least KVM_S390_MAX_BIT_DISTANCE clean bits is found;
 * no trailing clean bytes are saved.
 * In case no dirty bits were found, or if CMMA was not enabled or used, the
 * output buffer will indicate 0 as length.
 */
static int kvm_s390_get_cmma_bits(struct kvm *kvm,
				  struct kvm_s390_cmma_log *args)
{
	unsigned long bufsize;
	int srcu_idx, peek, ret;
	u8 *values;

	if (!kvm->arch.use_cmma)
		return -ENXIO;
	/* Invalid/unsupported flags were specified */
	if (args->flags & ~KVM_S390_CMMA_PEEK)
		return -EINVAL;
	/* Migration mode query, and we are not doing a migration */
	peek = !!(args->flags & KVM_S390_CMMA_PEEK);
	if (!peek && !kvm->arch.migration_mode)
		return -EINVAL;
	/* CMMA is disabled or was not used, or the buffer has length zero */
	bufsize = min(args->count, KVM_S390_CMMA_SIZE_MAX);
	if (!bufsize || !kvm->mm->context.uses_cmm) {
		memset(args, 0, sizeof(*args));
		return 0;
	}
	/* We are not peeking, and there are no dirty pages */
	if (!peek && !atomic64_read(&kvm->arch.cmma_dirty_pages)) {
		memset(args, 0, sizeof(*args));
		return 0;
	}

	values = vmalloc(bufsize);
	if (!values)
		return -ENOMEM;

	mmap_read_lock(kvm->mm);
	srcu_idx = srcu_read_lock(&kvm->srcu);
	if (peek)
		ret = kvm_s390_peek_cmma(kvm, args, values, bufsize);
	else
		ret = kvm_s390_get_cmma(kvm, args, values, bufsize);
	srcu_read_unlock(&kvm->srcu, srcu_idx);
	mmap_read_unlock(kvm->mm);

	if (kvm->arch.migration_mode)
		args->remaining = atomic64_read(&kvm->arch.cmma_dirty_pages);
	else
		args->remaining = 0;

	if (copy_to_user((void __user *)args->values, values, args->count))
		ret = -EFAULT;

	vfree(values);
	return ret;
}

/*
 * This function sets the CMMA attributes for the given pages. If the input
 * buffer has zero length, no action is taken, otherwise the attributes are
 * set and the mm->context.uses_cmm flag is set.
 */
static int kvm_s390_set_cmma_bits(struct kvm *kvm,
				  const struct kvm_s390_cmma_log *args)
{
	unsigned long hva, mask, pgstev, i;
	uint8_t *bits;
	int srcu_idx, r = 0;

	mask = args->mask;

	if (!kvm->arch.use_cmma)
		return -ENXIO;
	/* invalid/unsupported flags */
	if (args->flags != 0)
		return -EINVAL;
	/* Enforce sane limit on memory allocation */
	if (args->count > KVM_S390_CMMA_SIZE_MAX)
		return -EINVAL;
	/* Nothing to do */
	if (args->count == 0)
		return 0;

	bits = vmalloc(array_size(sizeof(*bits), args->count));
	if (!bits)
		return -ENOMEM;

	r = copy_from_user(bits, (void __user *)args->values, args->count);
	if (r) {
		r = -EFAULT;
		goto out;
	}

	mmap_read_lock(kvm->mm);
	srcu_idx = srcu_read_lock(&kvm->srcu);
	for (i = 0; i < args->count; i++) {
		hva = gfn_to_hva(kvm, args->start_gfn + i);
		if (kvm_is_error_hva(hva)) {
			r = -EFAULT;
			break;
		}

		pgstev = bits[i];
		pgstev = pgstev << 24;
		mask &= _PGSTE_GPS_USAGE_MASK | _PGSTE_GPS_NODAT;
		set_pgste_bits(kvm->mm, hva, mask, pgstev);
	}
	srcu_read_unlock(&kvm->srcu, srcu_idx);
	mmap_read_unlock(kvm->mm);

	if (!kvm->mm->context.uses_cmm) {
		mmap_write_lock(kvm->mm);
		kvm->mm->context.uses_cmm = 1;
		mmap_write_unlock(kvm->mm);
	}
out:
	vfree(bits);
	return r;
}

/**
 * kvm_s390_cpus_from_pv - Convert all protected vCPUs in a protected VM to
 * non protected.
 * @kvm: the VM whose protected vCPUs are to be converted
 * @rc: return value for the RC field of the UVC (in case of error)
 * @rrc: return value for the RRC field of the UVC (in case of error)
 *
 * Does not stop in case of error, tries to convert as many
 * CPUs as possible. In case of error, the RC and RRC of the last error are
 * returned.
 *
 * Return: 0 in case of success, otherwise -EIO
 */
int kvm_s390_cpus_from_pv(struct kvm *kvm, u16 *rc, u16 *rrc)
{
	struct kvm_vcpu *vcpu;
	unsigned long i;
	u16 _rc, _rrc;
	int ret = 0;

	/*
	 * We ignore failures and try to destroy as many CPUs as possible.
	 * At the same time we must not free the assigned resources when
	 * this fails, as the ultravisor has still access to that memory.
	 * So kvm_s390_pv_destroy_cpu can leave a "wanted" memory leak
	 * behind.
	 * We want to return the first failure rc and rrc, though.
	 */
	kvm_for_each_vcpu(i, vcpu, kvm) {
		mutex_lock(&vcpu->mutex);
		if (kvm_s390_pv_destroy_cpu(vcpu, &_rc, &_rrc) && !ret) {
			*rc = _rc;
			*rrc = _rrc;
			ret = -EIO;
		}
		mutex_unlock(&vcpu->mutex);
	}
	/* Ensure that we re-enable gisa if the non-PV guest used it but the PV guest did not. */
	if (use_gisa)
		kvm_s390_gisa_enable(kvm);
	return ret;
}

/**
 * kvm_s390_cpus_to_pv - Convert all non-protected vCPUs in a protected VM
 * to protected.
 * @kvm: the VM whose protected vCPUs are to be converted
 * @rc: return value for the RC field of the UVC (in case of error)
 * @rrc: return value for the RRC field of the UVC (in case of error)
 *
 * Tries to undo the conversion in case of error.
 *
 * Return: 0 in case of success, otherwise -EIO
 */
static int kvm_s390_cpus_to_pv(struct kvm *kvm, u16 *rc, u16 *rrc)
{
	unsigned long i;
	int r = 0;
	u16 dummy;

	struct kvm_vcpu *vcpu;

	/* Disable the GISA if the ultravisor does not support AIV. */
	if (!uv_has_feature(BIT_UV_FEAT_AIV))
		kvm_s390_gisa_disable(kvm);

	kvm_for_each_vcpu(i, vcpu, kvm) {
		mutex_lock(&vcpu->mutex);
		r = kvm_s390_pv_create_cpu(vcpu, rc, rrc);
		mutex_unlock(&vcpu->mutex);
		if (r)
			break;
	}
	if (r)
		kvm_s390_cpus_from_pv(kvm, &dummy, &dummy);
	return r;
}

/*
 * Here we provide user space with a direct interface to query UV
 * related data like UV maxima and available features as well as
 * feature specific data.
 *
 * To facilitate future extension of the data structures we'll try to
 * write data up to the maximum requested length.
 */
static ssize_t kvm_s390_handle_pv_info(struct kvm_s390_pv_info *info)
{
	ssize_t len_min;

	switch (info->header.id) {
	case KVM_PV_INFO_VM: {
		len_min =  sizeof(info->header) + sizeof(info->vm);

		if (info->header.len_max < len_min)
			return -EINVAL;

		memcpy(info->vm.inst_calls_list,
		       uv_info.inst_calls_list,
		       sizeof(uv_info.inst_calls_list));

		/* It's max cpuid not max cpus, so it's off by one */
		info->vm.max_cpus = uv_info.max_guest_cpu_id + 1;
		info->vm.max_guests = uv_info.max_num_sec_conf;
		info->vm.max_guest_addr = uv_info.max_sec_stor_addr;
		info->vm.feature_indication = uv_info.uv_feature_indications;

		return len_min;
	}
	case KVM_PV_INFO_DUMP: {
		len_min =  sizeof(info->header) + sizeof(info->dump);

		if (info->header.len_max < len_min)
			return -EINVAL;

		info->dump.dump_cpu_buffer_len = uv_info.guest_cpu_stor_len;
		info->dump.dump_config_mem_buffer_per_1m = uv_info.conf_dump_storage_state_len;
		info->dump.dump_config_finalize_len = uv_info.conf_dump_finalize_len;
		return len_min;
	}
	default:
		return -EINVAL;
	}
}

static int kvm_s390_pv_dmp(struct kvm *kvm, struct kvm_pv_cmd *cmd,
			   struct kvm_s390_pv_dmp dmp)
{
	int r = -EINVAL;
	void __user *result_buff = (void __user *)dmp.buff_addr;

	switch (dmp.subcmd) {
	case KVM_PV_DUMP_INIT: {
		if (kvm->arch.pv.dumping)
			break;

		/*
		 * Block SIE entry as concurrent dump UVCs could lead
		 * to validities.
		 */
		kvm_s390_vcpu_block_all(kvm);

		r = uv_cmd_nodata(kvm_s390_pv_get_handle(kvm),
				  UVC_CMD_DUMP_INIT, &cmd->rc, &cmd->rrc);
		KVM_UV_EVENT(kvm, 3, "PROTVIRT DUMP INIT: rc %x rrc %x",
			     cmd->rc, cmd->rrc);
		if (!r) {
			kvm->arch.pv.dumping = true;
		} else {
			kvm_s390_vcpu_unblock_all(kvm);
			r = -EINVAL;
		}
		break;
	}
	case KVM_PV_DUMP_CONFIG_STOR_STATE: {
		if (!kvm->arch.pv.dumping)
			break;

		/*
		 * gaddr is an output parameter since we might stop
		 * early. As dmp will be copied back in our caller, we
		 * don't need to do it ourselves.
		 */
		r = kvm_s390_pv_dump_stor_state(kvm, result_buff, &dmp.gaddr, dmp.buff_len,
						&cmd->rc, &cmd->rrc);
		break;
	}
	case KVM_PV_DUMP_COMPLETE: {
		if (!kvm->arch.pv.dumping)
			break;

		r = -EINVAL;
		if (dmp.buff_len < uv_info.conf_dump_finalize_len)
			break;

		r = kvm_s390_pv_dump_complete(kvm, result_buff,
					      &cmd->rc, &cmd->rrc);
		break;
	}
	default:
		r = -ENOTTY;
		break;
	}

	return r;
}

static int kvm_s390_handle_pv(struct kvm *kvm, struct kvm_pv_cmd *cmd)
{
	const bool need_lock = (cmd->cmd != KVM_PV_ASYNC_CLEANUP_PERFORM);
	void __user *argp = (void __user *)cmd->data;
	int r = 0;
	u16 dummy;

	if (need_lock)
		mutex_lock(&kvm->lock);

	switch (cmd->cmd) {
	case KVM_PV_ENABLE: {
		r = -EINVAL;
		if (kvm_s390_pv_is_protected(kvm))
			break;

		/*
		 *  FMT 4 SIE needs esca. As we never switch back to bsca from
		 *  esca, we need no cleanup in the error cases below
		 */
		r = sca_switch_to_extended(kvm);
		if (r)
			break;

		mmap_write_lock(kvm->mm);
		r = gmap_helper_disable_cow_sharing();
		mmap_write_unlock(kvm->mm);
		if (r)
			break;

		r = kvm_s390_pv_init_vm(kvm, &cmd->rc, &cmd->rrc);
		if (r)
			break;

		r = kvm_s390_cpus_to_pv(kvm, &cmd->rc, &cmd->rrc);
		if (r)
			kvm_s390_pv_deinit_vm(kvm, &dummy, &dummy);

		/* we need to block service interrupts from now on */
		set_bit(IRQ_PEND_EXT_SERVICE, &kvm->arch.float_int.masked_irqs);
		break;
	}
	case KVM_PV_ASYNC_CLEANUP_PREPARE:
		r = -EINVAL;
		if (!kvm_s390_pv_is_protected(kvm) || !async_destroy)
			break;

		r = kvm_s390_cpus_from_pv(kvm, &cmd->rc, &cmd->rrc);
		/*
		 * If a CPU could not be destroyed, destroy VM will also fail.
		 * There is no point in trying to destroy it. Instead return
		 * the rc and rrc from the first CPU that failed destroying.
		 */
		if (r)
			break;
		r = kvm_s390_pv_set_aside(kvm, &cmd->rc, &cmd->rrc);

		/* no need to block service interrupts any more */
		clear_bit(IRQ_PEND_EXT_SERVICE, &kvm->arch.float_int.masked_irqs);
		break;
	case KVM_PV_ASYNC_CLEANUP_PERFORM:
		r = -EINVAL;
		if (!async_destroy)
			break;
		/* kvm->lock must not be held; this is asserted inside the function. */
		r = kvm_s390_pv_deinit_aside_vm(kvm, &cmd->rc, &cmd->rrc);
		break;
	case KVM_PV_DISABLE: {
		r = -EINVAL;
		if (!kvm_s390_pv_is_protected(kvm))
			break;

		r = kvm_s390_cpus_from_pv(kvm, &cmd->rc, &cmd->rrc);
		/*
		 * If a CPU could not be destroyed, destroy VM will also fail.
		 * There is no point in trying to destroy it. Instead return
		 * the rc and rrc from the first CPU that failed destroying.
		 */
		if (r)
			break;
		r = kvm_s390_pv_deinit_cleanup_all(kvm, &cmd->rc, &cmd->rrc);

		/* no need to block service interrupts any more */
		clear_bit(IRQ_PEND_EXT_SERVICE, &kvm->arch.float_int.masked_irqs);
		break;
	}
	case KVM_PV_SET_SEC_PARMS: {
		struct kvm_s390_pv_sec_parm parms = {};
		void *hdr;

		r = -EINVAL;
		if (!kvm_s390_pv_is_protected(kvm))
			break;

		r = -EFAULT;
		if (copy_from_user(&parms, argp, sizeof(parms)))
			break;

		/* Currently restricted to 8KB */
		r = -EINVAL;
		if (parms.length > PAGE_SIZE * 2)
			break;

		r = -ENOMEM;
		hdr = vmalloc(parms.length);
		if (!hdr)
			break;

		r = -EFAULT;
		if (!copy_from_user(hdr, (void __user *)parms.origin,
				    parms.length))
			r = kvm_s390_pv_set_sec_parms(kvm, hdr, parms.length,
						      &cmd->rc, &cmd->rrc);

		vfree(hdr);
		break;
	}
	case KVM_PV_UNPACK: {
		struct kvm_s390_pv_unp unp = {};

		r = -EINVAL;
		if (!kvm_s390_pv_is_protected(kvm) || !mm_is_protected(kvm->mm))
			break;

		r = -EFAULT;
		if (copy_from_user(&unp, argp, sizeof(unp)))
			break;

		r = kvm_s390_pv_unpack(kvm, unp.addr, unp.size, unp.tweak,
				       &cmd->rc, &cmd->rrc);
		break;
	}
	case KVM_PV_VERIFY: {
		r = -EINVAL;
		if (!kvm_s390_pv_is_protected(kvm))
			break;

		r = uv_cmd_nodata(kvm_s390_pv_get_handle(kvm),
				  UVC_CMD_VERIFY_IMG, &cmd->rc, &cmd->rrc);
		KVM_UV_EVENT(kvm, 3, "PROTVIRT VERIFY: rc %x rrc %x", cmd->rc,
			     cmd->rrc);
		break;
	}
	case KVM_PV_PREP_RESET: {
		r = -EINVAL;
		if (!kvm_s390_pv_is_protected(kvm))
			break;

		r = uv_cmd_nodata(kvm_s390_pv_get_handle(kvm),
				  UVC_CMD_PREPARE_RESET, &cmd->rc, &cmd->rrc);
		KVM_UV_EVENT(kvm, 3, "PROTVIRT PREP RESET: rc %x rrc %x",
			     cmd->rc, cmd->rrc);
		break;
	}
	case KVM_PV_UNSHARE_ALL: {
		r = -EINVAL;
		if (!kvm_s390_pv_is_protected(kvm))
			break;

		r = uv_cmd_nodata(kvm_s390_pv_get_handle(kvm),
				  UVC_CMD_SET_UNSHARE_ALL, &cmd->rc, &cmd->rrc);
		KVM_UV_EVENT(kvm, 3, "PROTVIRT UNSHARE: rc %x rrc %x",
			     cmd->rc, cmd->rrc);
		break;
	}
	case KVM_PV_INFO: {
		struct kvm_s390_pv_info info = {};
		ssize_t data_len;

		/*
		 * No need to check the VM protection here.
		 *
		 * Maybe user space wants to query some of the data
		 * when the VM is still unprotected. If we see the
		 * need to fence a new data command we can still
		 * return an error in the info handler.
		 */

		r = -EFAULT;
		if (copy_from_user(&info, argp, sizeof(info.header)))
			break;

		r = -EINVAL;
		if (info.header.len_max < sizeof(info.header))
			break;

		data_len = kvm_s390_handle_pv_info(&info);
		if (data_len < 0) {
			r = data_len;
			break;
		}
		/*
		 * If a data command struct is extended (multiple
		 * times) this can be used to determine how much of it
		 * is valid.
		 */
		info.header.len_written = data_len;

		r = -EFAULT;
		if (copy_to_user(argp, &info, data_len))
			break;

		r = 0;
		break;
	}
	case KVM_PV_DUMP: {
		struct kvm_s390_pv_dmp dmp;

		r = -EINVAL;
		if (!kvm_s390_pv_is_protected(kvm))
			break;

		r = -EFAULT;
		if (copy_from_user(&dmp, argp, sizeof(dmp)))
			break;

		r = kvm_s390_pv_dmp(kvm, cmd, dmp);
		if (r)
			break;

		if (copy_to_user(argp, &dmp, sizeof(dmp))) {
			r = -EFAULT;
			break;
		}

		break;
	}
	default:
		r = -ENOTTY;
	}
	if (need_lock)
		mutex_unlock(&kvm->lock);

	return r;
}

static int mem_op_validate_common(struct kvm_s390_mem_op *mop, u64 supported_flags)
{
	if (mop->flags & ~supported_flags || !mop->size)
		return -EINVAL;
	if (mop->size > MEM_OP_MAX_SIZE)
		return -E2BIG;
	if (mop->flags & KVM_S390_MEMOP_F_SKEY_PROTECTION) {
		if (mop->key > 0xf)
			return -EINVAL;
	} else {
		mop->key = 0;
	}
	return 0;
}

static int kvm_s390_vm_mem_op_abs(struct kvm *kvm, struct kvm_s390_mem_op *mop)
{
	void __user *uaddr = (void __user *)mop->buf;
	enum gacc_mode acc_mode;
	void *tmpbuf = NULL;
	int r, srcu_idx;

	r = mem_op_validate_common(mop, KVM_S390_MEMOP_F_SKEY_PROTECTION |
					KVM_S390_MEMOP_F_CHECK_ONLY);
	if (r)
		return r;

	if (!(mop->flags & KVM_S390_MEMOP_F_CHECK_ONLY)) {
		tmpbuf = vmalloc(mop->size);
		if (!tmpbuf)
			return -ENOMEM;
	}

	srcu_idx = srcu_read_lock(&kvm->srcu);

	if (!kvm_is_gpa_in_memslot(kvm, mop->gaddr)) {
		r = PGM_ADDRESSING;
		goto out_unlock;
	}

	acc_mode = mop->op == KVM_S390_MEMOP_ABSOLUTE_READ ? GACC_FETCH : GACC_STORE;
	if (mop->flags & KVM_S390_MEMOP_F_CHECK_ONLY) {
		r = check_gpa_range(kvm, mop->gaddr, mop->size, acc_mode, mop->key);
		goto out_unlock;
	}
	if (acc_mode == GACC_FETCH) {
		r = access_guest_abs_with_key(kvm, mop->gaddr, tmpbuf,
					      mop->size, GACC_FETCH, mop->key);
		if (r)
			goto out_unlock;
		if (copy_to_user(uaddr, tmpbuf, mop->size))
			r = -EFAULT;
	} else {
		if (copy_from_user(tmpbuf, uaddr, mop->size)) {
			r = -EFAULT;
			goto out_unlock;
		}
		r = access_guest_abs_with_key(kvm, mop->gaddr, tmpbuf,
					      mop->size, GACC_STORE, mop->key);
	}

out_unlock:
	srcu_read_unlock(&kvm->srcu, srcu_idx);

	vfree(tmpbuf);
	return r;
}

static int kvm_s390_vm_mem_op_cmpxchg(struct kvm *kvm, struct kvm_s390_mem_op *mop)
{
	void __user *uaddr = (void __user *)mop->buf;
	void __user *old_addr = (void __user *)mop->old_addr;
	union {
		__uint128_t quad;
		char raw[sizeof(__uint128_t)];
	} old = { .quad = 0}, new = { .quad = 0 };
	unsigned int off_in_quad = sizeof(new) - mop->size;
	int r, srcu_idx;
	bool success;

	r = mem_op_validate_common(mop, KVM_S390_MEMOP_F_SKEY_PROTECTION);
	if (r)
		return r;
	/*
	 * This validates off_in_quad. Checking that size is a power
	 * of two is not necessary, as cmpxchg_guest_abs_with_key
	 * takes care of that
	 */
	if (mop->size > sizeof(new))
		return -EINVAL;
	if (copy_from_user(&new.raw[off_in_quad], uaddr, mop->size))
		return -EFAULT;
	if (copy_from_user(&old.raw[off_in_quad], old_addr, mop->size))
		return -EFAULT;

	srcu_idx = srcu_read_lock(&kvm->srcu);

	if (!kvm_is_gpa_in_memslot(kvm, mop->gaddr)) {
		r = PGM_ADDRESSING;
		goto out_unlock;
	}

	r = cmpxchg_guest_abs_with_key(kvm, mop->gaddr, mop->size, &old.quad,
				       new.quad, mop->key, &success);
	if (!success && copy_to_user(old_addr, &old.raw[off_in_quad], mop->size))
		r = -EFAULT;

out_unlock:
	srcu_read_unlock(&kvm->srcu, srcu_idx);
	return r;
}

static int kvm_s390_vm_mem_op(struct kvm *kvm, struct kvm_s390_mem_op *mop)
{
	/*
	 * This is technically a heuristic only, if the kvm->lock is not
	 * taken, it is not guaranteed that the vm is/remains non-protected.
	 * This is ok from a kernel perspective, wrongdoing is detected
	 * on the access, -EFAULT is returned and the vm may crash the
	 * next time it accesses the memory in question.
	 * There is no sane usecase to do switching and a memop on two
	 * different CPUs at the same time.
	 */
	if (kvm_s390_pv_get_handle(kvm))
		return -EINVAL;

	switch (mop->op) {
	case KVM_S390_MEMOP_ABSOLUTE_READ:
	case KVM_S390_MEMOP_ABSOLUTE_WRITE:
		return kvm_s390_vm_mem_op_abs(kvm, mop);
	case KVM_S390_MEMOP_ABSOLUTE_CMPXCHG:
		return kvm_s390_vm_mem_op_cmpxchg(kvm, mop);
	default:
		return -EINVAL;
	}
}

int kvm_arch_vm_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	struct kvm *kvm = filp->private_data;
	void __user *argp = (void __user *)arg;
	struct kvm_device_attr attr;
	int r;

	switch (ioctl) {
	case KVM_S390_INTERRUPT: {
		struct kvm_s390_interrupt s390int;

		r = -EFAULT;
		if (copy_from_user(&s390int, argp, sizeof(s390int)))
			break;
		r = kvm_s390_inject_vm(kvm, &s390int);
		break;
	}
	case KVM_CREATE_IRQCHIP: {
		r = -EINVAL;
		if (kvm->arch.use_irqchip)
			r = 0;
		break;
	}
	case KVM_SET_DEVICE_ATTR: {
		r = -EFAULT;
		if (copy_from_user(&attr, (void __user *)arg, sizeof(attr)))
			break;
		r = kvm_s390_vm_set_attr(kvm, &attr);
		break;
	}
	case KVM_GET_DEVICE_ATTR: {
		r = -EFAULT;
		if (copy_from_user(&attr, (void __user *)arg, sizeof(attr)))
			break;
		r = kvm_s390_vm_get_attr(kvm, &attr);
		break;
	}
	case KVM_HAS_DEVICE_ATTR: {
		r = -EFAULT;
		if (copy_from_user(&attr, (void __user *)arg, sizeof(attr)))
			break;
		r = kvm_s390_vm_has_attr(kvm, &attr);
		break;
	}
	case KVM_S390_GET_SKEYS: {
		struct kvm_s390_skeys args;

		r = -EFAULT;
		if (copy_from_user(&args, argp,
				   sizeof(struct kvm_s390_skeys)))
			break;
		r = kvm_s390_get_skeys(kvm, &args);
		break;
	}
	case KVM_S390_SET_SKEYS: {
		struct kvm_s390_skeys args;

		r = -EFAULT;
		if (copy_from_user(&args, argp,
				   sizeof(struct kvm_s390_skeys)))
			break;
		r = kvm_s390_set_skeys(kvm, &args);
		break;
	}
	case KVM_S390_GET_CMMA_BITS: {
		struct kvm_s390_cmma_log args;

		r = -EFAULT;
		if (copy_from_user(&args, argp, sizeof(args)))
			break;
		mutex_lock(&kvm->slots_lock);
		r = kvm_s390_get_cmma_bits(kvm, &args);
		mutex_unlock(&kvm->slots_lock);
		if (!r) {
			r = copy_to_user(argp, &args, sizeof(args));
			if (r)
				r = -EFAULT;
		}
		break;
	}
	case KVM_S390_SET_CMMA_BITS: {
		struct kvm_s390_cmma_log args;

		r = -EFAULT;
		if (copy_from_user(&args, argp, sizeof(args)))
			break;
		mutex_lock(&kvm->slots_lock);
		r = kvm_s390_set_cmma_bits(kvm, &args);
		mutex_unlock(&kvm->slots_lock);
		break;
	}
	case KVM_S390_PV_COMMAND: {
		struct kvm_pv_cmd args;

		/* protvirt means user cpu state */
		kvm_s390_set_user_cpu_state_ctrl(kvm);
		r = 0;
		if (!is_prot_virt_host()) {
			r = -EINVAL;
			break;
		}
		if (copy_from_user(&args, argp, sizeof(args))) {
			r = -EFAULT;
			break;
		}
		if (args.flags) {
			r = -EINVAL;
			break;
		}
		/* must be called without kvm->lock */
		r = kvm_s390_handle_pv(kvm, &args);
		if (copy_to_user(argp, &args, sizeof(args))) {
			r = -EFAULT;
			break;
		}
		break;
	}
	case KVM_S390_MEM_OP: {
		struct kvm_s390_mem_op mem_op;

		if (copy_from_user(&mem_op, argp, sizeof(mem_op)) == 0)
			r = kvm_s390_vm_mem_op(kvm, &mem_op);
		else
			r = -EFAULT;
		break;
	}
	case KVM_S390_ZPCI_OP: {
		struct kvm_s390_zpci_op args;

		r = -EINVAL;
		if (!IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM))
			break;
		if (copy_from_user(&args, argp, sizeof(args))) {
			r = -EFAULT;
			break;
		}
		r = kvm_s390_pci_zpci_op(kvm, &args);
		break;
	}
	default:
		r = -ENOTTY;
	}

	return r;
}

static int kvm_s390_apxa_installed(void)
{
	struct ap_config_info info;

	if (ap_instructions_available()) {
		if (ap_qci(&info) == 0)
			return info.apxa;
	}

	return 0;
}

/*
 * The format of the crypto control block (CRYCB) is specified in the 3 low
 * order bits of the CRYCB designation (CRYCBD) field as follows:
 * Format 0: Neither the message security assist extension 3 (MSAX3) nor the
 *	     AP extended addressing (APXA) facility are installed.
 * Format 1: The APXA facility is not installed but the MSAX3 facility is.
 * Format 2: Both the APXA and MSAX3 facilities are installed
 */
static void kvm_s390_set_crycb_format(struct kvm *kvm)
{
	kvm->arch.crypto.crycbd = virt_to_phys(kvm->arch.crypto.crycb);

	/* Clear the CRYCB format bits - i.e., set format 0 by default */
	kvm->arch.crypto.crycbd &= ~(CRYCB_FORMAT_MASK);

	/* Check whether MSAX3 is installed */
	if (!test_kvm_facility(kvm, 76))
		return;

	if (kvm_s390_apxa_installed())
		kvm->arch.crypto.crycbd |= CRYCB_FORMAT2;
	else
		kvm->arch.crypto.crycbd |= CRYCB_FORMAT1;
}

/*
 * kvm_arch_crypto_set_masks
 *
 * @kvm: pointer to the target guest's KVM struct containing the crypto masks
 *	 to be set.
 * @apm: the mask identifying the accessible AP adapters
 * @aqm: the mask identifying the accessible AP domains
 * @adm: the mask identifying the accessible AP control domains
 *
 * Set the masks that identify the adapters, domains and control domains to
 * which the KVM guest is granted access.
 *
 * Note: The kvm->lock mutex must be locked by the caller before invoking this
 *	 function.
 */
void kvm_arch_crypto_set_masks(struct kvm *kvm, unsigned long *apm,
			       unsigned long *aqm, unsigned long *adm)
{
	struct kvm_s390_crypto_cb *crycb = kvm->arch.crypto.crycb;

	kvm_s390_vcpu_block_all(kvm);

	switch (kvm->arch.crypto.crycbd & CRYCB_FORMAT_MASK) {
	case CRYCB_FORMAT2: /* APCB1 use 256 bits */
		memcpy(crycb->apcb1.apm, apm, 32);
		VM_EVENT(kvm, 3, "SET CRYCB: apm %016lx %016lx %016lx %016lx",
			 apm[0], apm[1], apm[2], apm[3]);
		memcpy(crycb->apcb1.aqm, aqm, 32);
		VM_EVENT(kvm, 3, "SET CRYCB: aqm %016lx %016lx %016lx %016lx",
			 aqm[0], aqm[1], aqm[2], aqm[3]);
		memcpy(crycb->apcb1.adm, adm, 32);
		VM_EVENT(kvm, 3, "SET CRYCB: adm %016lx %016lx %016lx %016lx",
			 adm[0], adm[1], adm[2], adm[3]);
		break;
	case CRYCB_FORMAT1:
	case CRYCB_FORMAT0: /* Fall through both use APCB0 */
		memcpy(crycb->apcb0.apm, apm, 8);
		memcpy(crycb->apcb0.aqm, aqm, 2);
		memcpy(crycb->apcb0.adm, adm, 2);
		VM_EVENT(kvm, 3, "SET CRYCB: apm %016lx aqm %04x adm %04x",
			 apm[0], *((unsigned short *)aqm),
			 *((unsigned short *)adm));
		break;
	default:	/* Can not happen */
		break;
	}

	/* recreate the shadow crycb for each vcpu */
	kvm_s390_sync_request_broadcast(kvm, KVM_REQ_VSIE_RESTART);
	kvm_s390_vcpu_unblock_all(kvm);
}
EXPORT_SYMBOL_GPL(kvm_arch_crypto_set_masks);

/*
 * kvm_arch_crypto_clear_masks
 *
 * @kvm: pointer to the target guest's KVM struct containing the crypto masks
 *	 to be cleared.
 *
 * Clear the masks that identify the adapters, domains and control domains to
 * which the KVM guest is granted access.
 *
 * Note: The kvm->lock mutex must be locked by the caller before invoking this
 *	 function.
 */
void kvm_arch_crypto_clear_masks(struct kvm *kvm)
{
	kvm_s390_vcpu_block_all(kvm);

	memset(&kvm->arch.crypto.crycb->apcb0, 0,
	       sizeof(kvm->arch.crypto.crycb->apcb0));
	memset(&kvm->arch.crypto.crycb->apcb1, 0,
	       sizeof(kvm->arch.crypto.crycb->apcb1));

	VM_EVENT(kvm, 3, "%s", "CLR CRYCB:");
	/* recreate the shadow crycb for each vcpu */
	kvm_s390_sync_request_broadcast(kvm, KVM_REQ_VSIE_RESTART);
	kvm_s390_vcpu_unblock_all(kvm);
}
EXPORT_SYMBOL_GPL(kvm_arch_crypto_clear_masks);

static u64 kvm_s390_get_initial_cpuid(void)
{
	struct cpuid cpuid;

	get_cpu_id(&cpuid);
	cpuid.version = 0xff;
	return *((u64 *) &cpuid);
}

static void kvm_s390_crypto_init(struct kvm *kvm)
{
	kvm->arch.crypto.crycb = &kvm->arch.sie_page2->crycb;
	kvm_s390_set_crycb_format(kvm);
	init_rwsem(&kvm->arch.crypto.pqap_hook_rwsem);

	if (!test_kvm_facility(kvm, 76))
		return;

	/* Enable AES/DEA protected key functions by default */
	kvm->arch.crypto.aes_kw = 1;
	kvm->arch.crypto.dea_kw = 1;
	get_random_bytes(kvm->arch.crypto.crycb->aes_wrapping_key_mask,
			 sizeof(kvm->arch.crypto.crycb->aes_wrapping_key_mask));
	get_random_bytes(kvm->arch.crypto.crycb->dea_wrapping_key_mask,
			 sizeof(kvm->arch.crypto.crycb->dea_wrapping_key_mask));
}

static void sca_dispose(struct kvm *kvm)
{
	if (kvm->arch.use_esca)
		free_pages_exact(kvm->arch.sca, sizeof(struct esca_block));
	else
		free_page((unsigned long)(kvm->arch.sca));
	kvm->arch.sca = NULL;
}

void kvm_arch_free_vm(struct kvm *kvm)
{
	if (IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM))
		kvm_s390_pci_clear_list(kvm);

	__kvm_arch_free_vm(kvm);
}

int kvm_arch_init_vm(struct kvm *kvm, unsigned long type)
{
	gfp_t alloc_flags = GFP_KERNEL_ACCOUNT;
	int i, rc;
	char debug_name[16];
	static unsigned long sca_offset;

	rc = -EINVAL;
#ifdef CONFIG_KVM_S390_UCONTROL
	if (type & ~KVM_VM_S390_UCONTROL)
		goto out_err;
	if ((type & KVM_VM_S390_UCONTROL) && (!capable(CAP_SYS_ADMIN)))
		goto out_err;
#else
	if (type)
		goto out_err;
#endif

	rc = s390_enable_sie();
	if (rc)
		goto out_err;

	rc = -ENOMEM;

	if (!sclp.has_64bscao)
		alloc_flags |= GFP_DMA;
	rwlock_init(&kvm->arch.sca_lock);
	/* start with basic SCA */
	kvm->arch.sca = (struct bsca_block *) get_zeroed_page(alloc_flags);
	if (!kvm->arch.sca)
		goto out_err;
	mutex_lock(&kvm_lock);
	sca_offset += 16;
	if (sca_offset + sizeof(struct bsca_block) > PAGE_SIZE)
		sca_offset = 0;
	kvm->arch.sca = (struct bsca_block *)
			((char *) kvm->arch.sca + sca_offset);
	mutex_unlock(&kvm_lock);

	sprintf(debug_name, "kvm-%u", current->pid);

	kvm->arch.dbf = debug_register(debug_name, 32, 1, 7 * sizeof(long));
	if (!kvm->arch.dbf)
		goto out_err;

	BUILD_BUG_ON(sizeof(struct sie_page2) != 4096);
	kvm->arch.sie_page2 =
	     (struct sie_page2 *) get_zeroed_page(GFP_KERNEL_ACCOUNT | GFP_DMA);
	if (!kvm->arch.sie_page2)
		goto out_err;

	kvm->arch.sie_page2->kvm = kvm;
	kvm->arch.model.fac_list = kvm->arch.sie_page2->fac_list;

	for (i = 0; i < kvm_s390_fac_size(); i++) {
		kvm->arch.model.fac_mask[i] = stfle_fac_list[i] &
					      (kvm_s390_fac_base[i] |
					       kvm_s390_fac_ext[i]);
		kvm->arch.model.fac_list[i] = stfle_fac_list[i] &
					      kvm_s390_fac_base[i];
	}
	kvm->arch.model.subfuncs = kvm_s390_available_subfunc;

	/* we are always in czam mode - even on pre z14 machines */
	set_kvm_facility(kvm->arch.model.fac_mask, 138);
	set_kvm_facility(kvm->arch.model.fac_list, 138);
	/* we emulate STHYI in kvm */
	set_kvm_facility(kvm->arch.model.fac_mask, 74);
	set_kvm_facility(kvm->arch.model.fac_list, 74);
	if (machine_has_tlb_guest()) {
		set_kvm_facility(kvm->arch.model.fac_mask, 147);
		set_kvm_facility(kvm->arch.model.fac_list, 147);
	}

	if (css_general_characteristics.aiv && test_facility(65))
		set_kvm_facility(kvm->arch.model.fac_mask, 65);

	kvm->arch.model.cpuid = kvm_s390_get_initial_cpuid();
	kvm->arch.model.ibc = sclp.ibc & 0x0fff;

	kvm->arch.model.uv_feat_guest.feat = 0;

	kvm_s390_crypto_init(kvm);

	if (IS_ENABLED(CONFIG_VFIO_PCI_ZDEV_KVM)) {
		mutex_lock(&kvm->lock);
		kvm_s390_pci_init_list(kvm);
		kvm_s390_vcpu_pci_enable_interp(kvm);
		mutex_unlock(&kvm->lock);
	}

	mutex_init(&kvm->arch.float_int.ais_lock);
	spin_lock_init(&kvm->arch.float_int.lock);
	for (i = 0; i < FIRQ_LIST_COUNT; i++)
		INIT_LIST_HEAD(&kvm->arch.float_int.lists[i]);
	init_waitqueue_head(&kvm->arch.ipte_wq);
	mutex_init(&kvm->arch.ipte_mutex);

	debug_register_view(kvm->arch.dbf, &debug_sprintf_view);
	VM_EVENT(kvm, 3, "vm created with type %lu", type);

	if (type & KVM_VM_S390_UCONTROL) {
		struct kvm_userspace_memory_region2 fake_memslot = {
			.slot = KVM_S390_UCONTROL_MEMSLOT,
			.guest_phys_addr = 0,
			.userspace_addr = 0,
			.memory_size = ALIGN_DOWN(TASK_SIZE, _SEGMENT_SIZE),
			.flags = 0,
		};

		kvm->arch.gmap = NULL;
		kvm->arch.mem_limit = KVM_S390_NO_MEM_LIMIT;
		/* one flat fake memslot covering the whole address-space */
		mutex_lock(&kvm->slots_lock);
		KVM_BUG_ON(kvm_set_internal_memslot(kvm, &fake_memslot), kvm);
		mutex_unlock(&kvm->slots_lock);
	} else {
		if (sclp.hamax == U64_MAX)
			kvm->arch.mem_limit = TASK_SIZE_MAX;
		else
			kvm->arch.mem_limit = min_t(unsigned long, TASK_SIZE_MAX,
						    sclp.hamax + 1);
		kvm->arch.gmap = gmap_create(current->mm, kvm->arch.mem_limit - 1);
		if (!kvm->arch.gmap)
			goto out_err;
		kvm->arch.gmap->private = kvm;
		kvm->arch.gmap->pfault_enabled = 0;
	}

	kvm->arch.use_pfmfi = sclp.has_pfmfi;
	kvm->arch.use_skf = sclp.has_skey;
	spin_lock_init(&kvm->arch.start_stop_lock);
	kvm_s390_vsie_init(kvm);
	if (use_gisa)
		kvm_s390_gisa_init(kvm);
	INIT_LIST_HEAD(&kvm->arch.pv.need_cleanup);
	kvm->arch.pv.set_aside = NULL;
	KVM_EVENT(3, "vm 0x%p created by pid %u", kvm, current->pid);

	return 0;
out_err:
	free_page((unsigned long)kvm->arch.sie_page2);
	debug_unregister(kvm->arch.dbf);
	sca_dispose(kvm);
	KVM_EVENT(3, "creation of vm failed: %d", rc);
	return rc;
}

void kvm_arch_vcpu_destroy(struct kvm_vcpu *vcpu)
{
	u16 rc, rrc;

	VCPU_EVENT(vcpu, 3, "%s", "free cpu");
	trace_kvm_s390_destroy_vcpu(vcpu->vcpu_id);
	kvm_s390_clear_local_irqs(vcpu);
	kvm_clear_async_pf_completion_queue(vcpu);
	if (!kvm_is_ucontrol(vcpu->kvm))
		sca_del_vcpu(vcpu);
	kvm_s390_update_topology_change_report(vcpu->kvm, 1);

	if (kvm_is_ucontrol(vcpu->kvm))
		gmap_remove(vcpu->arch.gmap);

	if (vcpu->kvm->arch.use_cmma)
		kvm_s390_vcpu_unsetup_cmma(vcpu);
	/* We can not hold the vcpu mutex here, we are already dying */
	if (kvm_s390_pv_cpu_get_handle(vcpu))
		kvm_s390_pv_destroy_cpu(vcpu, &rc, &rrc);
	free_page((unsigned long)(vcpu->arch.sie_block));
}

void kvm_arch_destroy_vm(struct kvm *kvm)
{
	u16 rc, rrc;

	kvm_destroy_vcpus(kvm);
	sca_dispose(kvm);
	kvm_s390_gisa_destroy(kvm);
	/*
	 * We are already at the end of life and kvm->lock is not taken.
	 * This is ok as the file descriptor is closed by now and nobody
	 * can mess with the pv state.
	 */
	kvm_s390_pv_deinit_cleanup_all(kvm, &rc, &rrc);
	/*
	 * Remove the mmu notifier only when the whole KVM VM is torn down,
	 * and only if one was registered to begin with. If the VM is
	 * currently not protected, but has been previously been protected,
	 * then it's possible that the notifier is still registered.
	 */
	if (kvm->arch.pv.mmu_notifier.ops)
		mmu_notifier_unregister(&kvm->arch.pv.mmu_notifier, kvm->mm);

	debug_unregister(kvm->arch.dbf);
	free_page((unsigned long)kvm->arch.sie_page2);
	if (!kvm_is_ucontrol(kvm))
		gmap_remove(kvm->arch.gmap);
	kvm_s390_destroy_adapters(kvm);
	kvm_s390_clear_float_irqs(kvm);
	kvm_s390_vsie_destroy(kvm);
	KVM_EVENT(3, "vm 0x%p destroyed", kvm);
}

/* Section: vcpu related */
static int __kvm_ucontrol_vcpu_init(struct kvm_vcpu *vcpu)
{
	vcpu->arch.gmap = gmap_create(current->mm, -1UL);
	if (!vcpu->arch.gmap)
		return -ENOMEM;
	vcpu->arch.gmap->private = vcpu->kvm;

	return 0;
}

static void sca_del_vcpu(struct kvm_vcpu *vcpu)
{
	if (!kvm_s390_use_sca_entries())
		return;
	read_lock(&vcpu->kvm->arch.sca_lock);
	if (vcpu->kvm->arch.use_esca) {
		struct esca_block *sca = vcpu->kvm->arch.sca;

		clear_bit_inv(vcpu->vcpu_id, (unsigned long *) sca->mcn);
		sca->cpu[vcpu->vcpu_id].sda = 0;
	} else {
		struct bsca_block *sca = vcpu->kvm->arch.sca;

		clear_bit_inv(vcpu->vcpu_id, (unsigned long *) &sca->mcn);
		sca->cpu[vcpu->vcpu_id].sda = 0;
	}
	read_unlock(&vcpu->kvm->arch.sca_lock);
}

static void sca_add_vcpu(struct kvm_vcpu *vcpu)
{
	if (!kvm_s390_use_sca_entries()) {
		phys_addr_t sca_phys = virt_to_phys(vcpu->kvm->arch.sca);

		/* we still need the basic sca for the ipte control */
		vcpu->arch.sie_block->scaoh = sca_phys >> 32;
		vcpu->arch.sie_block->scaol = sca_phys;
		return;
	}
	read_lock(&vcpu->kvm->arch.sca_lock);
	if (vcpu->kvm->arch.use_esca) {
		struct esca_block *sca = vcpu->kvm->arch.sca;
		phys_addr_t sca_phys = virt_to_phys(sca);

		sca->cpu[vcpu->vcpu_id].sda = virt_to_phys(vcpu->arch.sie_block);
		vcpu->arch.sie_block->scaoh = sca_phys >> 32;
		vcpu->arch.sie_block->scaol = sca_phys & ESCA_SCAOL_MASK;
		vcpu->arch.sie_block->ecb2 |= ECB2_ESCA;
		set_bit_inv(vcpu->vcpu_id, (unsigned long *) sca->mcn);
	} else {
		struct bsca_block *sca = vcpu->kvm->arch.sca;
		phys_addr_t sca_phys = virt_to_phys(sca);

		sca->cpu[vcpu->vcpu_id].sda = virt_to_phys(vcpu->arch.sie_block);
		vcpu->arch.sie_block->scaoh = sca_phys >> 32;
		vcpu->arch.sie_block->scaol = sca_phys;
		set_bit_inv(vcpu->vcpu_id, (unsigned long *) &sca->mcn);
	}
	read_unlock(&vcpu->kvm->arch.sca_lock);
}

/* Basic SCA to Extended SCA data copy routines */
static inline void sca_copy_entry(struct esca_entry *d, struct bsca_entry *s)
{
	d->sda = s->sda;
	d->sigp_ctrl.c = s->sigp_ctrl.c;
	d->sigp_ctrl.scn = s->sigp_ctrl.scn;
}

static void sca_copy_b_to_e(struct esca_block *d, struct bsca_block *s)
{
	int i;

	d->ipte_control = s->ipte_control;
	d->mcn[0] = s->mcn;
	for (i = 0; i < KVM_S390_BSCA_CPU_SLOTS; i++)
		sca_copy_entry(&d->cpu[i], &s->cpu[i]);
}

static int sca_switch_to_extended(struct kvm *kvm)
{
	struct bsca_block *old_sca = kvm->arch.sca;
	struct esca_block *new_sca;
	struct kvm_vcpu *vcpu;
	unsigned long vcpu_idx;
	u32 scaol, scaoh;
	phys_addr_t new_sca_phys;

	if (kvm->arch.use_esca)
		return 0;

	new_sca = alloc_pages_exact(sizeof(*new_sca), GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!new_sca)
		return -ENOMEM;

	new_sca_phys = virt_to_phys(new_sca);
	scaoh = new_sca_phys >> 32;
	scaol = new_sca_phys & ESCA_SCAOL_MASK;

	kvm_s390_vcpu_block_all(kvm);
	write_lock(&kvm->arch.sca_lock);

	sca_copy_b_to_e(new_sca, old_sca);

	kvm_for_each_vcpu(vcpu_idx, vcpu, kvm) {
		vcpu->arch.sie_block->scaoh = scaoh;
		vcpu->arch.sie_block->scaol = scaol;
		vcpu->arch.sie_block->ecb2 |= ECB2_ESCA;
	}
	kvm->arch.sca = new_sca;
	kvm->arch.use_esca = 1;

	write_unlock(&kvm->arch.sca_lock);
	kvm_s390_vcpu_unblock_all(kvm);

	free_page((unsigned long)old_sca);

	VM_EVENT(kvm, 2, "Switched to ESCA (0x%p -> 0x%p)",
		 old_sca, kvm->arch.sca);
	return 0;
}

static int sca_can_add_vcpu(struct kvm *kvm, unsigned int id)
{
	int rc;

	if (!kvm_s390_use_sca_entries()) {
		if (id < KVM_MAX_VCPUS)
			return true;
		return false;
	}
	if (id < KVM_S390_BSCA_CPU_SLOTS)
		return true;
	if (!sclp.has_esca || !sclp.has_64bscao)
		return false;

	rc = kvm->arch.use_esca ? 0 : sca_switch_to_extended(kvm);

	return rc == 0 && id < KVM_S390_ESCA_CPU_SLOTS;
}

/* needs disabled preemption to protect from TOD sync and vcpu_load/put */
static void __start_cpu_timer_accounting(struct kvm_vcpu *vcpu)
{
	WARN_ON_ONCE(vcpu->arch.cputm_start != 0);
	raw_write_seqcount_begin(&vcpu->arch.cputm_seqcount);
	vcpu->arch.cputm_start = get_tod_clock_fast();
	raw_write_seqcount_end(&vcpu->arch.cputm_seqcount);
}

/* needs disabled preemption to protect from TOD sync and vcpu_load/put */
static void __stop_cpu_timer_accounting(struct kvm_vcpu *vcpu)
{
	WARN_ON_ONCE(vcpu->arch.cputm_start == 0);
	raw_write_seqcount_begin(&vcpu->arch.cputm_seqcount);
	vcpu->arch.sie_block->cputm -= get_tod_clock_fast() - vcpu->arch.cputm_start;
	vcpu->arch.cputm_start = 0;
	raw_write_seqcount_end(&vcpu->arch.cputm_seqcount);
}

/* needs disabled preemption to protect from TOD sync and vcpu_load/put */
static void __enable_cpu_timer_accounting(struct kvm_vcpu *vcpu)
{
	WARN_ON_ONCE(vcpu->arch.cputm_enabled);
	vcpu->arch.cputm_enabled = true;
	__start_cpu_timer_accounting(vcpu);
}

/* needs disabled preemption to protect from TOD sync and vcpu_load/put */
static void __disable_cpu_timer_accounting(struct kvm_vcpu *vcpu)
{
	WARN_ON_ONCE(!vcpu->arch.cputm_enabled);
	__stop_cpu_timer_accounting(vcpu);
	vcpu->arch.cputm_enabled = false;
}

static void enable_cpu_timer_accounting(struct kvm_vcpu *vcpu)
{
	preempt_disable(); /* protect from TOD sync and vcpu_load/put */
	__enable_cpu_timer_accounting(vcpu);
	preempt_enable();
}

static void disable_cpu_timer_accounting(struct kvm_vcpu *vcpu)
{
	preempt_disable(); /* protect from TOD sync and vcpu_load/put */
	__disable_cpu_timer_accounting(vcpu);
	preempt_enable();
}

/* set the cpu timer - may only be called from the VCPU thread itself */
void kvm_s390_set_cpu_timer(struct kvm_vcpu *vcpu, __u64 cputm)
{
	preempt_disable(); /* protect from TOD sync and vcpu_load/put */
	raw_write_seqcount_begin(&vcpu->arch.cputm_seqcount);
	if (vcpu->arch.cputm_enabled)
		vcpu->arch.cputm_start = get_tod_clock_fast();
	vcpu->arch.sie_block->cputm = cputm;
	raw_write_seqcount_end(&vcpu->arch.cputm_seqcount);
	preempt_enable();
}

/* update and get the cpu timer - can also be called from other VCPU threads */
__u64 kvm_s390_get_cpu_timer(struct kvm_vcpu *vcpu)
{
	unsigned int seq;
	__u64 value;

	if (unlikely(!vcpu->arch.cputm_enabled))
		return vcpu->arch.sie_block->cputm;

	preempt_disable(); /* protect from TOD sync and vcpu_load/put */
	do {
		seq = raw_read_seqcount(&vcpu->arch.cputm_seqcount);
		/*
		 * If the writer would ever execute a read in the critical
		 * section, e.g. in irq context, we have a deadlock.
		 */
		WARN_ON_ONCE((seq & 1) && smp_processor_id() == vcpu->cpu);
		value = vcpu->arch.sie_block->cputm;
		/* if cputm_start is 0, accounting is being started/stopped */
		if (likely(vcpu->arch.cputm_start))
			value -= get_tod_clock_fast() - vcpu->arch.cputm_start;
	} while (read_seqcount_retry(&vcpu->arch.cputm_seqcount, seq & ~1));
	preempt_enable();
	return value;
}

void kvm_arch_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{

	kvm_s390_set_cpuflags(vcpu, CPUSTAT_RUNNING);
	if (vcpu->arch.cputm_enabled && !is_vcpu_idle(vcpu))
		__start_cpu_timer_accounting(vcpu);
	vcpu->cpu = cpu;
}

void kvm_arch_vcpu_put(struct kvm_vcpu *vcpu)
{
	vcpu->cpu = -1;
	if (vcpu->arch.cputm_enabled && !is_vcpu_idle(vcpu))
		__stop_cpu_timer_accounting(vcpu);
	kvm_s390_clear_cpuflags(vcpu, CPUSTAT_RUNNING);

}

void kvm_arch_vcpu_postcreate(struct kvm_vcpu *vcpu)
{
	mutex_lock(&vcpu->kvm->lock);
	preempt_disable();
	vcpu->arch.sie_block->epoch = vcpu->kvm->arch.epoch;
	vcpu->arch.sie_block->epdx = vcpu->kvm->arch.epdx;
	preempt_enable();
	mutex_unlock(&vcpu->kvm->lock);
	if (!kvm_is_ucontrol(vcpu->kvm)) {
		vcpu->arch.gmap = vcpu->kvm->arch.gmap;
		sca_add_vcpu(vcpu);
	}
	if (test_kvm_facility(vcpu->kvm, 74) || vcpu->kvm->arch.user_instr0)
		vcpu->arch.sie_block->ictl |= ICTL_OPEREXC;
}

static bool kvm_has_pckmo_subfunc(struct kvm *kvm, unsigned long nr)
{
	if (test_bit_inv(nr, (unsigned long *)&kvm->arch.model.subfuncs.pckmo) &&
	    test_bit_inv(nr, (unsigned long *)&kvm_s390_available_subfunc.pckmo))
		return true;
	return false;
}

static bool kvm_has_pckmo_ecc(struct kvm *kvm)
{
	/* At least one ECC subfunction must be present */
	return kvm_has_pckmo_subfunc(kvm, 32) ||
	       kvm_has_pckmo_subfunc(kvm, 33) ||
	       kvm_has_pckmo_subfunc(kvm, 34) ||
	       kvm_has_pckmo_subfunc(kvm, 40) ||
	       kvm_has_pckmo_subfunc(kvm, 41);

}

static bool kvm_has_pckmo_hmac(struct kvm *kvm)
{
	/* At least one HMAC subfunction must be present */
	return kvm_has_pckmo_subfunc(kvm, 118) ||
	       kvm_has_pckmo_subfunc(kvm, 122);
}

static void kvm_s390_vcpu_crypto_setup(struct kvm_vcpu *vcpu)
{
	/*
	 * If the AP instructions are not being interpreted and the MSAX3
	 * facility is not configured for the guest, there is nothing to set up.
	 */
	if (!vcpu->kvm->arch.crypto.apie && !test_kvm_facility(vcpu->kvm, 76))
		return;

	vcpu->arch.sie_block->crycbd = vcpu->kvm->arch.crypto.crycbd;
	vcpu->arch.sie_block->ecb3 &= ~(ECB3_AES | ECB3_DEA);
	vcpu->arch.sie_block->eca &= ~ECA_APIE;
	vcpu->arch.sie_block->ecd &= ~(ECD_ECC | ECD_HMAC);

	if (vcpu->kvm->arch.crypto.apie)
		vcpu->arch.sie_block->eca |= ECA_APIE;

	/* Set up protected key support */
	if (vcpu->kvm->arch.crypto.aes_kw) {
		vcpu->arch.sie_block->ecb3 |= ECB3_AES;
		/* ecc/hmac is also wrapped with AES key */
		if (kvm_has_pckmo_ecc(vcpu->kvm))
			vcpu->arch.sie_block->ecd |= ECD_ECC;
		if (kvm_has_pckmo_hmac(vcpu->kvm))
			vcpu->arch.sie_block->ecd |= ECD_HMAC;
	}

	if (vcpu->kvm->arch.crypto.dea_kw)
		vcpu->arch.sie_block->ecb3 |= ECB3_DEA;
}

void kvm_s390_vcpu_unsetup_cmma(struct kvm_vcpu *vcpu)
{
	free_page((unsigned long)phys_to_virt(vcpu->arch.sie_block->cbrlo));
	vcpu->arch.sie_block->cbrlo = 0;
}

int kvm_s390_vcpu_setup_cmma(struct kvm_vcpu *vcpu)
{
	void *cbrlo_page = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);

	if (!cbrlo_page)
		return -ENOMEM;

	vcpu->arch.sie_block->cbrlo = virt_to_phys(cbrlo_page);
	return 0;
}

static void kvm_s390_vcpu_setup_model(struct kvm_vcpu *vcpu)
{
	struct kvm_s390_cpu_model *model = &vcpu->kvm->arch.model;

	vcpu->arch.sie_block->ibc = model->ibc;
	if (test_kvm_facility(vcpu->kvm, 7))
		vcpu->arch.sie_block->fac = virt_to_phys(model->fac_list);
}

static int kvm_s390_vcpu_setup(struct kvm_vcpu *vcpu)
{
	int rc = 0;
	u16 uvrc, uvrrc;

	atomic_set(&vcpu->arch.sie_block->cpuflags, CPUSTAT_ZARCH |
						    CPUSTAT_SM |
						    CPUSTAT_STOPPED);

	if (test_kvm_facility(vcpu->kvm, 78))
		kvm_s390_set_cpuflags(vcpu, CPUSTAT_GED2);
	else if (test_kvm_facility(vcpu->kvm, 8))
		kvm_s390_set_cpuflags(vcpu, CPUSTAT_GED);

	kvm_s390_vcpu_setup_model(vcpu);

	/* pgste_set_pte has special handling for !machine_has_esop() */
	if (machine_has_esop())
		vcpu->arch.sie_block->ecb |= ECB_HOSTPROTINT;
	if (test_kvm_facility(vcpu->kvm, 9))
		vcpu->arch.sie_block->ecb |= ECB_SRSI;
	if (test_kvm_facility(vcpu->kvm, 11))
		vcpu->arch.sie_block->ecb |= ECB_PTF;
	if (test_kvm_facility(vcpu->kvm, 73))
		vcpu->arch.sie_block->ecb |= ECB_TE;
	if (!kvm_is_ucontrol(vcpu->kvm))
		vcpu->arch.sie_block->ecb |= ECB_SPECI;

	if (test_kvm_facility(vcpu->kvm, 8) && vcpu->kvm->arch.use_pfmfi)
		vcpu->arch.sie_block->ecb2 |= ECB2_PFMFI;
	if (test_kvm_facility(vcpu->kvm, 130))
		vcpu->arch.sie_block->ecb2 |= ECB2_IEP;
	vcpu->arch.sie_block->eca = ECA_MVPGI | ECA_PROTEXCI;
	if (sclp.has_cei)
		vcpu->arch.sie_block->eca |= ECA_CEI;
	if (sclp.has_ib)
		vcpu->arch.sie_block->eca |= ECA_IB;
	if (sclp.has_siif)
		vcpu->arch.sie_block->eca |= ECA_SII;
	if (sclp.has_sigpif)
		vcpu->arch.sie_block->eca |= ECA_SIGPI;
	if (test_kvm_facility(vcpu->kvm, 129)) {
		vcpu->arch.sie_block->eca |= ECA_VX;
		vcpu->arch.sie_block->ecd |= ECD_HOSTREGMGMT;
	}
	if (test_kvm_facility(vcpu->kvm, 139))
		vcpu->arch.sie_block->ecd |= ECD_MEF;
	if (test_kvm_facility(vcpu->kvm, 156))
		vcpu->arch.sie_block->ecd |= ECD_ETOKENF;
	if (vcpu->arch.sie_block->gd) {
		vcpu->arch.sie_block->eca |= ECA_AIV;
		VCPU_EVENT(vcpu, 3, "AIV gisa format-%u enabled for cpu %03u",
			   vcpu->arch.sie_block->gd & 0x3, vcpu->vcpu_id);
	}
	vcpu->arch.sie_block->sdnxo = virt_to_phys(&vcpu->run->s.regs.sdnx) | SDNXC;
	vcpu->arch.sie_block->riccbd = virt_to_phys(&vcpu->run->s.regs.riccb);

	if (sclp.has_kss)
		kvm_s390_set_cpuflags(vcpu, CPUSTAT_KSS);
	else
		vcpu->arch.sie_block->ictl |= ICTL_ISKE | ICTL_SSKE | ICTL_RRBE;

	if (vcpu->kvm->arch.use_cmma) {
		rc = kvm_s390_vcpu_setup_cmma(vcpu);
		if (rc)
			return rc;
	}
	hrtimer_setup(&vcpu->arch.ckc_timer, kvm_s390_idle_wakeup, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);

	vcpu->arch.sie_block->hpid = HPID_KVM;

	kvm_s390_vcpu_crypto_setup(vcpu);

	kvm_s390_vcpu_pci_setup(vcpu);

	mutex_lock(&vcpu->kvm->lock);
	if (kvm_s390_pv_is_protected(vcpu->kvm)) {
		rc = kvm_s390_pv_create_cpu(vcpu, &uvrc, &uvrrc);
		if (rc)
			kvm_s390_vcpu_unsetup_cmma(vcpu);
	}
	mutex_unlock(&vcpu->kvm->lock);

	return rc;
}

int kvm_arch_vcpu_precreate(struct kvm *kvm, unsigned int id)
{
	if (!kvm_is_ucontrol(kvm) && !sca_can_add_vcpu(kvm, id))
		return -EINVAL;
	return 0;
}

int kvm_arch_vcpu_create(struct kvm_vcpu *vcpu)
{
	struct sie_page *sie_page;
	int rc;

	BUILD_BUG_ON(sizeof(struct sie_page) != 4096);
	sie_page = (struct sie_page *) get_zeroed_page(GFP_KERNEL_ACCOUNT);
	if (!sie_page)
		return -ENOMEM;

	vcpu->arch.sie_block = &sie_page->sie_block;
	vcpu->arch.sie_block->itdba = virt_to_phys(&sie_page->itdb);

	/* the real guest size will always be smaller than msl */
	vcpu->arch.sie_block->mso = 0;
	vcpu->arch.sie_block->msl = sclp.hamax;

	vcpu->arch.sie_block->icpua = vcpu->vcpu_id;
	spin_lock_init(&vcpu->arch.local_int.lock);
	vcpu->arch.sie_block->gd = kvm_s390_get_gisa_desc(vcpu->kvm);
	seqcount_init(&vcpu->arch.cputm_seqcount);

	vcpu->arch.pfault_token = KVM_S390_PFAULT_TOKEN_INVALID;
	kvm_clear_async_pf_completion_queue(vcpu);
	vcpu->run->kvm_valid_regs = KVM_SYNC_PREFIX |
				    KVM_SYNC_GPRS |
				    KVM_SYNC_ACRS |
				    KVM_SYNC_CRS |
				    KVM_SYNC_ARCH0 |
				    KVM_SYNC_PFAULT |
				    KVM_SYNC_DIAG318;
	vcpu->arch.acrs_loaded = false;
	kvm_s390_set_prefix(vcpu, 0);
	if (test_kvm_facility(vcpu->kvm, 64))
		vcpu->run->kvm_valid_regs |= KVM_SYNC_RICCB;
	if (test_kvm_facility(vcpu->kvm, 82))
		vcpu->run->kvm_valid_regs |= KVM_SYNC_BPBC;
	if (test_kvm_facility(vcpu->kvm, 133))
		vcpu->run->kvm_valid_regs |= KVM_SYNC_GSCB;
	if (test_kvm_facility(vcpu->kvm, 156))
		vcpu->run->kvm_valid_regs |= KVM_SYNC_ETOKEN;
	/* fprs can be synchronized via vrs, even if the guest has no vx. With
	 * cpu_has_vx(), (load|store)_fpu_regs() will work with vrs format.
	 */
	if (cpu_has_vx())
		vcpu->run->kvm_valid_regs |= KVM_SYNC_VRS;
	else
		vcpu->run->kvm_valid_regs |= KVM_SYNC_FPRS;

	if (kvm_is_ucontrol(vcpu->kvm)) {
		rc = __kvm_ucontrol_vcpu_init(vcpu);
		if (rc)
			goto out_free_sie_block;
	}

	VM_EVENT(vcpu->kvm, 3, "create cpu %d at 0x%p, sie block at 0x%p",
		 vcpu->vcpu_id, vcpu, vcpu->arch.sie_block);
	trace_kvm_s390_create_vcpu(vcpu->vcpu_id, vcpu, vcpu->arch.sie_block);

	rc = kvm_s390_vcpu_setup(vcpu);
	if (rc)
		goto out_ucontrol_uninit;

	kvm_s390_update_topology_change_report(vcpu->kvm, 1);
	return 0;

out_ucontrol_uninit:
	if (kvm_is_ucontrol(vcpu->kvm))
		gmap_remove(vcpu->arch.gmap);
out_free_sie_block:
	free_page((unsigned long)(vcpu->arch.sie_block));
	return rc;
}

int kvm_arch_vcpu_runnable(struct kvm_vcpu *vcpu)
{
	clear_bit(vcpu->vcpu_idx, vcpu->kvm->arch.gisa_int.kicked_mask);
	return kvm_s390_vcpu_has_irq(vcpu, 0);
}

bool kvm_arch_vcpu_in_kernel(struct kvm_vcpu *vcpu)
{
	return !(vcpu->arch.sie_block->gpsw.mask & PSW_MASK_PSTATE);
}

void kvm_s390_vcpu_block(struct kvm_vcpu *vcpu)
{
	atomic_or(PROG_BLOCK_SIE, &vcpu->arch.sie_block->prog20);
	exit_sie(vcpu);
}

void kvm_s390_vcpu_unblock(struct kvm_vcpu *vcpu)
{
	atomic_andnot(PROG_BLOCK_SIE, &vcpu->arch.sie_block->prog20);
}

static void kvm_s390_vcpu_request(struct kvm_vcpu *vcpu)
{
	atomic_or(PROG_REQUEST, &vcpu->arch.sie_block->prog20);
	exit_sie(vcpu);
}

bool kvm_s390_vcpu_sie_inhibited(struct kvm_vcpu *vcpu)
{
	return atomic_read(&vcpu->arch.sie_block->prog20) &
	       (PROG_BLOCK_SIE | PROG_REQUEST);
}

static void kvm_s390_vcpu_request_handled(struct kvm_vcpu *vcpu)
{
	atomic_andnot(PROG_REQUEST, &vcpu->arch.sie_block->prog20);
}

/*
 * Kick a guest cpu out of (v)SIE and wait until (v)SIE is not running.
 * If the CPU is not running (e.g. waiting as idle) the function will
 * return immediately. */
void exit_sie(struct kvm_vcpu *vcpu)
{
	kvm_s390_set_cpuflags(vcpu, CPUSTAT_STOP_INT);
	kvm_s390_vsie_kick(vcpu);
	while (vcpu->arch.sie_block->prog0c & PROG_IN_SIE)
		cpu_relax();
}

/* Kick a guest cpu out of SIE to process a request synchronously */
void kvm_s390_sync_request(int req, struct kvm_vcpu *vcpu)
{
	__kvm_make_request(req, vcpu);
	kvm_s390_vcpu_request(vcpu);
}

static void kvm_gmap_notifier(struct gmap *gmap, unsigned long start,
			      unsigned long end)
{
	struct kvm *kvm = gmap->private;
	struct kvm_vcpu *vcpu;
	unsigned long prefix;
	unsigned long i;

	trace_kvm_s390_gmap_notifier(start, end, gmap_is_shadow(gmap));

	if (gmap_is_shadow(gmap))
		return;
	if (start >= 1UL << 31)
		/* We are only interested in prefix pages */
		return;
	kvm_for_each_vcpu(i, vcpu, kvm) {
		/* match against both prefix pages */
		prefix = kvm_s390_get_prefix(vcpu);
		if (prefix <= end && start <= prefix + 2*PAGE_SIZE - 1) {
			VCPU_EVENT(vcpu, 2, "gmap notifier for %lx-%lx",
				   start, end);
			kvm_s390_sync_request(KVM_REQ_REFRESH_GUEST_PREFIX, vcpu);
		}
	}
}

bool kvm_arch_no_poll(struct kvm_vcpu *vcpu)
{
	/* do not poll with more than halt_poll_max_steal percent of steal time */
	if (get_lowcore()->avg_steal_timer * 100 / (TICK_USEC << 12) >=
	    READ_ONCE(halt_poll_max_steal)) {
		vcpu->stat.halt_no_poll_steal++;
		return true;
	}
	return false;
}

int kvm_arch_vcpu_should_kick(struct kvm_vcpu *vcpu)
{
	/* kvm common code refers to this, but never calls it */
	BUG();
	return 0;
}

static int kvm_arch_vcpu_ioctl_get_one_reg(struct kvm_vcpu *vcpu,
					   struct kvm_one_reg *reg)
{
	int r = -EINVAL;

	switch (reg->id) {
	case KVM_REG_S390_TODPR:
		r = put_user(vcpu->arch.sie_block->todpr,
			     (u32 __user *)reg->addr);
		break;
	case KVM_REG_S390_EPOCHDIFF:
		r = put_user(vcpu->arch.sie_block->epoch,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_CPU_TIMER:
		r = put_user(kvm_s390_get_cpu_timer(vcpu),
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_CLOCK_COMP:
		r = put_user(vcpu->arch.sie_block->ckc,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_PFTOKEN:
		r = put_user(vcpu->arch.pfault_token,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_PFCOMPARE:
		r = put_user(vcpu->arch.pfault_compare,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_PFSELECT:
		r = put_user(vcpu->arch.pfault_select,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_PP:
		r = put_user(vcpu->arch.sie_block->pp,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_GBEA:
		r = put_user(vcpu->arch.sie_block->gbea,
			     (u64 __user *)reg->addr);
		break;
	default:
		break;
	}

	return r;
}

static int kvm_arch_vcpu_ioctl_set_one_reg(struct kvm_vcpu *vcpu,
					   struct kvm_one_reg *reg)
{
	int r = -EINVAL;
	__u64 val;

	switch (reg->id) {
	case KVM_REG_S390_TODPR:
		r = get_user(vcpu->arch.sie_block->todpr,
			     (u32 __user *)reg->addr);
		break;
	case KVM_REG_S390_EPOCHDIFF:
		r = get_user(vcpu->arch.sie_block->epoch,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_CPU_TIMER:
		r = get_user(val, (u64 __user *)reg->addr);
		if (!r)
			kvm_s390_set_cpu_timer(vcpu, val);
		break;
	case KVM_REG_S390_CLOCK_COMP:
		r = get_user(vcpu->arch.sie_block->ckc,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_PFTOKEN:
		r = get_user(vcpu->arch.pfault_token,
			     (u64 __user *)reg->addr);
		if (vcpu->arch.pfault_token == KVM_S390_PFAULT_TOKEN_INVALID)
			kvm_clear_async_pf_completion_queue(vcpu);
		break;
	case KVM_REG_S390_PFCOMPARE:
		r = get_user(vcpu->arch.pfault_compare,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_PFSELECT:
		r = get_user(vcpu->arch.pfault_select,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_PP:
		r = get_user(vcpu->arch.sie_block->pp,
			     (u64 __user *)reg->addr);
		break;
	case KVM_REG_S390_GBEA:
		r = get_user(vcpu->arch.sie_block->gbea,
			     (u64 __user *)reg->addr);
		break;
	default:
		break;
	}

	return r;
}

static void kvm_arch_vcpu_ioctl_normal_reset(struct kvm_vcpu *vcpu)
{
	vcpu->arch.sie_block->gpsw.mask &= ~PSW_MASK_RI;
	vcpu->arch.pfault_token = KVM_S390_PFAULT_TOKEN_INVALID;
	memset(vcpu->run->s.regs.riccb, 0, sizeof(vcpu->run->s.regs.riccb));

	kvm_clear_async_pf_completion_queue(vcpu);
	if (!kvm_s390_user_cpu_state_ctrl(vcpu->kvm))
		kvm_s390_vcpu_stop(vcpu);
	kvm_s390_clear_local_irqs(vcpu);
}

static void kvm_arch_vcpu_ioctl_initial_reset(struct kvm_vcpu *vcpu)
{
	/* Initial reset is a superset of the normal reset */
	kvm_arch_vcpu_ioctl_normal_reset(vcpu);

	/*
	 * This equals initial cpu reset in pop, but we don't switch to ESA.
	 * We do not only reset the internal data, but also ...
	 */
	vcpu->arch.sie_block->gpsw.mask = 0;
	vcpu->arch.sie_block->gpsw.addr = 0;
	kvm_s390_set_prefix(vcpu, 0);
	kvm_s390_set_cpu_timer(vcpu, 0);
	vcpu->arch.sie_block->ckc = 0;
	memset(vcpu->arch.sie_block->gcr, 0, sizeof(vcpu->arch.sie_block->gcr));
	vcpu->arch.sie_block->gcr[0] = CR0_INITIAL_MASK;
	vcpu->arch.sie_block->gcr[14] = CR14_INITIAL_MASK;

	/* ... the data in sync regs */
	memset(vcpu->run->s.regs.crs, 0, sizeof(vcpu->run->s.regs.crs));
	vcpu->run->s.regs.ckc = 0;
	vcpu->run->s.regs.crs[0] = CR0_INITIAL_MASK;
	vcpu->run->s.regs.crs[14] = CR14_INITIAL_MASK;
	vcpu->run->psw_addr = 0;
	vcpu->run->psw_mask = 0;
	vcpu->run->s.regs.todpr = 0;
	vcpu->run->s.regs.cputm = 0;
	vcpu->run->s.regs.ckc = 0;
	vcpu->run->s.regs.pp = 0;
	vcpu->run->s.regs.gbea = 1;
	vcpu->run->s.regs.fpc = 0;
	/*
	 * Do not reset these registers in the protected case, as some of
	 * them are overlaid and they are not accessible in this case
	 * anyway.
	 */
	if (!kvm_s390_pv_cpu_is_protected(vcpu)) {
		vcpu->arch.sie_block->gbea = 1;
		vcpu->arch.sie_block->pp = 0;
		vcpu->arch.sie_block->fpf &= ~FPF_BPBC;
		vcpu->arch.sie_block->todpr = 0;
	}
}

static void kvm_arch_vcpu_ioctl_clear_reset(struct kvm_vcpu *vcpu)
{
	struct kvm_sync_regs *regs = &vcpu->run->s.regs;

	/* Clear reset is a superset of the initial reset */
	kvm_arch_vcpu_ioctl_initial_reset(vcpu);

	memset(&regs->gprs, 0, sizeof(regs->gprs));
	memset(&regs->vrs, 0, sizeof(regs->vrs));
	memset(&regs->acrs, 0, sizeof(regs->acrs));
	memset(&regs->gscb, 0, sizeof(regs->gscb));

	regs->etoken = 0;
	regs->etoken_extension = 0;
}

int kvm_arch_vcpu_ioctl_set_regs(struct kvm_vcpu *vcpu, struct kvm_regs *regs)
{
	vcpu_load(vcpu);
	memcpy(&vcpu->run->s.regs.gprs, &regs->gprs, sizeof(regs->gprs));
	vcpu_put(vcpu);
	return 0;
}

int kvm_arch_vcpu_ioctl_get_regs(struct kvm_vcpu *vcpu, struct kvm_regs *regs)
{
	vcpu_load(vcpu);
	memcpy(&regs->gprs, &vcpu->run->s.regs.gprs, sizeof(regs->gprs));
	vcpu_put(vcpu);
	return 0;
}

int kvm_arch_vcpu_ioctl_set_sregs(struct kvm_vcpu *vcpu,
				  struct kvm_sregs *sregs)
{
	vcpu_load(vcpu);

	memcpy(&vcpu->run->s.regs.acrs, &sregs->acrs, sizeof(sregs->acrs));
	memcpy(&vcpu->arch.sie_block->gcr, &sregs->crs, sizeof(sregs->crs));

	vcpu_put(vcpu);
	return 0;
}

int kvm_arch_vcpu_ioctl_get_sregs(struct kvm_vcpu *vcpu,
				  struct kvm_sregs *sregs)
{
	vcpu_load(vcpu);

	memcpy(&sregs->acrs, &vcpu->run->s.regs.acrs, sizeof(sregs->acrs));
	memcpy(&sregs->crs, &vcpu->arch.sie_block->gcr, sizeof(sregs->crs));

	vcpu_put(vcpu);
	return 0;
}

int kvm_arch_vcpu_ioctl_set_fpu(struct kvm_vcpu *vcpu, struct kvm_fpu *fpu)
{
	int ret = 0;

	vcpu_load(vcpu);

	vcpu->run->s.regs.fpc = fpu->fpc;
	if (cpu_has_vx())
		convert_fp_to_vx((__vector128 *) vcpu->run->s.regs.vrs,
				 (freg_t *) fpu->fprs);
	else
		memcpy(vcpu->run->s.regs.fprs, &fpu->fprs, sizeof(fpu->fprs));

	vcpu_put(vcpu);
	return ret;
}

int kvm_arch_vcpu_ioctl_get_fpu(struct kvm_vcpu *vcpu, struct kvm_fpu *fpu)
{
	vcpu_load(vcpu);

	if (cpu_has_vx())
		convert_vx_to_fp((freg_t *) fpu->fprs,
				 (__vector128 *) vcpu->run->s.regs.vrs);
	else
		memcpy(fpu->fprs, vcpu->run->s.regs.fprs, sizeof(fpu->fprs));
	fpu->fpc = vcpu->run->s.regs.fpc;

	vcpu_put(vcpu);
	return 0;
}

static int kvm_arch_vcpu_ioctl_set_initial_psw(struct kvm_vcpu *vcpu, psw_t psw)
{
	int rc = 0;

	if (!is_vcpu_stopped(vcpu))
		rc = -EBUSY;
	else {
		vcpu->run->psw_mask = psw.mask;
		vcpu->run->psw_addr = psw.addr;
	}
	return rc;
}

int kvm_arch_vcpu_ioctl_translate(struct kvm_vcpu *vcpu,
				  struct kvm_translation *tr)
{
	return -EINVAL; /* not implemented yet */
}

#define VALID_GUESTDBG_FLAGS (KVM_GUESTDBG_SINGLESTEP | \
			      KVM_GUESTDBG_USE_HW_BP | \
			      KVM_GUESTDBG_ENABLE)

int kvm_arch_vcpu_ioctl_set_guest_debug(struct kvm_vcpu *vcpu,
					struct kvm_guest_debug *dbg)
{
	int rc = 0;

	vcpu_load(vcpu);

	vcpu->guest_debug = 0;
	kvm_s390_clear_bp_data(vcpu);

	if (dbg->control & ~VALID_GUESTDBG_FLAGS) {
		rc = -EINVAL;
		goto out;
	}
	if (!sclp.has_gpere) {
		rc = -EINVAL;
		goto out;
	}

	if (dbg->control & KVM_GUESTDBG_ENABLE) {
		vcpu->guest_debug = dbg->control;
		/* enforce guest PER */
		kvm_s390_set_cpuflags(vcpu, CPUSTAT_P);

		if (dbg->control & KVM_GUESTDBG_USE_HW_BP)
			rc = kvm_s390_import_bp_data(vcpu, dbg);
	} else {
		kvm_s390_clear_cpuflags(vcpu, CPUSTAT_P);
		vcpu->arch.guestdbg.last_bp = 0;
	}

	if (rc) {
		vcpu->guest_debug = 0;
		kvm_s390_clear_bp_data(vcpu);
		kvm_s390_clear_cpuflags(vcpu, CPUSTAT_P);
	}

out:
	vcpu_put(vcpu);
	return rc;
}

int kvm_arch_vcpu_ioctl_get_mpstate(struct kvm_vcpu *vcpu,
				    struct kvm_mp_state *mp_state)
{
	int ret;

	vcpu_load(vcpu);

	/* CHECK_STOP and LOAD are not supported yet */
	ret = is_vcpu_stopped(vcpu) ? KVM_MP_STATE_STOPPED :
				      KVM_MP_STATE_OPERATING;

	vcpu_put(vcpu);
	return ret;
}

int kvm_arch_vcpu_ioctl_set_mpstate(struct kvm_vcpu *vcpu,
				    struct kvm_mp_state *mp_state)
{
	int rc = 0;

	vcpu_load(vcpu);

	/* user space knows about this interface - let it control the state */
	kvm_s390_set_user_cpu_state_ctrl(vcpu->kvm);

	switch (mp_state->mp_state) {
	case KVM_MP_STATE_STOPPED:
		rc = kvm_s390_vcpu_stop(vcpu);
		break;
	case KVM_MP_STATE_OPERATING:
		rc = kvm_s390_vcpu_start(vcpu);
		break;
	case KVM_MP_STATE_LOAD:
		if (!kvm_s390_pv_cpu_is_protected(vcpu)) {
			rc = -ENXIO;
			break;
		}
		rc = kvm_s390_pv_set_cpu_state(vcpu, PV_CPU_STATE_OPR_LOAD);
		break;
	case KVM_MP_STATE_CHECK_STOP:
		fallthrough;	/* CHECK_STOP and LOAD are not supported yet */
	default:
		rc = -ENXIO;
	}

	vcpu_put(vcpu);
	return rc;
}

static bool ibs_enabled(struct kvm_vcpu *vcpu)
{
	return kvm_s390_test_cpuflags(vcpu, CPUSTAT_IBS);
}

static int __kvm_s390_fixup_fault_sync(struct gmap *gmap, gpa_t gaddr, unsigned int flags)
{
	struct kvm *kvm = gmap->private;
	gfn_t gfn = gpa_to_gfn(gaddr);
	bool unlocked;
	hva_t vmaddr;
	gpa_t tmp;
	int rc;

	if (kvm_is_ucontrol(kvm)) {
		tmp = __gmap_translate(gmap, gaddr);
		gfn = gpa_to_gfn(tmp);
	}

	vmaddr = gfn_to_hva(kvm, gfn);
	rc = fixup_user_fault(gmap->mm, vmaddr, FAULT_FLAG_WRITE, &unlocked);
	if (!rc)
		rc = __gmap_link(gmap, gaddr, vmaddr);
	return rc;
}

/**
 * __kvm_s390_mprotect_many() - Apply specified protection to guest pages
 * @gmap: the gmap of the guest
 * @gpa: the starting guest address
 * @npages: how many pages to protect
 * @prot: indicates access rights: PROT_NONE, PROT_READ or PROT_WRITE
 * @bits: pgste notification bits to set
 *
 * Returns: 0 in case of success, < 0 in case of error - see gmap_protect_one()
 *
 * Context: kvm->srcu and gmap->mm need to be held in read mode
 */
int __kvm_s390_mprotect_many(struct gmap *gmap, gpa_t gpa, u8 npages, unsigned int prot,
			     unsigned long bits)
{
	unsigned int fault_flag = (prot & PROT_WRITE) ? FAULT_FLAG_WRITE : 0;
	gpa_t end = gpa + npages * PAGE_SIZE;
	int rc;

	for (; gpa < end; gpa = ALIGN(gpa + 1, rc)) {
		rc = gmap_protect_one(gmap, gpa, prot, bits);
		if (rc == -EAGAIN) {
			__kvm_s390_fixup_fault_sync(gmap, gpa, fault_flag);
			rc = gmap_protect_one(gmap, gpa, prot, bits);
		}
		if (rc < 0)
			return rc;
	}

	return 0;
}

static int kvm_s390_mprotect_notify_prefix(struct kvm_vcpu *vcpu)
{
	gpa_t gaddr = kvm_s390_get_prefix(vcpu);
	int idx, rc;

	idx = srcu_read_lock(&vcpu->kvm->srcu);
	mmap_read_lock(vcpu->arch.gmap->mm);

	rc = __kvm_s390_mprotect_many(vcpu->arch.gmap, gaddr, 2, PROT_WRITE, GMAP_NOTIFY_MPROT);

	mmap_read_unlock(vcpu->arch.gmap->mm);
	srcu_read_unlock(&vcpu->kvm->srcu, idx);

	return rc;
}

static int kvm_s390_handle_requests(struct kvm_vcpu *vcpu)
{
retry:
	kvm_s390_vcpu_request_handled(vcpu);
	if (!kvm_request_pending(vcpu))
		return 0;
	/*
	 * If the guest prefix changed, re-arm the ipte notifier for the
	 * guest prefix page. gmap_mprotect_notify will wait on the ptl lock.
	 * This ensures that the ipte instruction for this request has
	 * already finished. We might race against a second unmapper that
	 * wants to set the blocking bit. Lets just retry the request loop.
	 */
	if (kvm_check_request(KVM_REQ_REFRESH_GUEST_PREFIX, vcpu)) {
		int rc;

		rc = kvm_s390_mprotect_notify_prefix(vcpu);
		if (rc) {
			kvm_make_request(KVM_REQ_REFRESH_GUEST_PREFIX, vcpu);
			return rc;
		}
		goto retry;
	}

	if (kvm_check_request(KVM_REQ_TLB_FLUSH, vcpu)) {
		vcpu->arch.sie_block->ihcpu = 0xffff;
		goto retry;
	}

	if (kvm_check_request(KVM_REQ_ENABLE_IBS, vcpu)) {
		if (!ibs_enabled(vcpu)) {
			trace_kvm_s390_enable_disable_ibs(vcpu->vcpu_id, 1);
			kvm_s390_set_cpuflags(vcpu, CPUSTAT_IBS);
		}
		goto retry;
	}

	if (kvm_check_request(KVM_REQ_DISABLE_IBS, vcpu)) {
		if (ibs_enabled(vcpu)) {
			trace_kvm_s390_enable_disable_ibs(vcpu->vcpu_id, 0);
			kvm_s390_clear_cpuflags(vcpu, CPUSTAT_IBS);
		}
		goto retry;
	}

	if (kvm_check_request(KVM_REQ_ICPT_OPEREXC, vcpu)) {
		vcpu->arch.sie_block->ictl |= ICTL_OPEREXC;
		goto retry;
	}

	if (kvm_check_request(KVM_REQ_START_MIGRATION, vcpu)) {
		/*
		 * Disable CMM virtualization; we will emulate the ESSA
		 * instruction manually, in order to provide additional
		 * functionalities needed for live migration.
		 */
		vcpu->arch.sie_block->ecb2 &= ~ECB2_CMMA;
		goto retry;
	}

	if (kvm_check_request(KVM_REQ_STOP_MIGRATION, vcpu)) {
		/*
		 * Re-enable CMM virtualization if CMMA is available and
		 * CMM has been used.
		 */
		if ((vcpu->kvm->arch.use_cmma) &&
		    (vcpu->kvm->mm->context.uses_cmm))
			vcpu->arch.sie_block->ecb2 |= ECB2_CMMA;
		goto retry;
	}

	/* we left the vsie handler, nothing to do, just clear the request */
	kvm_clear_request(KVM_REQ_VSIE_RESTART, vcpu);

	return 0;
}

static void __kvm_s390_set_tod_clock(struct kvm *kvm, const struct kvm_s390_vm_tod_clock *gtod)
{
	struct kvm_vcpu *vcpu;
	union tod_clock clk;
	unsigned long i;

	preempt_disable();

	store_tod_clock_ext(&clk);

	kvm->arch.epoch = gtod->tod - clk.tod;
	kvm->arch.epdx = 0;
	if (test_kvm_facility(kvm, 139)) {
		kvm->arch.epdx = gtod->epoch_idx - clk.ei;
		if (kvm->arch.epoch > gtod->tod)
			kvm->arch.epdx -= 1;
	}

	kvm_s390_vcpu_block_all(kvm);
	kvm_for_each_vcpu(i, vcpu, kvm) {
		vcpu->arch.sie_block->epoch = kvm->arch.epoch;
		vcpu->arch.sie_block->epdx  = kvm->arch.epdx;
	}

	kvm_s390_vcpu_unblock_all(kvm);
	preempt_enable();
}

int kvm_s390_try_set_tod_clock(struct kvm *kvm, const struct kvm_s390_vm_tod_clock *gtod)
{
	if (!mutex_trylock(&kvm->lock))
		return 0;
	__kvm_s390_set_tod_clock(kvm, gtod);
	mutex_unlock(&kvm->lock);
	return 1;
}

static void __kvm_inject_pfault_token(struct kvm_vcpu *vcpu, bool start_token,
				      unsigned long token)
{
	struct kvm_s390_interrupt inti;
	struct kvm_s390_irq irq;

	if (start_token) {
		irq.u.ext.ext_params2 = token;
		irq.type = KVM_S390_INT_PFAULT_INIT;
		WARN_ON_ONCE(kvm_s390_inject_vcpu(vcpu, &irq));
	} else {
		inti.type = KVM_S390_INT_PFAULT_DONE;
		inti.parm64 = token;
		WARN_ON_ONCE(kvm_s390_inject_vm(vcpu->kvm, &inti));
	}
}

bool kvm_arch_async_page_not_present(struct kvm_vcpu *vcpu,
				     struct kvm_async_pf *work)
{
	trace_kvm_s390_pfault_init(vcpu, work->arch.pfault_token);
	__kvm_inject_pfault_token(vcpu, true, work->arch.pfault_token);

	return true;
}

void kvm_arch_async_page_present(struct kvm_vcpu *vcpu,
				 struct kvm_async_pf *work)
{
	trace_kvm_s390_pfault_done(vcpu, work->arch.pfault_token);
	__kvm_inject_pfault_token(vcpu, false, work->arch.pfault_token);
}

void kvm_arch_async_page_ready(struct kvm_vcpu *vcpu,
			       struct kvm_async_pf *work)
{
	/* s390 will always inject the page directly */
}

bool kvm_arch_can_dequeue_async_page_present(struct kvm_vcpu *vcpu)
{
	/*
	 * s390 will always inject the page directly,
	 * but we still want check_async_completion to cleanup
	 */
	return true;
}

static bool kvm_arch_setup_async_pf(struct kvm_vcpu *vcpu)
{
	hva_t hva;
	struct kvm_arch_async_pf arch;

	if (vcpu->arch.pfault_token == KVM_S390_PFAULT_TOKEN_INVALID)
		return false;
	if ((vcpu->arch.sie_block->gpsw.mask & vcpu->arch.pfault_select) !=
	    vcpu->arch.pfault_compare)
		return false;
	if (psw_extint_disabled(vcpu))
		return false;
	if (kvm_s390_vcpu_has_irq(vcpu, 0))
		return false;
	if (!(vcpu->arch.sie_block->gcr[0] & CR0_SERVICE_SIGNAL_SUBMASK))
		return false;
	if (!vcpu->arch.gmap->pfault_enabled)
		return false;

	hva = gfn_to_hva(vcpu->kvm, current->thread.gmap_teid.addr);
	if (read_guest_real(vcpu, vcpu->arch.pfault_token, &arch.pfault_token, 8))
		return false;

	return kvm_setup_async_pf(vcpu, current->thread.gmap_teid.addr * PAGE_SIZE, hva, &arch);
}

static int vcpu_pre_run(struct kvm_vcpu *vcpu)
{
	int rc, cpuflags;

	/*
	 * On s390 notifications for arriving pages will be delivered directly
	 * to the guest but the house keeping for completed pfaults is
	 * handled outside the worker.
	 */
	kvm_check_async_pf_completion(vcpu);

	vcpu->arch.sie_block->gg14 = vcpu->run->s.regs.gprs[14];
	vcpu->arch.sie_block->gg15 = vcpu->run->s.regs.gprs[15];

	if (need_resched())
		schedule();

	if (!kvm_is_ucontrol(vcpu->kvm)) {
		rc = kvm_s390_deliver_pending_interrupts(vcpu);
		if (rc || guestdbg_exit_pending(vcpu))
			return rc;
	}

	rc = kvm_s390_handle_requests(vcpu);
	if (rc)
		return rc;

	if (guestdbg_enabled(vcpu)) {
		kvm_s390_backup_guest_per_regs(vcpu);
		kvm_s390_patch_guest_per_regs(vcpu);
	}

	clear_bit(vcpu->vcpu_idx, vcpu->kvm->arch.gisa_int.kicked_mask);

	vcpu->arch.sie_block->icptcode = 0;
	current->thread.gmap_int_code = 0;
	cpuflags = atomic_read(&vcpu->arch.sie_block->cpuflags);
	VCPU_EVENT(vcpu, 6, "entering sie flags %x", cpuflags);
	trace_kvm_s390_sie_enter(vcpu, cpuflags);

	return 0;
}

static int vcpu_post_run_addressing_exception(struct kvm_vcpu *vcpu)
{
	struct kvm_s390_pgm_info pgm_info = {
		.code = PGM_ADDRESSING,
	};
	u8 opcode, ilen;
	int rc;

	VCPU_EVENT(vcpu, 3, "%s", "fault in sie instruction");
	trace_kvm_s390_sie_fault(vcpu);

	/*
	 * We want to inject an addressing exception, which is defined as a
	 * suppressing or terminating exception. However, since we came here
	 * by a DAT access exception, the PSW still points to the faulting
	 * instruction since DAT exceptions are nullifying. So we've got
	 * to look up the current opcode to get the length of the instruction
	 * to be able to forward the PSW.
	 */
	rc = read_guest_instr(vcpu, vcpu->arch.sie_block->gpsw.addr, &opcode, 1);
	ilen = insn_length(opcode);
	if (rc < 0) {
		return rc;
	} else if (rc) {
		/* Instruction-Fetching Exceptions - we can't detect the ilen.
		 * Forward by arbitrary ilc, injection will take care of
		 * nullification if necessary.
		 */
		pgm_info = vcpu->arch.pgm;
		ilen = 4;
	}
	pgm_info.flags = ilen | KVM_S390_PGM_FLAGS_ILC_VALID;
	kvm_s390_forward_psw(vcpu, ilen);
	return kvm_s390_inject_prog_irq(vcpu, &pgm_info);
}

static void kvm_s390_assert_primary_as(struct kvm_vcpu *vcpu)
{
	KVM_BUG(current->thread.gmap_teid.as != PSW_BITS_AS_PRIMARY, vcpu->kvm,
		"Unexpected program interrupt 0x%x, TEID 0x%016lx",
		current->thread.gmap_int_code, current->thread.gmap_teid.val);
}

/*
 * __kvm_s390_handle_dat_fault() - handle a dat fault for the gmap of a vcpu
 * @vcpu: the vCPU whose gmap is to be fixed up
 * @gfn: the guest frame number used for memslots (including fake memslots)
 * @gaddr: the gmap address, does not have to match @gfn for ucontrol gmaps
 * @flags: FOLL_* flags
 *
 * Return: 0 on success, < 0 in case of error.
 * Context: The mm lock must not be held before calling. May sleep.
 */
int __kvm_s390_handle_dat_fault(struct kvm_vcpu *vcpu, gfn_t gfn, gpa_t gaddr, unsigned int flags)
{
	struct kvm_memory_slot *slot;
	unsigned int fault_flags;
	bool writable, unlocked;
	unsigned long vmaddr;
	struct page *page;
	kvm_pfn_t pfn;
	int rc;

	slot = kvm_vcpu_gfn_to_memslot(vcpu, gfn);
	if (!slot || slot->flags & KVM_MEMSLOT_INVALID)
		return vcpu_post_run_addressing_exception(vcpu);

	fault_flags = flags & FOLL_WRITE ? FAULT_FLAG_WRITE : 0;
	if (vcpu->arch.gmap->pfault_enabled)
		flags |= FOLL_NOWAIT;
	vmaddr = __gfn_to_hva_memslot(slot, gfn);

try_again:
	pfn = __kvm_faultin_pfn(slot, gfn, flags, &writable, &page);

	/* Access outside memory, inject addressing exception */
	if (is_noslot_pfn(pfn))
		return vcpu_post_run_addressing_exception(vcpu);
	/* Signal pending: try again */
	if (pfn == KVM_PFN_ERR_SIGPENDING)
		return -EAGAIN;

	/* Needs I/O, try to setup async pfault (only possible with FOLL_NOWAIT) */
	if (pfn == KVM_PFN_ERR_NEEDS_IO) {
		trace_kvm_s390_major_guest_pfault(vcpu);
		if (kvm_arch_setup_async_pf(vcpu))
			return 0;
		vcpu->stat.pfault_sync++;
		/* Could not setup async pfault, try again synchronously */
		flags &= ~FOLL_NOWAIT;
		goto try_again;
	}
	/* Any other error */
	if (is_error_pfn(pfn))
		return -EFAULT;

	/* Success */
	mmap_read_lock(vcpu->arch.gmap->mm);
	/* Mark the userspace PTEs as young and/or dirty, to avoid page fault loops */
	rc = fixup_user_fault(vcpu->arch.gmap->mm, vmaddr, fault_flags, &unlocked);
	if (!rc)
		rc = __gmap_link(vcpu->arch.gmap, gaddr, vmaddr);
	scoped_guard(spinlock, &vcpu->kvm->mmu_lock) {
		kvm_release_faultin_page(vcpu->kvm, page, false, writable);
	}
	mmap_read_unlock(vcpu->arch.gmap->mm);
	return rc;
}

static int vcpu_dat_fault_handler(struct kvm_vcpu *vcpu, unsigned long gaddr, unsigned int flags)
{
	unsigned long gaddr_tmp;
	gfn_t gfn;

	gfn = gpa_to_gfn(gaddr);
	if (kvm_is_ucontrol(vcpu->kvm)) {
		/*
		 * This translates the per-vCPU guest address into a
		 * fake guest address, which can then be used with the
		 * fake memslots that are identity mapping userspace.
		 * This allows ucontrol VMs to use the normal fault
		 * resolution path, like normal VMs.
		 */
		mmap_read_lock(vcpu->arch.gmap->mm);
		gaddr_tmp = __gmap_translate(vcpu->arch.gmap, gaddr);
		mmap_read_unlock(vcpu->arch.gmap->mm);
		if (gaddr_tmp == -EFAULT) {
			vcpu->run->exit_reason = KVM_EXIT_S390_UCONTROL;
			vcpu->run->s390_ucontrol.trans_exc_code = gaddr;
			vcpu->run->s390_ucontrol.pgm_code = PGM_SEGMENT_TRANSLATION;
			return -EREMOTE;
		}
		gfn = gpa_to_gfn(gaddr_tmp);
	}
	return __kvm_s390_handle_dat_fault(vcpu, gfn, gaddr, flags);
}

static int vcpu_post_run_handle_fault(struct kvm_vcpu *vcpu)
{
	unsigned int flags = 0;
	unsigned long gaddr;
	int rc;

	gaddr = current->thread.gmap_teid.addr * PAGE_SIZE;
	if (kvm_s390_cur_gmap_fault_is_write())
		flags = FAULT_FLAG_WRITE;

	switch (current->thread.gmap_int_code & PGM_INT_CODE_MASK) {
	case 0:
		vcpu->stat.exit_null++;
		break;
	case PGM_SECURE_STORAGE_ACCESS:
	case PGM_SECURE_STORAGE_VIOLATION:
		kvm_s390_assert_primary_as(vcpu);
		/*
		 * This can happen after a reboot with asynchronous teardown;
		 * the new guest (normal or protected) will run on top of the
		 * previous protected guest. The old pages need to be destroyed
		 * so the new guest can use them.
		 */
		if (kvm_s390_pv_destroy_page(vcpu->kvm, gaddr)) {
			/*
			 * Either KVM messed up the secure guest mapping or the
			 * same page is mapped into multiple secure guests.
			 *
			 * This exception is only triggered when a guest 2 is
			 * running and can therefore never occur in kernel
			 * context.
			 */
			pr_warn_ratelimited("Secure storage violation (%x) in task: %s, pid %d\n",
					    current->thread.gmap_int_code, current->comm,
					    current->pid);
			send_sig(SIGSEGV, current, 0);
		}
		break;
	case PGM_NON_SECURE_STORAGE_ACCESS:
		kvm_s390_assert_primary_as(vcpu);
		/*
		 * This is normal operation; a page belonging to a protected
		 * guest has not been imported yet. Try to import the page into
		 * the protected guest.
		 */
		rc = kvm_s390_pv_convert_to_secure(vcpu->kvm, gaddr);
		if (rc == -EINVAL)
			send_sig(SIGSEGV, current, 0);
		if (rc != -ENXIO)
			break;
		flags = FAULT_FLAG_WRITE;
		fallthrough;
	case PGM_PROTECTION:
	case PGM_SEGMENT_TRANSLATION:
	case PGM_PAGE_TRANSLATION:
	case PGM_ASCE_TYPE:
	case PGM_REGION_FIRST_TRANS:
	case PGM_REGION_SECOND_TRANS:
	case PGM_REGION_THIRD_TRANS:
		kvm_s390_assert_primary_as(vcpu);
		return vcpu_dat_fault_handler(vcpu, gaddr, flags);
	default:
		KVM_BUG(1, vcpu->kvm, "Unexpected program interrupt 0x%x, TEID 0x%016lx",
			current->thread.gmap_int_code, current->thread.gmap_teid.val);
		send_sig(SIGSEGV, current, 0);
		break;
	}
	return 0;
}

static int vcpu_post_run(struct kvm_vcpu *vcpu, int exit_reason)
{
	struct mcck_volatile_info *mcck_info;
	struct sie_page *sie_page;
	int rc;

	VCPU_EVENT(vcpu, 6, "exit sie icptcode %d",
		   vcpu->arch.sie_block->icptcode);
	trace_kvm_s390_sie_exit(vcpu, vcpu->arch.sie_block->icptcode);

	if (guestdbg_enabled(vcpu))
		kvm_s390_restore_guest_per_regs(vcpu);

	vcpu->run->s.regs.gprs[14] = vcpu->arch.sie_block->gg14;
	vcpu->run->s.regs.gprs[15] = vcpu->arch.sie_block->gg15;

	if (exit_reason == -EINTR) {
		VCPU_EVENT(vcpu, 3, "%s", "machine check");
		sie_page = container_of(vcpu->arch.sie_block,
					struct sie_page, sie_block);
		mcck_info = &sie_page->mcck_info;
		kvm_s390_reinject_machine_check(vcpu, mcck_info);
		return 0;
	}

	if (vcpu->arch.sie_block->icptcode > 0) {
		rc = kvm_handle_sie_intercept(vcpu);

		if (rc != -EOPNOTSUPP)
			return rc;
		vcpu->run->exit_reason = KVM_EXIT_S390_SIEIC;
		vcpu->run->s390_sieic.icptcode = vcpu->arch.sie_block->icptcode;
		vcpu->run->s390_sieic.ipa = vcpu->arch.sie_block->ipa;
		vcpu->run->s390_sieic.ipb = vcpu->arch.sie_block->ipb;
		return -EREMOTE;
	}

	return vcpu_post_run_handle_fault(vcpu);
}

int noinstr kvm_s390_enter_exit_sie(struct kvm_s390_sie_block *scb,
				    u64 *gprs, unsigned long gasce)
{
	int ret;

	guest_state_enter_irqoff();

	/*
	 * The guest_state_{enter,exit}_irqoff() functions inform lockdep and
	 * tracing that entry to the guest will enable host IRQs, and exit from
	 * the guest will disable host IRQs.
	 *
	 * We must not use lockdep/tracing/RCU in this critical section, so we
	 * use the low-level arch_local_irq_*() helpers to enable/disable IRQs.
	 */
	arch_local_irq_enable();
	ret = sie64a(scb, gprs, gasce);
	arch_local_irq_disable();

	guest_state_exit_irqoff();

	return ret;
}

#define PSW_INT_MASK (PSW_MASK_EXT | PSW_MASK_IO | PSW_MASK_MCHECK)
static int __vcpu_run(struct kvm_vcpu *vcpu)
{
	int rc, exit_reason;
	struct sie_page *sie_page = (struct sie_page *)vcpu->arch.sie_block;

	/*
	 * We try to hold kvm->srcu during most of vcpu_run (except when run-
	 * ning the guest), so that memslots (and other stuff) are protected
	 */
	kvm_vcpu_srcu_read_lock(vcpu);

	do {
		rc = vcpu_pre_run(vcpu);
		if (rc || guestdbg_exit_pending(vcpu))
			break;

		kvm_vcpu_srcu_read_unlock(vcpu);
		/*
		 * As PF_VCPU will be used in fault handler, between
		 * guest_timing_enter_irqoff and guest_timing_exit_irqoff
		 * should be no uaccess.
		 */
		if (kvm_s390_pv_cpu_is_protected(vcpu)) {
			memcpy(sie_page->pv_grregs,
			       vcpu->run->s.regs.gprs,
			       sizeof(sie_page->pv_grregs));
		}

		local_irq_disable();
		guest_timing_enter_irqoff();
		__disable_cpu_timer_accounting(vcpu);

		exit_reason = kvm_s390_enter_exit_sie(vcpu->arch.sie_block,
						      vcpu->run->s.regs.gprs,
						      vcpu->arch.gmap->asce);

		__enable_cpu_timer_accounting(vcpu);
		guest_timing_exit_irqoff();
		local_irq_enable();

		if (kvm_s390_pv_cpu_is_protected(vcpu)) {
			memcpy(vcpu->run->s.regs.gprs,
			       sie_page->pv_grregs,
			       sizeof(sie_page->pv_grregs));
			/*
			 * We're not allowed to inject interrupts on intercepts
			 * that leave the guest state in an "in-between" state
			 * where the next SIE entry will do a continuation.
			 * Fence interrupts in our "internal" PSW.
			 */
			if (vcpu->arch.sie_block->icptcode == ICPT_PV_INSTR ||
			    vcpu->arch.sie_block->icptcode == ICPT_PV_PREF) {
				vcpu->arch.sie_block->gpsw.mask &= ~PSW_INT_MASK;
			}
		}
		kvm_vcpu_srcu_read_lock(vcpu);

		rc = vcpu_post_run(vcpu, exit_reason);
	} while (!signal_pending(current) && !guestdbg_exit_pending(vcpu) && !rc);

	kvm_vcpu_srcu_read_unlock(vcpu);
	return rc;
}

static void sync_regs_fmt2(struct kvm_vcpu *vcpu)
{
	struct kvm_run *kvm_run = vcpu->run;
	struct runtime_instr_cb *riccb;
	struct gs_cb *gscb;

	riccb = (struct runtime_instr_cb *) &kvm_run->s.regs.riccb;
	gscb = (struct gs_cb *) &kvm_run->s.regs.gscb;
	vcpu->arch.sie_block->gpsw.mask = kvm_run->psw_mask;
	vcpu->arch.sie_block->gpsw.addr = kvm_run->psw_addr;
	if (kvm_run->kvm_dirty_regs & KVM_SYNC_ARCH0) {
		vcpu->arch.sie_block->todpr = kvm_run->s.regs.todpr;
		vcpu->arch.sie_block->pp = kvm_run->s.regs.pp;
		vcpu->arch.sie_block->gbea = kvm_run->s.regs.gbea;
	}
	if (kvm_run->kvm_dirty_regs & KVM_SYNC_PFAULT) {
		vcpu->arch.pfault_token = kvm_run->s.regs.pft;
		vcpu->arch.pfault_select = kvm_run->s.regs.pfs;
		vcpu->arch.pfault_compare = kvm_run->s.regs.pfc;
		if (vcpu->arch.pfault_token == KVM_S390_PFAULT_TOKEN_INVALID)
			kvm_clear_async_pf_completion_queue(vcpu);
	}
	if (kvm_run->kvm_dirty_regs & KVM_SYNC_DIAG318) {
		vcpu->arch.diag318_info.val = kvm_run->s.regs.diag318;
		vcpu->arch.sie_block->cpnc = vcpu->arch.diag318_info.cpnc;
		VCPU_EVENT(vcpu, 3, "setting cpnc to %d", vcpu->arch.diag318_info.cpnc);
	}
	/*
	 * If userspace sets the riccb (e.g. after migration) to a valid state,
	 * we should enable RI here instead of doing the lazy enablement.
	 */
	if ((kvm_run->kvm_dirty_regs & KVM_SYNC_RICCB) &&
	    test_kvm_facility(vcpu->kvm, 64) &&
	    riccb->v &&
	    !(vcpu->arch.sie_block->ecb3 & ECB3_RI)) {
		VCPU_EVENT(vcpu, 3, "%s", "ENABLE: RI (sync_regs)");
		vcpu->arch.sie_block->ecb3 |= ECB3_RI;
	}
	/*
	 * If userspace sets the gscb (e.g. after migration) to non-zero,
	 * we should enable GS here instead of doing the lazy enablement.
	 */
	if ((kvm_run->kvm_dirty_regs & KVM_SYNC_GSCB) &&
	    test_kvm_facility(vcpu->kvm, 133) &&
	    gscb->gssm &&
	    !vcpu->arch.gs_enabled) {
		VCPU_EVENT(vcpu, 3, "%s", "ENABLE: GS (sync_regs)");
		vcpu->arch.sie_block->ecb |= ECB_GS;
		vcpu->arch.sie_block->ecd |= ECD_HOSTREGMGMT;
		vcpu->arch.gs_enabled = 1;
	}
	if ((kvm_run->kvm_dirty_regs & KVM_SYNC_BPBC) &&
	    test_kvm_facility(vcpu->kvm, 82)) {
		vcpu->arch.sie_block->fpf &= ~FPF_BPBC;
		vcpu->arch.sie_block->fpf |= kvm_run->s.regs.bpbc ? FPF_BPBC : 0;
	}
	if (cpu_has_gs()) {
		preempt_disable();
		local_ctl_set_bit(2, CR2_GUARDED_STORAGE_BIT);
		if (current->thread.gs_cb) {
			vcpu->arch.host_gscb = current->thread.gs_cb;
			save_gs_cb(vcpu->arch.host_gscb);
		}
		if (vcpu->arch.gs_enabled) {
			current->thread.gs_cb = (struct gs_cb *)
						&vcpu->run->s.regs.gscb;
			restore_gs_cb(current->thread.gs_cb);
		}
		preempt_enable();
	}
	/* SIE will load etoken directly from SDNX and therefore kvm_run */
}

static void sync_regs(struct kvm_vcpu *vcpu)
{
	struct kvm_run *kvm_run = vcpu->run;

	if (kvm_run->kvm_dirty_regs & KVM_SYNC_PREFIX)
		kvm_s390_set_prefix(vcpu, kvm_run->s.regs.prefix);
	if (kvm_run->kvm_dirty_regs & KVM_SYNC_CRS) {
		memcpy(&vcpu->arch.sie_block->gcr, &kvm_run->s.regs.crs, 128);
		/* some control register changes require a tlb flush */
		kvm_make_request(KVM_REQ_TLB_FLUSH, vcpu);
	}
	if (kvm_run->kvm_dirty_regs & KVM_SYNC_ARCH0) {
		kvm_s390_set_cpu_timer(vcpu, kvm_run->s.regs.cputm);
		vcpu->arch.sie_block->ckc = kvm_run->s.regs.ckc;
	}
	save_access_regs(vcpu->arch.host_acrs);
	restore_access_regs(vcpu->run->s.regs.acrs);
	vcpu->arch.acrs_loaded = true;
	kvm_s390_fpu_load(vcpu->run);
	/* Sync fmt2 only data */
	if (likely(!kvm_s390_pv_cpu_is_protected(vcpu))) {
		sync_regs_fmt2(vcpu);
	} else {
		/*
		 * In several places we have to modify our internal view to
		 * not do things that are disallowed by the ultravisor. For
		 * example we must not inject interrupts after specific exits
		 * (e.g. 112 prefix page not secure). We do this by turning
		 * off the machine check, external and I/O interrupt bits
		 * of our PSW copy. To avoid getting validity intercepts, we
		 * do only accept the condition code from userspace.
		 */
		vcpu->arch.sie_block->gpsw.mask &= ~PSW_MASK_CC;
		vcpu->arch.sie_block->gpsw.mask |= kvm_run->psw_mask &
						   PSW_MASK_CC;
	}

	kvm_run->kvm_dirty_regs = 0;
}

static void store_regs_fmt2(struct kvm_vcpu *vcpu)
{
	struct kvm_run *kvm_run = vcpu->run;

	kvm_run->s.regs.todpr = vcpu->arch.sie_block->todpr;
	kvm_run->s.regs.pp = vcpu->arch.sie_block->pp;
	kvm_run->s.regs.gbea = vcpu->arch.sie_block->gbea;
	kvm_run->s.regs.bpbc = (vcpu->arch.sie_block->fpf & FPF_BPBC) == FPF_BPBC;
	kvm_run->s.regs.diag318 = vcpu->arch.diag318_info.val;
	if (cpu_has_gs()) {
		preempt_disable();
		local_ctl_set_bit(2, CR2_GUARDED_STORAGE_BIT);
		if (vcpu->arch.gs_enabled)
			save_gs_cb(current->thread.gs_cb);
		current->thread.gs_cb = vcpu->arch.host_gscb;
		restore_gs_cb(vcpu->arch.host_gscb);
		if (!vcpu->arch.host_gscb)
			local_ctl_clear_bit(2, CR2_GUARDED_STORAGE_BIT);
		vcpu->arch.host_gscb = NULL;
		preempt_enable();
	}
	/* SIE will save etoken directly into SDNX and therefore kvm_run */
}

static void store_regs(struct kvm_vcpu *vcpu)
{
	struct kvm_run *kvm_run = vcpu->run;

	kvm_run->psw_mask = vcpu->arch.sie_block->gpsw.mask;
	kvm_run->psw_addr = vcpu->arch.sie_block->gpsw.addr;
	kvm_run->s.regs.prefix = kvm_s390_get_prefix(vcpu);
	memcpy(&kvm_run->s.regs.crs, &vcpu->arch.sie_block->gcr, 128);
	kvm_run->s.regs.cputm = kvm_s390_get_cpu_timer(vcpu);
	kvm_run->s.regs.ckc = vcpu->arch.sie_block->ckc;
	kvm_run->s.regs.pft = vcpu->arch.pfault_token;
	kvm_run->s.regs.pfs = vcpu->arch.pfault_select;
	kvm_run->s.regs.pfc = vcpu->arch.pfault_compare;
	save_access_regs(vcpu->run->s.regs.acrs);
	restore_access_regs(vcpu->arch.host_acrs);
	vcpu->arch.acrs_loaded = false;
	kvm_s390_fpu_store(vcpu->run);
	if (likely(!kvm_s390_pv_cpu_is_protected(vcpu)))
		store_regs_fmt2(vcpu);
}

int kvm_arch_vcpu_ioctl_run(struct kvm_vcpu *vcpu)
{
	struct kvm_run *kvm_run = vcpu->run;
	DECLARE_KERNEL_FPU_ONSTACK32(fpu);
	int rc;

	/*
	 * Running a VM while dumping always has the potential to
	 * produce inconsistent dump data. But for PV vcpus a SIE
	 * entry while dumping could also lead to a fatal validity
	 * intercept which we absolutely want to avoid.
	 */
	if (vcpu->kvm->arch.pv.dumping)
		return -EINVAL;

	if (!vcpu->wants_to_run)
		return -EINTR;

	if (kvm_run->kvm_valid_regs & ~KVM_SYNC_S390_VALID_FIELDS ||
	    kvm_run->kvm_dirty_regs & ~KVM_SYNC_S390_VALID_FIELDS)
		return -EINVAL;

	vcpu_load(vcpu);

	if (guestdbg_exit_pending(vcpu)) {
		kvm_s390_prepare_debug_exit(vcpu);
		rc = 0;
		goto out;
	}

	kvm_sigset_activate(vcpu);

	/*
	 * no need to check the return value of vcpu_start as it can only have
	 * an error for protvirt, but protvirt means user cpu state
	 */
	if (!kvm_s390_user_cpu_state_ctrl(vcpu->kvm)) {
		kvm_s390_vcpu_start(vcpu);
	} else if (is_vcpu_stopped(vcpu)) {
		pr_err_ratelimited("can't run stopped vcpu %d\n",
				   vcpu->vcpu_id);
		rc = -EINVAL;
		goto out;
	}

	kernel_fpu_begin(&fpu, KERNEL_FPC | KERNEL_VXR);
	sync_regs(vcpu);
	enable_cpu_timer_accounting(vcpu);

	might_fault();
	rc = __vcpu_run(vcpu);

	if (signal_pending(current) && !rc) {
		kvm_run->exit_reason = KVM_EXIT_INTR;
		rc = -EINTR;
	}

	if (guestdbg_exit_pending(vcpu) && !rc)  {
		kvm_s390_prepare_debug_exit(vcpu);
		rc = 0;
	}

	if (rc == -EREMOTE) {
		/* userspace support is needed, kvm_run has been prepared */
		rc = 0;
	}

	disable_cpu_timer_accounting(vcpu);
	store_regs(vcpu);
	kernel_fpu_end(&fpu, KERNEL_FPC | KERNEL_VXR);

	kvm_sigset_deactivate(vcpu);

	vcpu->stat.exit_userspace++;
out:
	vcpu_put(vcpu);
	return rc;
}

/*
 * store status at address
 * we use have two special cases:
 * KVM_S390_STORE_STATUS_NOADDR: -> 0x1200 on 64 bit
 * KVM_S390_STORE_STATUS_PREFIXED: -> prefix
 */
int kvm_s390_store_status_unloaded(struct kvm_vcpu *vcpu, unsigned long gpa)
{
	unsigned char archmode = 1;
	freg_t fprs[NUM_FPRS];
	unsigned int px;
	u64 clkcomp, cputm;
	int rc;

	px = kvm_s390_get_prefix(vcpu);
	if (gpa == KVM_S390_STORE_STATUS_NOADDR) {
		if (write_guest_abs(vcpu, 163, &archmode, 1))
			return -EFAULT;
		gpa = 0;
	} else if (gpa == KVM_S390_STORE_STATUS_PREFIXED) {
		if (write_guest_real(vcpu, 163, &archmode, 1))
			return -EFAULT;
		gpa = px;
	} else
		gpa -= __LC_FPREGS_SAVE_AREA;

	/* manually convert vector registers if necessary */
	if (cpu_has_vx()) {
		convert_vx_to_fp(fprs, (__vector128 *) vcpu->run->s.regs.vrs);
		rc = write_guest_abs(vcpu, gpa + __LC_FPREGS_SAVE_AREA,
				     fprs, 128);
	} else {
		rc = write_guest_abs(vcpu, gpa + __LC_FPREGS_SAVE_AREA,
				     vcpu->run->s.regs.fprs, 128);
	}
	rc |= write_guest_abs(vcpu, gpa + __LC_GPREGS_SAVE_AREA,
			      vcpu->run->s.regs.gprs, 128);
	rc |= write_guest_abs(vcpu, gpa + __LC_PSW_SAVE_AREA,
			      &vcpu->arch.sie_block->gpsw, 16);
	rc |= write_guest_abs(vcpu, gpa + __LC_PREFIX_SAVE_AREA,
			      &px, 4);
	rc |= write_guest_abs(vcpu, gpa + __LC_FP_CREG_SAVE_AREA,
			      &vcpu->run->s.regs.fpc, 4);
	rc |= write_guest_abs(vcpu, gpa + __LC_TOD_PROGREG_SAVE_AREA,
			      &vcpu->arch.sie_block->todpr, 4);
	cputm = kvm_s390_get_cpu_timer(vcpu);
	rc |= write_guest_abs(vcpu, gpa + __LC_CPU_TIMER_SAVE_AREA,
			      &cputm, 8);
	clkcomp = vcpu->arch.sie_block->ckc >> 8;
	rc |= write_guest_abs(vcpu, gpa + __LC_CLOCK_COMP_SAVE_AREA,
			      &clkcomp, 8);
	rc |= write_guest_abs(vcpu, gpa + __LC_AREGS_SAVE_AREA,
			      &vcpu->run->s.regs.acrs, 64);
	rc |= write_guest_abs(vcpu, gpa + __LC_CREGS_SAVE_AREA,
			      &vcpu->arch.sie_block->gcr, 128);
	return rc ? -EFAULT : 0;
}

int kvm_s390_vcpu_store_status(struct kvm_vcpu *vcpu, unsigned long addr)
{
	/*
	 * The guest FPRS and ACRS are in the host FPRS/ACRS due to the lazy
	 * switch in the run ioctl. Let's update our copies before we save
	 * it into the save area
	 */
	kvm_s390_fpu_store(vcpu->run);
	save_access_regs(vcpu->run->s.regs.acrs);

	return kvm_s390_store_status_unloaded(vcpu, addr);
}

static void __disable_ibs_on_vcpu(struct kvm_vcpu *vcpu)
{
	kvm_check_request(KVM_REQ_ENABLE_IBS, vcpu);
	kvm_s390_sync_request(KVM_REQ_DISABLE_IBS, vcpu);
}

static void __disable_ibs_on_all_vcpus(struct kvm *kvm)
{
	unsigned long i;
	struct kvm_vcpu *vcpu;

	kvm_for_each_vcpu(i, vcpu, kvm) {
		__disable_ibs_on_vcpu(vcpu);
	}
}

static void __enable_ibs_on_vcpu(struct kvm_vcpu *vcpu)
{
	if (!sclp.has_ibs)
		return;
	kvm_check_request(KVM_REQ_DISABLE_IBS, vcpu);
	kvm_s390_sync_request(KVM_REQ_ENABLE_IBS, vcpu);
}

int kvm_s390_vcpu_start(struct kvm_vcpu *vcpu)
{
	int i, online_vcpus, r = 0, started_vcpus = 0;

	if (!is_vcpu_stopped(vcpu))
		return 0;

	trace_kvm_s390_vcpu_start_stop(vcpu->vcpu_id, 1);
	/* Only one cpu at a time may enter/leave the STOPPED state. */
	spin_lock(&vcpu->kvm->arch.start_stop_lock);
	online_vcpus = atomic_read(&vcpu->kvm->online_vcpus);

	/* Let's tell the UV that we want to change into the operating state */
	if (kvm_s390_pv_cpu_is_protected(vcpu)) {
		r = kvm_s390_pv_set_cpu_state(vcpu, PV_CPU_STATE_OPR);
		if (r) {
			spin_unlock(&vcpu->kvm->arch.start_stop_lock);
			return r;
		}
	}

	for (i = 0; i < online_vcpus; i++) {
		if (!is_vcpu_stopped(kvm_get_vcpu(vcpu->kvm, i)))
			started_vcpus++;
	}

	if (started_vcpus == 0) {
		/* we're the only active VCPU -> speed it up */
		__enable_ibs_on_vcpu(vcpu);
	} else if (started_vcpus == 1) {
		/*
		 * As we are starting a second VCPU, we have to disable
		 * the IBS facility on all VCPUs to remove potentially
		 * outstanding ENABLE requests.
		 */
		__disable_ibs_on_all_vcpus(vcpu->kvm);
	}

	kvm_s390_clear_cpuflags(vcpu, CPUSTAT_STOPPED);
	/*
	 * The real PSW might have changed due to a RESTART interpreted by the
	 * ultravisor. We block all interrupts and let the next sie exit
	 * refresh our view.
	 */
	if (kvm_s390_pv_cpu_is_protected(vcpu))
		vcpu->arch.sie_block->gpsw.mask &= ~PSW_INT_MASK;
	/*
	 * Another VCPU might have used IBS while we were offline.
	 * Let's play safe and flush the VCPU at startup.
	 */
	kvm_make_request(KVM_REQ_TLB_FLUSH, vcpu);
	spin_unlock(&vcpu->kvm->arch.start_stop_lock);
	return 0;
}

int kvm_s390_vcpu_stop(struct kvm_vcpu *vcpu)
{
	int i, online_vcpus, r = 0, started_vcpus = 0;
	struct kvm_vcpu *started_vcpu = NULL;

	if (is_vcpu_stopped(vcpu))
		return 0;

	trace_kvm_s390_vcpu_start_stop(vcpu->vcpu_id, 0);
	/* Only one cpu at a time may enter/leave the STOPPED state. */
	spin_lock(&vcpu->kvm->arch.start_stop_lock);
	online_vcpus = atomic_read(&vcpu->kvm->online_vcpus);

	/* Let's tell the UV that we want to change into the stopped state */
	if (kvm_s390_pv_cpu_is_protected(vcpu)) {
		r = kvm_s390_pv_set_cpu_state(vcpu, PV_CPU_STATE_STP);
		if (r) {
			spin_unlock(&vcpu->kvm->arch.start_stop_lock);
			return r;
		}
	}

	/*
	 * Set the VCPU to STOPPED and THEN clear the interrupt flag,
	 * now that the SIGP STOP and SIGP STOP AND STORE STATUS orders
	 * have been fully processed. This will ensure that the VCPU
	 * is kept BUSY if another VCPU is inquiring with SIGP SENSE.
	 */
	kvm_s390_set_cpuflags(vcpu, CPUSTAT_STOPPED);
	kvm_s390_clear_stop_irq(vcpu);

	__disable_ibs_on_vcpu(vcpu);

	for (i = 0; i < online_vcpus; i++) {
		struct kvm_vcpu *tmp = kvm_get_vcpu(vcpu->kvm, i);

		if (!is_vcpu_stopped(tmp)) {
			started_vcpus++;
			started_vcpu = tmp;
		}
	}

	if (started_vcpus == 1) {
		/*
		 * As we only have one VCPU left, we want to enable the
		 * IBS facility for that VCPU to speed it up.
		 */
		__enable_ibs_on_vcpu(started_vcpu);
	}

	spin_unlock(&vcpu->kvm->arch.start_stop_lock);
	return 0;
}

static int kvm_vcpu_ioctl_enable_cap(struct kvm_vcpu *vcpu,
				     struct kvm_enable_cap *cap)
{
	int r;

	if (cap->flags)
		return -EINVAL;

	switch (cap->cap) {
	case KVM_CAP_S390_CSS_SUPPORT:
		if (!vcpu->kvm->arch.css_support) {
			vcpu->kvm->arch.css_support = 1;
			VM_EVENT(vcpu->kvm, 3, "%s", "ENABLE: CSS support");
			trace_kvm_s390_enable_css(vcpu->kvm);
		}
		r = 0;
		break;
	default:
		r = -EINVAL;
		break;
	}
	return r;
}

static long kvm_s390_vcpu_sida_op(struct kvm_vcpu *vcpu,
				  struct kvm_s390_mem_op *mop)
{
	void __user *uaddr = (void __user *)mop->buf;
	void *sida_addr;
	int r = 0;

	if (mop->flags || !mop->size)
		return -EINVAL;
	if (mop->size + mop->sida_offset < mop->size)
		return -EINVAL;
	if (mop->size + mop->sida_offset > sida_size(vcpu->arch.sie_block))
		return -E2BIG;
	if (!kvm_s390_pv_cpu_is_protected(vcpu))
		return -EINVAL;

	sida_addr = (char *)sida_addr(vcpu->arch.sie_block) + mop->sida_offset;

	switch (mop->op) {
	case KVM_S390_MEMOP_SIDA_READ:
		if (copy_to_user(uaddr, sida_addr, mop->size))
			r = -EFAULT;

		break;
	case KVM_S390_MEMOP_SIDA_WRITE:
		if (copy_from_user(sida_addr, uaddr, mop->size))
			r = -EFAULT;
		break;
	}
	return r;
}

static long kvm_s390_vcpu_mem_op(struct kvm_vcpu *vcpu,
				 struct kvm_s390_mem_op *mop)
{
	void __user *uaddr = (void __user *)mop->buf;
	enum gacc_mode acc_mode;
	void *tmpbuf = NULL;
	int r;

	r = mem_op_validate_common(mop, KVM_S390_MEMOP_F_INJECT_EXCEPTION |
					KVM_S390_MEMOP_F_CHECK_ONLY |
					KVM_S390_MEMOP_F_SKEY_PROTECTION);
	if (r)
		return r;
	if (mop->ar >= NUM_ACRS)
		return -EINVAL;
	if (kvm_s390_pv_cpu_is_protected(vcpu))
		return -EINVAL;
	if (!(mop->flags & KVM_S390_MEMOP_F_CHECK_ONLY)) {
		tmpbuf = vmalloc(mop->size);
		if (!tmpbuf)
			return -ENOMEM;
	}

	acc_mode = mop->op == KVM_S390_MEMOP_LOGICAL_READ ? GACC_FETCH : GACC_STORE;
	if (mop->flags & KVM_S390_MEMOP_F_CHECK_ONLY) {
		r = check_gva_range(vcpu, mop->gaddr, mop->ar, mop->size,
				    acc_mode, mop->key);
		goto out_inject;
	}
	if (acc_mode == GACC_FETCH) {
		r = read_guest_with_key(vcpu, mop->gaddr, mop->ar, tmpbuf,
					mop->size, mop->key);
		if (r)
			goto out_inject;
		if (copy_to_user(uaddr, tmpbuf, mop->size)) {
			r = -EFAULT;
			goto out_free;
		}
	} else {
		if (copy_from_user(tmpbuf, uaddr, mop->size)) {
			r = -EFAULT;
			goto out_free;
		}
		r = write_guest_with_key(vcpu, mop->gaddr, mop->ar, tmpbuf,
					 mop->size, mop->key);
	}

out_inject:
	if (r > 0 && (mop->flags & KVM_S390_MEMOP_F_INJECT_EXCEPTION) != 0)
		kvm_s390_inject_prog_irq(vcpu, &vcpu->arch.pgm);

out_free:
	vfree(tmpbuf);
	return r;
}

static long kvm_s390_vcpu_memsida_op(struct kvm_vcpu *vcpu,
				     struct kvm_s390_mem_op *mop)
{
	int r, srcu_idx;

	srcu_idx = srcu_read_lock(&vcpu->kvm->srcu);

	switch (mop->op) {
	case KVM_S390_MEMOP_LOGICAL_READ:
	case KVM_S390_MEMOP_LOGICAL_WRITE:
		r = kvm_s390_vcpu_mem_op(vcpu, mop);
		break;
	case KVM_S390_MEMOP_SIDA_READ:
	case KVM_S390_MEMOP_SIDA_WRITE:
		/* we are locked against sida going away by the vcpu->mutex */
		r = kvm_s390_vcpu_sida_op(vcpu, mop);
		break;
	default:
		r = -EINVAL;
	}

	srcu_read_unlock(&vcpu->kvm->srcu, srcu_idx);
	return r;
}

long kvm_arch_vcpu_async_ioctl(struct file *filp,
			       unsigned int ioctl, unsigned long arg)
{
	struct kvm_vcpu *vcpu = filp->private_data;
	void __user *argp = (void __user *)arg;
	int rc;

	switch (ioctl) {
	case KVM_S390_IRQ: {
		struct kvm_s390_irq s390irq;

		if (copy_from_user(&s390irq, argp, sizeof(s390irq)))
			return -EFAULT;
		rc = kvm_s390_inject_vcpu(vcpu, &s390irq);
		break;
	}
	case KVM_S390_INTERRUPT: {
		struct kvm_s390_interrupt s390int;
		struct kvm_s390_irq s390irq = {};

		if (copy_from_user(&s390int, argp, sizeof(s390int)))
			return -EFAULT;
		if (s390int_to_s390irq(&s390int, &s390irq))
			return -EINVAL;
		rc = kvm_s390_inject_vcpu(vcpu, &s390irq);
		break;
	}
	default:
		rc = -ENOIOCTLCMD;
		break;
	}

	/*
	 * To simplify single stepping of userspace-emulated instructions,
	 * KVM_EXIT_S390_SIEIC exit sets KVM_GUESTDBG_EXIT_PENDING (see
	 * should_handle_per_ifetch()). However, if userspace emulation injects
	 * an interrupt, it needs to be cleared, so that KVM_EXIT_DEBUG happens
	 * after (and not before) the interrupt delivery.
	 */
	if (!rc)
		vcpu->guest_debug &= ~KVM_GUESTDBG_EXIT_PENDING;

	return rc;
}

static int kvm_s390_handle_pv_vcpu_dump(struct kvm_vcpu *vcpu,
					struct kvm_pv_cmd *cmd)
{
	struct kvm_s390_pv_dmp dmp;
	void *data;
	int ret;

	/* Dump initialization is a prerequisite */
	if (!vcpu->kvm->arch.pv.dumping)
		return -EINVAL;

	if (copy_from_user(&dmp, (__u8 __user *)cmd->data, sizeof(dmp)))
		return -EFAULT;

	/* We only handle this subcmd right now */
	if (dmp.subcmd != KVM_PV_DUMP_CPU)
		return -EINVAL;

	/* CPU dump length is the same as create cpu storage donation. */
	if (dmp.buff_len != uv_info.guest_cpu_stor_len)
		return -EINVAL;

	data = kvzalloc(uv_info.guest_cpu_stor_len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = kvm_s390_pv_dump_cpu(vcpu, data, &cmd->rc, &cmd->rrc);

	VCPU_EVENT(vcpu, 3, "PROTVIRT DUMP CPU %d rc %x rrc %x",
		   vcpu->vcpu_id, cmd->rc, cmd->rrc);

	if (ret)
		ret = -EINVAL;

	/* On success copy over the dump data */
	if (!ret && copy_to_user((__u8 __user *)dmp.buff_addr, data, uv_info.guest_cpu_stor_len))
		ret = -EFAULT;

	kvfree(data);
	return ret;
}

long kvm_arch_vcpu_ioctl(struct file *filp,
			 unsigned int ioctl, unsigned long arg)
{
	struct kvm_vcpu *vcpu = filp->private_data;
	void __user *argp = (void __user *)arg;
	int idx;
	long r;
	u16 rc, rrc;

	vcpu_load(vcpu);

	switch (ioctl) {
	case KVM_S390_STORE_STATUS:
		idx = srcu_read_lock(&vcpu->kvm->srcu);
		r = kvm_s390_store_status_unloaded(vcpu, arg);
		srcu_read_unlock(&vcpu->kvm->srcu, idx);
		break;
	case KVM_S390_SET_INITIAL_PSW: {
		psw_t psw;

		r = -EFAULT;
		if (copy_from_user(&psw, argp, sizeof(psw)))
			break;
		r = kvm_arch_vcpu_ioctl_set_initial_psw(vcpu, psw);
		break;
	}
	case KVM_S390_CLEAR_RESET:
		r = 0;
		kvm_arch_vcpu_ioctl_clear_reset(vcpu);
		if (kvm_s390_pv_cpu_is_protected(vcpu)) {
			r = uv_cmd_nodata(kvm_s390_pv_cpu_get_handle(vcpu),
					  UVC_CMD_CPU_RESET_CLEAR, &rc, &rrc);
			VCPU_EVENT(vcpu, 3, "PROTVIRT RESET CLEAR VCPU: rc %x rrc %x",
				   rc, rrc);
		}
		break;
	case KVM_S390_INITIAL_RESET:
		r = 0;
		kvm_arch_vcpu_ioctl_initial_reset(vcpu);
		if (kvm_s390_pv_cpu_is_protected(vcpu)) {
			r = uv_cmd_nodata(kvm_s390_pv_cpu_get_handle(vcpu),
					  UVC_CMD_CPU_RESET_INITIAL,
					  &rc, &rrc);
			VCPU_EVENT(vcpu, 3, "PROTVIRT RESET INITIAL VCPU: rc %x rrc %x",
				   rc, rrc);
		}
		break;
	case KVM_S390_NORMAL_RESET:
		r = 0;
		kvm_arch_vcpu_ioctl_normal_reset(vcpu);
		if (kvm_s390_pv_cpu_is_protected(vcpu)) {
			r = uv_cmd_nodata(kvm_s390_pv_cpu_get_handle(vcpu),
					  UVC_CMD_CPU_RESET, &rc, &rrc);
			VCPU_EVENT(vcpu, 3, "PROTVIRT RESET NORMAL VCPU: rc %x rrc %x",
				   rc, rrc);
		}
		break;
	case KVM_SET_ONE_REG:
	case KVM_GET_ONE_REG: {
		struct kvm_one_reg reg;
		r = -EINVAL;
		if (kvm_s390_pv_cpu_is_protected(vcpu))
			break;
		r = -EFAULT;
		if (copy_from_user(&reg, argp, sizeof(reg)))
			break;
		if (ioctl == KVM_SET_ONE_REG)
			r = kvm_arch_vcpu_ioctl_set_one_reg(vcpu, &reg);
		else
			r = kvm_arch_vcpu_ioctl_get_one_reg(vcpu, &reg);
		break;
	}
#ifdef CONFIG_KVM_S390_UCONTROL
	case KVM_S390_UCAS_MAP: {
		struct kvm_s390_ucas_mapping ucasmap;

		if (copy_from_user(&ucasmap, argp, sizeof(ucasmap))) {
			r = -EFAULT;
			break;
		}

		if (!kvm_is_ucontrol(vcpu->kvm)) {
			r = -EINVAL;
			break;
		}

		r = gmap_map_segment(vcpu->arch.gmap, ucasmap.user_addr,
				     ucasmap.vcpu_addr, ucasmap.length);
		break;
	}
	case KVM_S390_UCAS_UNMAP: {
		struct kvm_s390_ucas_mapping ucasmap;

		if (copy_from_user(&ucasmap, argp, sizeof(ucasmap))) {
			r = -EFAULT;
			break;
		}

		if (!kvm_is_ucontrol(vcpu->kvm)) {
			r = -EINVAL;
			break;
		}

		r = gmap_unmap_segment(vcpu->arch.gmap, ucasmap.vcpu_addr,
			ucasmap.length);
		break;
	}
#endif
	case KVM_S390_VCPU_FAULT: {
		idx = srcu_read_lock(&vcpu->kvm->srcu);
		r = vcpu_dat_fault_handler(vcpu, arg, 0);
		srcu_read_unlock(&vcpu->kvm->srcu, idx);
		break;
	}
	case KVM_ENABLE_CAP:
	{
		struct kvm_enable_cap cap;
		r = -EFAULT;
		if (copy_from_user(&cap, argp, sizeof(cap)))
			break;
		r = kvm_vcpu_ioctl_enable_cap(vcpu, &cap);
		break;
	}
	case KVM_S390_MEM_OP: {
		struct kvm_s390_mem_op mem_op;

		if (copy_from_user(&mem_op, argp, sizeof(mem_op)) == 0)
			r = kvm_s390_vcpu_memsida_op(vcpu, &mem_op);
		else
			r = -EFAULT;
		break;
	}
	case KVM_S390_SET_IRQ_STATE: {
		struct kvm_s390_irq_state irq_state;

		r = -EFAULT;
		if (copy_from_user(&irq_state, argp, sizeof(irq_state)))
			break;
		if (irq_state.len > VCPU_IRQS_MAX_BUF ||
		    irq_state.len == 0 ||
		    irq_state.len % sizeof(struct kvm_s390_irq) > 0) {
			r = -EINVAL;
			break;
		}
		/* do not use irq_state.flags, it will break old QEMUs */
		r = kvm_s390_set_irq_state(vcpu,
					   (void __user *) irq_state.buf,
					   irq_state.len);
		break;
	}
	case KVM_S390_GET_IRQ_STATE: {
		struct kvm_s390_irq_state irq_state;

		r = -EFAULT;
		if (copy_from_user(&irq_state, argp, sizeof(irq_state)))
			break;
		if (irq_state.len == 0) {
			r = -EINVAL;
			break;
		}
		/* do not use irq_state.flags, it will break old QEMUs */
		r = kvm_s390_get_irq_state(vcpu,
					   (__u8 __user *)  irq_state.buf,
					   irq_state.len);
		break;
	}
	case KVM_S390_PV_CPU_COMMAND: {
		struct kvm_pv_cmd cmd;

		r = -EINVAL;
		if (!is_prot_virt_host())
			break;

		r = -EFAULT;
		if (copy_from_user(&cmd, argp, sizeof(cmd)))
			break;

		r = -EINVAL;
		if (cmd.flags)
			break;

		/* We only handle this cmd right now */
		if (cmd.cmd != KVM_PV_DUMP)
			break;

		r = kvm_s390_handle_pv_vcpu_dump(vcpu, &cmd);

		/* Always copy over UV rc / rrc data */
		if (copy_to_user((__u8 __user *)argp, &cmd.rc,
				 sizeof(cmd.rc) + sizeof(cmd.rrc)))
			r = -EFAULT;
		break;
	}
	default:
		r = -ENOTTY;
	}

	vcpu_put(vcpu);
	return r;
}

vm_fault_t kvm_arch_vcpu_fault(struct kvm_vcpu *vcpu, struct vm_fault *vmf)
{
#ifdef CONFIG_KVM_S390_UCONTROL
	if ((vmf->pgoff == KVM_S390_SIE_PAGE_OFFSET)
		 && (kvm_is_ucontrol(vcpu->kvm))) {
		vmf->page = virt_to_page(vcpu->arch.sie_block);
		get_page(vmf->page);
		return 0;
	}
#endif
	return VM_FAULT_SIGBUS;
}

bool kvm_arch_irqchip_in_kernel(struct kvm *kvm)
{
	return true;
}

/* Section: memory related */
int kvm_arch_prepare_memory_region(struct kvm *kvm,
				   const struct kvm_memory_slot *old,
				   struct kvm_memory_slot *new,
				   enum kvm_mr_change change)
{
	gpa_t size;

	if (kvm_is_ucontrol(kvm) && new->id < KVM_USER_MEM_SLOTS)
		return -EINVAL;

	/* When we are protected, we should not change the memory slots */
	if (kvm_s390_pv_get_handle(kvm))
		return -EINVAL;

	if (change != KVM_MR_DELETE && change != KVM_MR_FLAGS_ONLY) {
		/*
		 * A few sanity checks. We can have memory slots which have to be
		 * located/ended at a segment boundary (1MB). The memory in userland is
		 * ok to be fragmented into various different vmas. It is okay to mmap()
		 * and munmap() stuff in this slot after doing this call at any time
		 */

		if (new->userspace_addr & 0xffffful)
			return -EINVAL;

		size = new->npages * PAGE_SIZE;
		if (size & 0xffffful)
			return -EINVAL;

		if ((new->base_gfn * PAGE_SIZE) + size > kvm->arch.mem_limit)
			return -EINVAL;
	}

	if (!kvm->arch.migration_mode)
		return 0;

	/*
	 * Turn off migration mode when:
	 * - userspace creates a new memslot with dirty logging off,
	 * - userspace modifies an existing memslot (MOVE or FLAGS_ONLY) and
	 *   dirty logging is turned off.
	 * Migration mode expects dirty page logging being enabled to store
	 * its dirty bitmap.
	 */
	if (change != KVM_MR_DELETE &&
	    !(new->flags & KVM_MEM_LOG_DIRTY_PAGES))
		WARN(kvm_s390_vm_stop_migration(kvm),
		     "Failed to stop migration mode");

	return 0;
}

void kvm_arch_commit_memory_region(struct kvm *kvm,
				struct kvm_memory_slot *old,
				const struct kvm_memory_slot *new,
				enum kvm_mr_change change)
{
	int rc = 0;

	if (kvm_is_ucontrol(kvm))
		return;

	switch (change) {
	case KVM_MR_DELETE:
		rc = gmap_unmap_segment(kvm->arch.gmap, old->base_gfn * PAGE_SIZE,
					old->npages * PAGE_SIZE);
		break;
	case KVM_MR_MOVE:
		rc = gmap_unmap_segment(kvm->arch.gmap, old->base_gfn * PAGE_SIZE,
					old->npages * PAGE_SIZE);
		if (rc)
			break;
		fallthrough;
	case KVM_MR_CREATE:
		rc = gmap_map_segment(kvm->arch.gmap, new->userspace_addr,
				      new->base_gfn * PAGE_SIZE,
				      new->npages * PAGE_SIZE);
		break;
	case KVM_MR_FLAGS_ONLY:
		break;
	default:
		WARN(1, "Unknown KVM MR CHANGE: %d\n", change);
	}
	if (rc)
		pr_warn("failed to commit memory region\n");
	return;
}

static inline unsigned long nonhyp_mask(int i)
{
	unsigned int nonhyp_fai = (sclp.hmfai << i * 2) >> 30;

	return 0x0000ffffffffffffUL >> (nonhyp_fai << 4);
}

static int __init kvm_s390_init(void)
{
	int i, r;

	if (!sclp.has_sief2) {
		pr_info("SIE is not available\n");
		return -ENODEV;
	}

	if (nested && hpage) {
		pr_info("A KVM host that supports nesting cannot back its KVM guests with huge pages\n");
		return -EINVAL;
	}

	for (i = 0; i < 16; i++)
		kvm_s390_fac_base[i] |=
			stfle_fac_list[i] & nonhyp_mask(i);

	r = __kvm_s390_init();
	if (r)
		return r;

	r = kvm_init(sizeof(struct kvm_vcpu), 0, THIS_MODULE);
	if (r) {
		__kvm_s390_exit();
		return r;
	}
	return 0;
}

static void __exit kvm_s390_exit(void)
{
	kvm_exit();

	__kvm_s390_exit();
}

module_init(kvm_s390_init);
module_exit(kvm_s390_exit);

/*
 * Enable autoloading of the kvm module.
 * Note that we add the module alias here instead of virt/kvm/kvm_main.c
 * since x86 takes a different approach.
 */
#include <linux/miscdevice.h>
MODULE_ALIAS_MISCDEV(KVM_MINOR);
MODULE_ALIAS("devname:kvm");
