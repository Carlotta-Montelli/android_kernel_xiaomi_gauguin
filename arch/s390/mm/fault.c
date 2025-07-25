// SPDX-License-Identifier: GPL-2.0
/*
 *  S390 version
 *    Copyright IBM Corp. 1999
 *    Author(s): Hartmut Penner (hp@de.ibm.com)
 *               Ulrich Weigand (uweigand@de.ibm.com)
 *
 *  Derived from "arch/i386/mm/fault.c"
 *    Copyright (C) 1995  Linus Torvalds
 */

#include <linux/kernel_stat.h>
#include <linux/perf_event.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/compat.h>
#include <linux/smp.h>
#include <linux/kdebug.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/extable.h>
#include <linux/hardirq.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/hugetlb.h>
#include <asm/asm-offsets.h>
#include <asm/diag.h>
#include <asm/pgtable.h>
#include <asm/gmap.h>
#include <asm/irq.h>
#include <asm/mmu_context.h>
#include <asm/facility.h>
#include "../kernel/entry.h"

#define __FAIL_ADDR_MASK -4096L
#define __SUBCODE_MASK 0x0600
#define __PF_RES_FIELD 0x8000000000000000ULL

#define VM_FAULT_BADCONTEXT	0x010000
#define VM_FAULT_BADMAP		0x020000
#define VM_FAULT_BADACCESS	0x040000
#define VM_FAULT_SIGNAL		0x080000
#define VM_FAULT_PFAULT		0x100000

enum fault_type {
	KERNEL_FAULT,
	USER_FAULT,
	VDSO_FAULT,
	GMAP_FAULT,
};

static unsigned long store_indication __read_mostly;

static int __init fault_init(void)
{
	if (test_facility(75))
		store_indication = 0xc00;
	return 0;
}
early_initcall(fault_init);

static inline int notify_page_fault(struct pt_regs *regs)
{
	int ret = 0;

	/* kprobe_running() needs smp_processor_id() */
	if (kprobes_built_in() && !user_mode(regs)) {
		preempt_disable();
		if (kprobe_running() && kprobe_fault_handler(regs, 14))
			ret = 1;
		preempt_enable();
	}
	return ret;
}


/*
 * Unlock any spinlocks which will prevent us from getting the
 * message out.
 */
void bust_spinlocks(int yes)
{
	if (yes) {
		oops_in_progress = 1;
	} else {
		int loglevel_save = console_loglevel;
		console_unblank();
		oops_in_progress = 0;
		/*
		 * OK, the message is on the console.  Now we call printk()
		 * without oops_in_progress set so that printk will give klogd
		 * a poke.  Hold onto your hats...
		 */
		console_loglevel = 15;
		printk(" ");
		console_loglevel = loglevel_save;
	}
}

/*
 * Find out which address space caused the exception.
 */
static inline enum fault_type get_fault_type(struct pt_regs *regs)
{
	unsigned long trans_exc_code;

	trans_exc_code = regs->int_parm_long & 3;
	if (likely(trans_exc_code == 0)) {
		/* primary space exception */
		if (IS_ENABLED(CONFIG_PGSTE) &&
		    test_pt_regs_flag(regs, PIF_GUEST_FAULT))
			return GMAP_FAULT;
		if (current->thread.mm_segment == USER_DS)
			return USER_FAULT;
		return KERNEL_FAULT;
	}
	if (trans_exc_code == 2) {
		/* secondary space exception */
		if (current->thread.mm_segment & 1) {
			if (current->thread.mm_segment == USER_DS_SACF)
				return USER_FAULT;
			return KERNEL_FAULT;
		}
		return VDSO_FAULT;
	}
	if (trans_exc_code == 1) {
		/* access register mode, not used in the kernel */
		return USER_FAULT;
	}
	/* home space exception -> access via kernel ASCE */
	return KERNEL_FAULT;
}

static int bad_address(void *p)
{
	unsigned long dummy;

	return probe_kernel_address((unsigned long *)p, dummy);
}

static void dump_pagetable(unsigned long asce, unsigned long address)
{
	unsigned long *table = __va(asce & _ASCE_ORIGIN);

	pr_alert("AS:%016lx ", asce);
	switch (asce & _ASCE_TYPE_MASK) {
	case _ASCE_TYPE_REGION1:
		table += (address & _REGION1_INDEX) >> _REGION1_SHIFT;
		if (bad_address(table))
			goto bad;
		pr_cont("R1:%016lx ", *table);
		if (*table & _REGION_ENTRY_INVALID)
			goto out;
		table = (unsigned long *)(*table & _REGION_ENTRY_ORIGIN);
		/* fallthrough */
	case _ASCE_TYPE_REGION2:
		table += (address & _REGION2_INDEX) >> _REGION2_SHIFT;
		if (bad_address(table))
			goto bad;
		pr_cont("R2:%016lx ", *table);
		if (*table & _REGION_ENTRY_INVALID)
			goto out;
		table = (unsigned long *)(*table & _REGION_ENTRY_ORIGIN);
		/* fallthrough */
	case _ASCE_TYPE_REGION3:
		table += (address & _REGION3_INDEX) >> _REGION3_SHIFT;
		if (bad_address(table))
			goto bad;
		pr_cont("R3:%016lx ", *table);
		if (*table & (_REGION_ENTRY_INVALID | _REGION3_ENTRY_LARGE))
			goto out;
		table = (unsigned long *)(*table & _REGION_ENTRY_ORIGIN);
		/* fallthrough */
	case _ASCE_TYPE_SEGMENT:
		table += (address & _SEGMENT_INDEX) >> _SEGMENT_SHIFT;
		if (bad_address(table))
			goto bad;
		pr_cont("S:%016lx ", *table);
		if (*table & (_SEGMENT_ENTRY_INVALID | _SEGMENT_ENTRY_LARGE))
			goto out;
		table = (unsigned long *)(*table & _SEGMENT_ENTRY_ORIGIN);
	}
	table += (address & _PAGE_INDEX) >> _PAGE_SHIFT;
	if (bad_address(table))
		goto bad;
	pr_cont("P:%016lx ", *table);
out:
	pr_cont("\n");
	return;
bad:
	pr_cont("BAD\n");
}

static void dump_fault_info(struct pt_regs *regs)
{
	unsigned long asce;

	pr_alert("Failing address: %016lx TEID: %016lx\n",
		 regs->int_parm_long & __FAIL_ADDR_MASK, regs->int_parm_long);
	pr_alert("Fault in ");
	switch (regs->int_parm_long & 3) {
	case 3:
		pr_cont("home space ");
		break;
	case 2:
		pr_cont("secondary space ");
		break;
	case 1:
		pr_cont("access register ");
		break;
	case 0:
		pr_cont("primary space ");
		break;
	}
	pr_cont("mode while using ");
	switch (get_fault_type(regs)) {
	case USER_FAULT:
		asce = S390_lowcore.user_asce;
		pr_cont("user ");
		break;
	case VDSO_FAULT:
		asce = S390_lowcore.vdso_asce;
		pr_cont("vdso ");
		break;
	case GMAP_FAULT:
		asce = ((struct gmap *) S390_lowcore.gmap)->asce;
		pr_cont("gmap ");
		break;
	case KERNEL_FAULT:
		asce = S390_lowcore.kernel_asce;
		pr_cont("kernel ");
		break;
	}
	pr_cont("ASCE.\n");
	dump_pagetable(asce, regs->int_parm_long & __FAIL_ADDR_MASK);
}

int show_unhandled_signals = 1;

void report_user_fault(struct pt_regs *regs, long signr, int is_mm_fault)
{
	if ((task_pid_nr(current) > 1) && !show_unhandled_signals)
		return;
	if (!unhandled_signal(current, signr))
		return;
	if (!printk_ratelimit())
		return;
	printk(KERN_ALERT "User process fault: interruption code %04x ilc:%d ",
	       regs->int_code & 0xffff, regs->int_code >> 17);
	print_vma_addr(KERN_CONT "in ", regs->psw.addr);
	printk(KERN_CONT "\n");
	if (is_mm_fault)
		dump_fault_info(regs);
	show_regs(regs);
}

/*
 * Send SIGSEGV to task.  This is an external routine
 * to keep the stack usage of do_page_fault small.
 */
static noinline void do_sigsegv(struct pt_regs *regs, int si_code)
{
	report_user_fault(regs, SIGSEGV, 1);
	force_sig_fault(SIGSEGV, si_code,
			(void __user *)(regs->int_parm_long & __FAIL_ADDR_MASK),
			current);
}

static noinline void do_no_context(struct pt_regs *regs)
{
	const struct exception_table_entry *fixup;

	/* Are we prepared to handle this kernel fault?  */
	fixup = search_exception_tables(regs->psw.addr);
	if (fixup) {
		regs->psw.addr = extable_fixup(fixup);
		return;
	}

	/*
	 * Oops. The kernel tried to access some bad page. We'll have to
	 * terminate things with extreme prejudice.
	 */
	if (get_fault_type(regs) == KERNEL_FAULT)
		printk(KERN_ALERT "Unable to handle kernel pointer dereference"
		       " in virtual kernel address space\n");
	else
		printk(KERN_ALERT "Unable to handle kernel paging request"
		       " in virtual user address space\n");
	dump_fault_info(regs);
	die(regs, "Oops");
	do_exit(SIGKILL);
}

static noinline void do_low_address(struct pt_regs *regs)
{
	/* Low-address protection hit in kernel mode means
	   NULL pointer write access in kernel mode.  */
	if (regs->psw.mask & PSW_MASK_PSTATE) {
		/* Low-address protection hit in user mode 'cannot happen'. */
		die (regs, "Low-address protection");
		do_exit(SIGKILL);
	}

	do_no_context(regs);
}

static noinline void do_sigbus(struct pt_regs *regs)
{
	/*
	 * Send a sigbus, regardless of whether we were in kernel
	 * or user mode.
	 */
	force_sig_fault(SIGBUS, BUS_ADRERR,
			(void __user *)(regs->int_parm_long & __FAIL_ADDR_MASK),
			current);
}

static noinline int signal_return(struct pt_regs *regs)
{
	u16 instruction;
	int rc;

	rc = __get_user(instruction, (u16 __user *) regs->psw.addr);
	if (rc)
		return rc;
	if (instruction == 0x0a77) {
		set_pt_regs_flag(regs, PIF_SYSCALL);
		regs->int_code = 0x00040077;
		return 0;
	} else if (instruction == 0x0aad) {
		set_pt_regs_flag(regs, PIF_SYSCALL);
		regs->int_code = 0x000400ad;
		return 0;
	}
	return -EACCES;
}

static noinline void do_fault_error(struct pt_regs *regs, int access,
					vm_fault_t fault)
{
	int si_code;

	switch (fault) {
	case VM_FAULT_BADACCESS:
		if (access == VM_EXEC && signal_return(regs) == 0)
			break;
	case VM_FAULT_BADMAP:
		/* Bad memory access. Check if it is kernel or user space. */
		if (user_mode(regs)) {
			/* User mode accesses just cause a SIGSEGV */
			si_code = (fault == VM_FAULT_BADMAP) ?
				SEGV_MAPERR : SEGV_ACCERR;
			do_sigsegv(regs, si_code);
			break;
		}
	case VM_FAULT_BADCONTEXT:
	case VM_FAULT_PFAULT:
		do_no_context(regs);
		break;
	case VM_FAULT_SIGNAL:
		if (!user_mode(regs))
			do_no_context(regs);
		break;
	default: /* fault & VM_FAULT_ERROR */
		if (fault & VM_FAULT_OOM) {
			if (!user_mode(regs))
				do_no_context(regs);
			else
				pagefault_out_of_memory();
		} else if (fault & VM_FAULT_SIGSEGV) {
			/* Kernel mode? Handle exceptions or die */
			if (!user_mode(regs))
				do_no_context(regs);
			else
				do_sigsegv(regs, SEGV_MAPERR);
		} else if (fault & VM_FAULT_SIGBUS) {
			/* Kernel mode? Handle exceptions or die */
			if (!user_mode(regs))
				do_no_context(regs);
			else
				do_sigbus(regs);
		} else
			BUG();
		break;
	}
}

/*
 * This routine handles page faults.  It determines the address,
 * and the problem, and then passes it off to one of the appropriate
 * routines.
 *
 * interruption code (int_code):
 *   04       Protection           ->  Write-Protection  (suprression)
 *   10       Segment translation  ->  Not present       (nullification)
 *   11       Page translation     ->  Not present       (nullification)
 *   3b       Region third trans.  ->  Not present       (nullification)
 */
static inline vm_fault_t do_exception(struct pt_regs *regs, int access)
{
	struct gmap *gmap;
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	enum fault_type type;
	unsigned long trans_exc_code;
	unsigned long address;
	unsigned int flags;
	vm_fault_t fault;

	tsk = current;
	/*
	 * The instruction that caused the program check has
	 * been nullified. Don't signal single step via SIGTRAP.
	 */
	clear_pt_regs_flag(regs, PIF_PER_TRAP);

	if (notify_page_fault(regs))
		return 0;

	mm = tsk->mm;
	trans_exc_code = regs->int_parm_long;

	/*
	 * Verify that the fault happened in user space, that
	 * we are not in an interrupt and that there is a 
	 * user context.
	 */
	fault = VM_FAULT_BADCONTEXT;
	type = get_fault_type(regs);
	switch (type) {
	case KERNEL_FAULT:
		goto out;
	case VDSO_FAULT:
		fault = VM_FAULT_BADMAP;
		goto out;
	case USER_FAULT:
	case GMAP_FAULT:
		if (faulthandler_disabled() || !mm)
			goto out;
		break;
	}

	address = trans_exc_code & __FAIL_ADDR_MASK;
	perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS, 1, regs, address);
	flags = FAULT_FLAG_ALLOW_RETRY | FAULT_FLAG_KILLABLE;
	if (user_mode(regs))
		flags |= FAULT_FLAG_USER;
	if ((trans_exc_code & store_indication) == 0x400)
		access = VM_WRITE;
	if (access == VM_WRITE)
		flags |= FAULT_FLAG_WRITE;
	down_read(&mm->mmap_sem);

	gmap = NULL;
	if (IS_ENABLED(CONFIG_PGSTE) && type == GMAP_FAULT) {
		gmap = (struct gmap *) S390_lowcore.gmap;
		current->thread.gmap_addr = address;
		current->thread.gmap_write_flag = !!(flags & FAULT_FLAG_WRITE);
		current->thread.gmap_int_code = regs->int_code & 0xffff;
		address = __gmap_translate(gmap, address);
		if (address == -EFAULT) {
			fault = VM_FAULT_BADMAP;
			goto out_up;
		}
		if (gmap->pfault_enabled)
			flags |= FAULT_FLAG_RETRY_NOWAIT;
	}

retry:
	fault = VM_FAULT_BADMAP;
	vma = find_vma(mm, address);
	if (!vma)
		goto out_up;

	if (unlikely(vma->vm_start > address)) {
		if (!(vma->vm_flags & VM_GROWSDOWN))
			goto out_up;
		if (expand_stack(vma, address))
			goto out_up;
	}

	/*
	 * Ok, we have a good vm_area for this memory access, so
	 * we can handle it..
	 */
	fault = VM_FAULT_BADACCESS;
	if (unlikely(!(vma->vm_flags & access)))
		goto out_up;

	if (is_vm_hugetlb_page(vma))
		address &= HPAGE_MASK;
	/*
	 * If for any reason at all we couldn't handle the fault,
	 * make sure we exit gracefully rather than endlessly redo
	 * the fault.
	 */
	fault = handle_mm_fault(vma, address, flags);
	if (fault_signal_pending(fault, regs)) {
		fault = VM_FAULT_SIGNAL;
		if (flags & FAULT_FLAG_RETRY_NOWAIT)
			goto out_up;
		goto out;
	}
	if (unlikely(fault & VM_FAULT_ERROR))
		goto out_up;

	/*
	 * Major/minor page fault accounting is only done on the
	 * initial attempt. If we go through a retry, it is extremely
	 * likely that the page will be found in page cache at that point.
	 */
	if (flags & FAULT_FLAG_ALLOW_RETRY) {
		if (fault & VM_FAULT_MAJOR) {
			tsk->maj_flt++;
			perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS_MAJ, 1,
				      regs, address);
		} else {
			tsk->min_flt++;
			perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS_MIN, 1,
				      regs, address);
		}
		if (fault & VM_FAULT_RETRY) {
			if (IS_ENABLED(CONFIG_PGSTE) && gmap &&
			    (flags & FAULT_FLAG_RETRY_NOWAIT)) {
				/* FAULT_FLAG_RETRY_NOWAIT has been set,
				 * mmap_sem has not been released */
				current->thread.gmap_pfault = 1;
				fault = VM_FAULT_PFAULT;
				goto out_up;
			}
			/* Clear FAULT_FLAG_ALLOW_RETRY to avoid any risk
			 * of starvation. */
			flags &= ~(FAULT_FLAG_ALLOW_RETRY |
				   FAULT_FLAG_RETRY_NOWAIT);
			flags |= FAULT_FLAG_TRIED;
			down_read(&mm->mmap_sem);
			goto retry;
		}
	}
	if (IS_ENABLED(CONFIG_PGSTE) && gmap) {
		address =  __gmap_link(gmap, current->thread.gmap_addr,
				       address);
		if (address == -EFAULT) {
			fault = VM_FAULT_BADMAP;
			goto out_up;
		}
		if (address == -ENOMEM) {
			fault = VM_FAULT_OOM;
			goto out_up;
		}
	}
	fault = 0;
out_up:
	up_read(&mm->mmap_sem);
out:
	return fault;
}

void do_protection_exception(struct pt_regs *regs)
{
	unsigned long trans_exc_code;
	int access;
	vm_fault_t fault;

	trans_exc_code = regs->int_parm_long;
	/*
	 * Protection exceptions are suppressing, decrement psw address.
	 * The exception to this rule are aborted transactions, for these
	 * the PSW already points to the correct location.
	 */
	if (!(regs->int_code & 0x200))
		regs->psw.addr = __rewind_psw(regs->psw, regs->int_code >> 16);
	/*
	 * Check for low-address protection.  This needs to be treated
	 * as a special case because the translation exception code
	 * field is not guaranteed to contain valid data in this case.
	 */
	if (unlikely(!(trans_exc_code & 4))) {
		do_low_address(regs);
		return;
	}
	if (unlikely(MACHINE_HAS_NX && (trans_exc_code & 0x80))) {
		regs->int_parm_long = (trans_exc_code & ~PAGE_MASK) |
					(regs->psw.addr & PAGE_MASK);
		access = VM_EXEC;
		fault = VM_FAULT_BADACCESS;
	} else {
		access = VM_WRITE;
		fault = do_exception(regs, access);
	}
	if (unlikely(fault))
		do_fault_error(regs, access, fault);
}
NOKPROBE_SYMBOL(do_protection_exception);

void do_dat_exception(struct pt_regs *regs)
{
	int access;
	vm_fault_t fault;

	access = VM_READ | VM_EXEC | VM_WRITE;
	fault = do_exception(regs, access);
	if (unlikely(fault))
		do_fault_error(regs, access, fault);
}
NOKPROBE_SYMBOL(do_dat_exception);

#ifdef CONFIG_PFAULT 
/*
 * 'pfault' pseudo page faults routines.
 */
static int pfault_disable;

static int __init nopfault(char *str)
{
	pfault_disable = 1;
	return 1;
}

__setup("nopfault", nopfault);

struct pfault_refbk {
	u16 refdiagc;
	u16 reffcode;
	u16 refdwlen;
	u16 refversn;
	u64 refgaddr;
	u64 refselmk;
	u64 refcmpmk;
	u64 reserved;
} __attribute__ ((packed, aligned(8)));

int pfault_init(void)
{
	struct pfault_refbk refbk = {
		.refdiagc = 0x258,
		.reffcode = 0,
		.refdwlen = 5,
		.refversn = 2,
		.refgaddr = __LC_LPP,
		.refselmk = 1ULL << 48,
		.refcmpmk = 1ULL << 48,
		.reserved = __PF_RES_FIELD };
        int rc;

	if (pfault_disable)
		return -1;
	diag_stat_inc(DIAG_STAT_X258);
	asm volatile(
		"	diag	%1,%0,0x258\n"
		"0:	j	2f\n"
		"1:	la	%0,8\n"
		"2:\n"
		EX_TABLE(0b,1b)
		: "=d" (rc) : "a" (&refbk), "m" (refbk) : "cc");
        return rc;
}

void pfault_fini(void)
{
	struct pfault_refbk refbk = {
		.refdiagc = 0x258,
		.reffcode = 1,
		.refdwlen = 5,
		.refversn = 2,
	};

	if (pfault_disable)
		return;
	diag_stat_inc(DIAG_STAT_X258);
	asm volatile(
		"	diag	%0,0,0x258\n"
		"0:	nopr	%%r7\n"
		EX_TABLE(0b,0b)
		: : "a" (&refbk), "m" (refbk) : "cc");
}

static DEFINE_SPINLOCK(pfault_lock);
static LIST_HEAD(pfault_list);

#define PF_COMPLETE	0x0080

/*
 * The mechanism of our pfault code: if Linux is running as guest, runs a user
 * space process and the user space process accesses a page that the host has
 * paged out we get a pfault interrupt.
 *
 * This allows us, within the guest, to schedule a different process. Without
 * this mechanism the host would have to suspend the whole virtual cpu until
 * the page has been paged in.
 *
 * So when we get such an interrupt then we set the state of the current task
 * to uninterruptible and also set the need_resched flag. Both happens within
 * interrupt context(!). If we later on want to return to user space we
 * recognize the need_resched flag and then call schedule().  It's not very
 * obvious how this works...
 *
 * Of course we have a lot of additional fun with the completion interrupt (->
 * host signals that a page of a process has been paged in and the process can
 * continue to run). This interrupt can arrive on any cpu and, since we have
 * virtual cpus, actually appear before the interrupt that signals that a page
 * is missing.
 */
static void pfault_interrupt(struct ext_code ext_code,
			     unsigned int param32, unsigned long param64)
{
	struct task_struct *tsk;
	__u16 subcode;
	pid_t pid;

	/*
	 * Get the external interruption subcode & pfault initial/completion
	 * signal bit. VM stores this in the 'cpu address' field associated
	 * with the external interrupt.
	 */
	subcode = ext_code.subcode;
	if ((subcode & 0xff00) != __SUBCODE_MASK)
		return;
	inc_irq_stat(IRQEXT_PFL);
	/* Get the token (= pid of the affected task). */
	pid = param64 & LPP_PID_MASK;
	rcu_read_lock();
	tsk = find_task_by_pid_ns(pid, &init_pid_ns);
	if (tsk)
		get_task_struct(tsk);
	rcu_read_unlock();
	if (!tsk)
		return;
	spin_lock(&pfault_lock);
	if (subcode & PF_COMPLETE) {
		/* signal bit is set -> a page has been swapped in by VM */
		if (tsk->thread.pfault_wait == 1) {
			/* Initial interrupt was faster than the completion
			 * interrupt. pfault_wait is valid. Set pfault_wait
			 * back to zero and wake up the process. This can
			 * safely be done because the task is still sleeping
			 * and can't produce new pfaults. */
			tsk->thread.pfault_wait = 0;
			list_del(&tsk->thread.list);
			wake_up_process(tsk);
			put_task_struct(tsk);
		} else {
			/* Completion interrupt was faster than initial
			 * interrupt. Set pfault_wait to -1 so the initial
			 * interrupt doesn't put the task to sleep.
			 * If the task is not running, ignore the completion
			 * interrupt since it must be a leftover of a PFAULT
			 * CANCEL operation which didn't remove all pending
			 * completion interrupts. */
			if (task_is_running(tsk))
				tsk->thread.pfault_wait = -1;
		}
	} else {
		/* signal bit not set -> a real page is missing. */
		if (WARN_ON_ONCE(tsk != current))
			goto out;
		if (tsk->thread.pfault_wait == 1) {
			/* Already on the list with a reference: put to sleep */
			goto block;
		} else if (tsk->thread.pfault_wait == -1) {
			/* Completion interrupt was faster than the initial
			 * interrupt (pfault_wait == -1). Set pfault_wait
			 * back to zero and exit. */
			tsk->thread.pfault_wait = 0;
		} else {
			/* Initial interrupt arrived before completion
			 * interrupt. Let the task sleep.
			 * An extra task reference is needed since a different
			 * cpu may set the task state to TASK_RUNNING again
			 * before the scheduler is reached. */
			get_task_struct(tsk);
			tsk->thread.pfault_wait = 1;
			list_add(&tsk->thread.list, &pfault_list);
block:
			/* Since this must be a userspace fault, there
			 * is no kernel task state to trample. Rely on the
			 * return to userspace schedule() to block. */
			__set_current_state(TASK_UNINTERRUPTIBLE);
			set_tsk_need_resched(tsk);
			set_preempt_need_resched();
		}
	}
out:
	spin_unlock(&pfault_lock);
	put_task_struct(tsk);
}

static int pfault_cpu_dead(unsigned int cpu)
{
	struct thread_struct *thread, *next;
	struct task_struct *tsk;

	spin_lock_irq(&pfault_lock);
	list_for_each_entry_safe(thread, next, &pfault_list, list) {
		thread->pfault_wait = 0;
		list_del(&thread->list);
		tsk = container_of(thread, struct task_struct, thread);
		wake_up_process(tsk);
		put_task_struct(tsk);
	}
	spin_unlock_irq(&pfault_lock);
	return 0;
}

static int __init pfault_irq_init(void)
{
	int rc;

	rc = register_external_irq(EXT_IRQ_CP_SERVICE, pfault_interrupt);
	if (rc)
		goto out_extint;
	rc = pfault_init() == 0 ? 0 : -EOPNOTSUPP;
	if (rc)
		goto out_pfault;
	irq_subclass_register(IRQ_SUBCLASS_SERVICE_SIGNAL);
	cpuhp_setup_state_nocalls(CPUHP_S390_PFAULT_DEAD, "s390/pfault:dead",
				  NULL, pfault_cpu_dead);
	return 0;

out_pfault:
	unregister_external_irq(EXT_IRQ_CP_SERVICE, pfault_interrupt);
out_extint:
	pfault_disable = 1;
	return rc;
}
early_initcall(pfault_irq_init);

#endif /* CONFIG_PFAULT */
