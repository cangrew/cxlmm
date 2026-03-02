// SPDX-License-Identifier: GPL-2.0
/*
 * cxlmm_scanner.c — kthread that detects write-heavy pages
 *
 * Algorithm (mirrors mm/damon/paddr.c pattern):
 *
 *   For each tracked PID, every SCAN_INTERVAL_MS:
 *     1. get_task_mm()            — grab mm_struct (pins the mm)
 *     2. mmap_read_lock(mm)
 *     3. Iterate VMAs; for each, GUP 512 pages at a time:
 *          get_user_pages_remote() with FOLL_GET|FOLL_DUMP
 *          for each page:
 *            folio = page_folio(page)
 *            if folio_trylock() succeeds:
 *              cleaned = folio_mkclean(folio)   ← write detection
 *              folio_unlock()
 *              emit cxlmm_page_score to ring
 *            put_page()
 *     4. mmap_read_unlock(mm); mmput(mm)
 *
 * folio_mkclean() removes all write-protect overrides and returns the
 * number of PTEs it cleaned.  If cleaned > 0, the folio was dirty/writable
 * since the last scan → write_score++.
 *
 * IMPORTANT: folio_mkclean requires the folio to be locked (folio_trylock).
 * We use trylock to avoid deadlocks with page reclaim paths.
 */

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/numa.h>

#include "cxlmm.h"

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

static int cxlmm_scanner_thread(void *data);
static void scan_one_pid(struct cxlmm_state *st, struct cxlmm_tracked_pid *tp);
static void scan_vma(struct cxlmm_state *st, struct mm_struct *mm,
		     struct vm_area_struct *vma, pid_t pid);

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

	VMA_ITERATOR(vmi, NULL, 0);

	/* Find the task */
	rcu_read_lock();
	task = find_task_by_vpid(tp->pid);
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

	/* Initialize VMA iterator */
	vma_iter_init(&vmi, mm, 0);

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
	struct page *pages[CXLMM_GUP_BATCH];
	int nr_pages, i;

	while (addr < vma->vm_end) {
		unsigned long len = min_t(unsigned long,
					  (vma->vm_end - addr) >> PAGE_SHIFT,
					  (unsigned long)CXLMM_GUP_BATCH);

		if (len == 0)
			break;

		/*
		 * get_user_pages_remote:
		 *   FOLL_GET  — increment page refcount
		 *   FOLL_DUMP — skip special pages (avoids IO pages etc.)
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
			int cleaned = 0;

			folio = page_folio(pages[i]);

			/*
			 * folio_trylock: non-blocking; skip if page is
			 * locked by reclaim/writeback to avoid deadlock.
			 */
			if (folio_trylock(folio)) {
				/*
				 * folio_mkclean: removes all writable PTEs
				 * mapping this folio and clears the dirty bit.
				 * Returns the number of PTEs cleaned (> 0 means
				 * the folio was writable/dirty since last scan).
				 *
				 * Requires folio to be locked. Defined in
				 * mm/rmap.c:1095, EXPORT_SYMBOL_GPL.
				 */
				cleaned = folio_mkclean(folio);
				folio_unlock(folio);
			}

			score.vaddr       = addr + ((unsigned long)i << PAGE_SHIFT);
			score.pid         = pid;
			score.write_score = (cleaned > 0) ? 1 : 0;
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
}
