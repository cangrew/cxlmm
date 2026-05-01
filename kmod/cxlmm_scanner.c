// SPDX-License-Identifier: GPL-2.0
/*
 * cxlmm_scanner.c : kthread that detects write-heavy pages
 *
 * Algorithm:
 *
 *   For each tracked PID, every SCAN_INTERVAL_MS:
 *     1. get_task_mm()            : grab mm_struct (pins the mm)
 *     2. mmap_read_lock(mm)
 *     3. Iterate VMAs; for each, GUP 512 pages at a time:
 *          get_user_pages_remote() with FOLL_GET|FOLL_DUMP
 *          for each page:
 *            walk page table → check pte_dirty()
 *            clear dirty + write-protect PTE → future writes fault
 *            emit cxlmm_page_score to ring
 *            put_page()
 *     4. mmap_read_unlock(mm); mmput(mm)
 *
 * Write detection uses direct PTE dirty-bit inspection rather than
 * folio_mkclean(), because folio_mkclean() returns 0 for anonymous
 * pages (folio_mapping() is NULL for anon memory, causing early return).
 *
 * The scanner write-protects each PTE after reading its dirty bit.
 * When the application next writes to that page, the CPU triggers a
 * write-protect fault; the kernel's do_wp_page() path sees it is a
 * private anonymous page and transparently re-enables the writable PTE.
 * On the next scan cycle the dirty bit is set again → write detected.
 */

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/numa.h>
#include <linux/pgtable.h>
#include <linux/smp.h>

#include "cxlmm.h"

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

static int cxlmm_scanner_thread(void *data);
static void scan_one_pid(struct cxlmm_state *st, struct cxlmm_tracked_pid *tp);
static void scan_vma(struct cxlmm_state *st, struct mm_struct *mm,
		     struct vm_area_struct *vma, pid_t pid);

/*
 * TLB flush callback for on_each_cpu().  flush_tlb_mm_range() is not
 * exported for modules, so we use the exported __flush_tlb_all() on
 * every CPU once per PID scan to ensure write-protected PTEs take
 * effect immediately.
 */
static void flush_tlb_all_cpus(void *info)
{
	__flush_tlb_all();
}

/* --------------------------------------------------------------------------
 * PTE dirty-bit write detection for anonymous pages
 *
 * folio_mkclean() returns 0 for anonymous pages because folio_mapping()
 * is NULL (no address_space for anon memory).  Instead, we walk the page
 * table directly, check the hardware dirty bit, and write-protect the PTE
 * so future writes trigger a fault that transparently re-enables it.
 * -------------------------------------------------------------------------- */

/*
 * Handle transparent huge page (THP) PMD entries.
 * Check the PMD dirty bit and write-protect the entire 2MB page.
 * Returns 1 if the huge page was dirty, 0 otherwise.
 */
static int check_clear_pmd_dirty(struct mm_struct *mm,
				 struct vm_area_struct *vma,
				 unsigned long addr, pmd_t *pmdp)
{
	pmd_t pmd_val;
	spinlock_t *ptl;
	int was_dirty = 0;

	ptl = pmd_lock(mm, pmdp);
	pmd_val = *pmdp;

	/* Recheck under lock : PMD may have been split */
	if (!pmd_trans_huge(pmd_val)) {
		spin_unlock(ptl);
		return -1;  /* PMD was split, caller should retry as PTE */
	}

	if (pmd_dirty(pmd_val))
		was_dirty = 1;

	if (pmd_dirty(pmd_val) || pmd_write(pmd_val)) {
		pmd_t new_pmd;

		new_pmd = pmd_mkclean(pmd_val);
		new_pmd = pmd_wrprotect(new_pmd);
		set_pmd_at(mm, addr & PMD_MASK, pmdp, new_pmd);
	}

	spin_unlock(ptl);
	return was_dirty;
}

static int check_clear_pte_dirty(struct mm_struct *mm,
				 struct vm_area_struct *vma,
				 unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	int was_dirty = 0;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return 0;

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return 0;

	/* Handle transparent huge pages at the PMD level */
	if (pmd_trans_huge(*pmd)) {
		int rc = check_clear_pmd_dirty(mm, vma, addr, pmd);
		if (rc >= 0)
			return rc;
		/* rc == -1: PMD was split under us, fall through to PTE path */
	}

	if (pmd_bad(*pmd))
		return 0;

	/*
	 * Access PTE directly: pte_offset_map_lock is not exported for
	 * modules on this kernel.  We hold mmap_read_lock and the PMD
	 * is verified present and non-THP, so the PTE page is stable.
	 * Use pmd_page + pte_index for the pointer and pte_lockptr for
	 * the per-PTE-page spinlock.
	 */
	ptep = (pte_t *)page_address(pmd_page(*pmd)) + pte_index(addr);
	ptl = pte_lockptr(mm, pmd);

	spin_lock(ptl);
	pte = ptep_get(ptep);

	if (!pte_present(pte)) {
		spin_unlock(ptl);
		return 0;
	}

	if (pte_dirty(pte))
		was_dirty = 1;

	/*
	 * Clear dirty bit and remove write permission.  The next write
	 * triggers do_wp_page() which re-enables the writable PTE for
	 * private anonymous pages transparently.
	 *
	 * ptep_modify_prot_start/commit handle TLB invalidation
	 * atomically on x86 : no separate flush_tlb_* call needed.
	 */
	if (pte_dirty(pte) || pte_write(pte)) {
		pte_t new_pte;

		pte = ptep_modify_prot_start(vma, addr, ptep);
		new_pte = pte_mkclean(pte);
		new_pte = pte_wrprotect(new_pte);
		ptep_modify_prot_commit(vma, addr, ptep, pte, new_pte);
	}

	spin_unlock(ptl);
	return was_dirty;
}

/* --------------------------------------------------------------------------
 * Public: init / exit
 * -------------------------------------------------------------------------- */

int cxlmm_scanner_init(struct cxlmm_state *st)
{
	struct task_struct *tsk;

	tsk = kthread_run(cxlmm_scanner_thread, st, "cxlmm_scan");
	if (IS_ERR(tsk)) {
		pr_err("cxlmm: failed to create scanner kthread: %ld\n",
		       PTR_ERR(tsk));
		return PTR_ERR(tsk);
	}

	st->scanner_thread = tsk;
	pr_info("cxlmm: scanner kthread started (pid %d)\n", tsk->pid);
	return 0;
}

void cxlmm_scanner_exit(struct cxlmm_state *st)
{
	if (st->scanner_thread) {
		kthread_stop(st->scanner_thread);
		st->scanner_thread = NULL;
		pr_info("cxlmm: scanner kthread stopped\n");
	}
}

/* --------------------------------------------------------------------------
 * Main scanner loop
 * -------------------------------------------------------------------------- */

static int cxlmm_scanner_thread(void *data)
{
	struct cxlmm_state *st = data;

	pr_info("cxlmm: scanner thread running\n");

	while (!kthread_should_stop()) {
		struct cxlmm_tracked_pid *tp;
		unsigned int interval_ms;

		/* Sample BW counters each wakeup */
		cxlmm_bw_monitor_sample(st);

		/* Read current scan interval under lock */
		spin_lock(&st->config_lock);
		interval_ms = st->config.scan_interval_ms;
		spin_unlock(&st->config_lock);

		/* Scan all tracked PIDs */
		read_lock(&st->pid_lock);
		list_for_each_entry(tp, &st->tracked_pid_list, list) {
			if (kthread_should_stop())
				break;
			scan_one_pid(st, tp);
		}
		read_unlock(&st->pid_lock);

		atomic_inc(&st->scan_generation);

		/* Sleep for scan_interval_ms, waking early if stopped */
		msleep_interruptible(interval_ms);
	}

	pr_info("cxlmm: scanner thread exiting\n");
	return 0;
}

/* --------------------------------------------------------------------------
 * Scan one tracked PID
 * -------------------------------------------------------------------------- */

static void scan_one_pid(struct cxlmm_state *st, struct cxlmm_tracked_pid *tp)
{
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;

	/* Find the task */
	rcu_read_lock();
	task = pid_task(find_vpid(tp->pid), PIDTYPE_PID);
	if (!task) {
		rcu_read_unlock();
		return;
	}
	get_task_struct(task);
	rcu_read_unlock();

	/* Pin the mm */
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return;

	mmap_read_lock(mm);

	/* Iterate VMAs in this mm from address 0 */
	VMA_ITERATOR(vmi, mm, 0);

	for_each_vma(vmi, vma) {
		if (kthread_should_stop())
			break;

		/* Skip special VMAs that shouldn't be touched */
		if (vma->vm_flags & (VM_IO | VM_PFNMAP | VM_HUGETLB))
			continue;
		/* Only scan writable user mappings for write detection */
		if (!(vma->vm_flags & VM_WRITE))
			continue;

		scan_vma(st, mm, vma, tp->pid);
	}

	mmap_read_unlock(mm);

	/*
	 * Flush TLB on all CPUs so that the write-protected PTEs take
	 * effect immediately.  Without this, stale TLB entries absorb
	 * writes without updating the dirty bit in the page table,
	 * causing the next scan to miss real writes.
	 *
	 * This is done once per PID per scan cycle (every ~1 s), so the
	 * cross-CPU IPI cost is acceptable.
	 */
	on_each_cpu(flush_tlb_all_cpus, NULL, 1);

	mmput(mm);

	tp->last_scan_jiffies = jiffies;
}

/* --------------------------------------------------------------------------
 * Scan one VMA in GUP batches
 * -------------------------------------------------------------------------- */

static void scan_vma(struct cxlmm_state *st, struct mm_struct *mm,
		     struct vm_area_struct *vma, pid_t pid)
{
	unsigned long addr = vma->vm_start;
	struct page **pages;
	int nr_pages, i;
	/*
	 * THP tracking: a single PMD covers 2MB (512 base pages).
	 * check_clear_pte_dirty clears the dirty bit for the entire
	 * huge page on the first call.  Cache the result so subsequent
	 * base pages in the same huge page reuse it.
	 */
	unsigned long last_thp_pmd_addr = ~0UL;
	int           last_thp_dirty    = 0;

	pages = kcalloc(CXLMM_GUP_BATCH, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return;

	while (addr < vma->vm_end) {
		unsigned long len = min_t(unsigned long,
					  (vma->vm_end - addr) >> PAGE_SHIFT,
					  (unsigned long)CXLMM_GUP_BATCH);

		if (len == 0)
			break;

		/*
		 * get_user_pages_remote:
		 *   FOLL_GET  : increment page refcount
		 *   FOLL_DUMP : skip special pages (avoids IO pages etc.)
		 *
		 * Returns the number of pages pinned, or negative error.
		 * We drop mmap_read_lock implicitly via the _remote variant
		 * which handles the lock internally on some paths; here we
		 * hold it ourselves.
		 */
		nr_pages = get_user_pages_remote(mm, addr, len,
						 FOLL_GET | FOLL_DUMP,
						 pages, NULL);
		if (nr_pages <= 0) {
			addr += len << PAGE_SHIFT;
			continue;
		}

		for (i = 0; i < nr_pages; i++) {
			struct folio *folio;
			struct cxlmm_page_score score = {};
			unsigned long page_addr;
			int was_dirty;

			folio = page_folio(pages[i]);
			page_addr = addr + ((unsigned long)i << PAGE_SHIFT);

			/*
			 * For THP: all base pages in the same 2MB huge page
			 * share one PMD dirty bit.  Check once, reuse for all
			 * pages in the same PMD range.
			 */
			if ((page_addr & PMD_MASK) == last_thp_pmd_addr) {
				was_dirty = last_thp_dirty;
			} else {
				was_dirty = check_clear_pte_dirty(mm, vma,
								  page_addr);
				/*
				 * If this was a THP hit (the function checked
				 * the PMD), cache the result.  We detect this
				 * by checking if the address is in a huge page
				 * folio (compound page with order >= 9).
				 */
				if (folio_test_large(folio) &&
				    folio_order(folio) >= 9) {
					last_thp_pmd_addr = page_addr & PMD_MASK;
					last_thp_dirty = was_dirty;
				}
			}

			score.vaddr       = page_addr;
			score.pid         = pid;
			score.write_score = was_dirty ? 1 : 0;
			score.read_score  = 0;  /* filled by userspace */
			score.scan_count  = 1;
			score.current_node = folio_nid(folio);
			score.flags       = (folio_nid(folio) == st->cxl_node)
					     ? CXLMM_PFLAG_ON_CXL
					     : CXLMM_PFLAG_ON_DDR;
			score._pad        = 0;

			cxlmm_ring_push(&st->score_ring, &score);

			put_page(pages[i]);
		}

		addr += (unsigned long)nr_pages << PAGE_SHIFT;
	}

	kfree(pages);
}
