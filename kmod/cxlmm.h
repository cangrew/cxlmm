/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cxlmm.h : kernel-internal data structures for the CXL-aware memory manager
 *
 * All mutable shared state goes through explicit locking (spinlock or rwlock).
 * Structures are replaced atomically rather than mutated in-place where possible.
 */
#ifndef _CXLMM_H
#define _CXLMM_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/rwlock.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/perf_event.h>
#include <linux/sched.h>
#include <linux/mm.h>

#include "../include/cxlmm_uapi.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define CXLMM_MODULE_NAME       "cxlmm"
#define CXLMM_DEVICE_NAME       "cxlmm"
#define CXLMM_PROC_DIR          "cxlmm"

#define CXLMM_DEFAULT_SCAN_MS          1000
#define CXLMM_DEFAULT_WRITE_THRESH_PCT   60
#define CXLMM_DEFAULT_MIN_SCANS           3
#define CXLMM_DEFAULT_BATCH_SIZE         64
#define CXLMM_DEFAULT_MAX_PIDS           64
#define CXLMM_SCORE_RING_SIZE          (128 * 1024)

/* GUP batch size : must be a power of 2, <= 512 */
#define CXLMM_GUP_BATCH  512

/*
 * SPR IMC uncore perf event encoding for CAS counts.
 * Source: arch/x86/events/intel/uncore_snbep.c:6053
 *   event=0x05, umask=0xcf → cas_count_read
 *   event=0x05, umask=0xf0 → cas_count_write
 * One CAS = one 64-byte cache line = 64 bytes.
 * Scale to MiB: count * 64 / (1024*1024) = count * 6.103515625e-5
 */
#define CXLMM_IMC_EVENT_CODE   0x05
#define CXLMM_IMC_UMASK_READ   0xcf
#define CXLMM_IMC_UMASK_WRITE  0xf0
#define CXLMM_MAX_IMC_BOXES    16   /* Sapphire Rapids has up to 12 IMCs */

/* CXL uncore types (same source file, line ~6169) */
#define CXLMM_CXLCM_EVENT_CODE  0x05
#define CXLMM_CXLDP_EVENT_CODE  0x05

/* --------------------------------------------------------------------------
 * Tracked PID entry
 * -------------------------------------------------------------------------- */

struct cxlmm_tracked_pid {
	struct list_head  list;
	pid_t             pid;
	__u32             flags;        /* reserved */
	unsigned long     last_scan_jiffies;
};

/* --------------------------------------------------------------------------
 * Score ring buffer
 *
 * The scanner pushes entries; the ioctl FETCH_SCORES drains them.
 * Protected by a single spinlock. Ring is sized CXLMM_SCORE_RING_SIZE.
 * -------------------------------------------------------------------------- */

struct cxlmm_score_ring {
	spinlock_t              lock;
	struct cxlmm_page_score *buf;      /* kmalloc'd array            */
	unsigned int             capacity;  /* total slots                */
	unsigned int             head;      /* next read position         */
	unsigned int             tail;      /* next write position        */
	unsigned long            dropped;   /* overflow counter           */
};

/* --------------------------------------------------------------------------
 * IMC perf event box
 *
 * One IMC channel = one read counter + one write counter.
 * -------------------------------------------------------------------------- */

struct cxlmm_imc_box {
	struct perf_event  *read_event;   /* CAS read  counter  */
	struct perf_event  *write_event;  /* CAS write counter  */
	int                 cpu;          /* CPU affinity for this box */
};

/* --------------------------------------------------------------------------
 * Global module state : single instance
 *
 * Immutable fields (set at init, never changed): config_lock, misc_dev, etc.
 * Mutable fields use explicit locking as noted.
 * -------------------------------------------------------------------------- */

struct cxlmm_state {
	/* ----- configuration (atomic replacement) ----- */
	spinlock_t           config_lock;
	struct cxlmm_config  config;          /* protected by config_lock */

	/* ----- scanner kthread ----- */
	struct task_struct  *scanner_thread;
	atomic_t             scan_generation; /* bumped each full sweep   */

	/* ----- tracked PID list ----- */
	rwlock_t             pid_lock;
	struct list_head     tracked_pid_list;
	unsigned int         tracked_pid_count; /* protected by pid_lock  */

	/* ----- score ring buffer ----- */
	struct cxlmm_score_ring score_ring;

	/* ----- IMC bandwidth monitoring ----- */
	struct cxlmm_imc_box  imc_boxes[CXLMM_MAX_IMC_BOXES];
	unsigned int           imc_box_count;
	spinlock_t             bw_lock;
	struct cxlmm_bw_stats  bw_snapshot;    /* protected by bw_lock   */
	struct cxlmm_bw_stats  bw_prev;        /* for delta calculation   */

	/* ----- kernel infrastructure ----- */
	struct miscdevice    misc_dev;
	struct proc_dir_entry *proc_dir;

	/* ----- module params (read-only after init) ----- */
	int                  cxl_node;
	int                  ddr_node;
};

/* Single global state pointer : set in module_init, cleared in module_exit */
extern struct cxlmm_state *g_cxlmm;

/* --------------------------------------------------------------------------
 * Subsystem init/exit prototypes
 * -------------------------------------------------------------------------- */

/* cxlmm_scanner.c */
int  cxlmm_scanner_init(struct cxlmm_state *st);
void cxlmm_scanner_exit(struct cxlmm_state *st);

/* cxlmm_bw_monitor.c */
int  cxlmm_bw_monitor_init(struct cxlmm_state *st);
void cxlmm_bw_monitor_exit(struct cxlmm_state *st);
void cxlmm_bw_monitor_sample(struct cxlmm_state *st);

/* cxlmm_procfs.c */
int  cxlmm_procfs_init(struct cxlmm_state *st);
void cxlmm_procfs_exit(struct cxlmm_state *st);

/* cxlmm_ioctl.c */
long cxlmm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

/* --------------------------------------------------------------------------
 * Score ring helpers : defined in cxlmm_main.c
 * -------------------------------------------------------------------------- */

/**
 * cxlmm_ring_push - add a score record to the ring (scanner context)
 * Returns 0 on success, -ENOSPC if full (entry dropped, counter bumped).
 */
int cxlmm_ring_push(struct cxlmm_score_ring *ring,
		    const struct cxlmm_page_score *score);

/**
 * cxlmm_ring_drain - copy up to @max entries into @out[], return count copied.
 * Called from ioctl FETCH_SCORES context.
 */
unsigned int cxlmm_ring_drain(struct cxlmm_score_ring *ring,
			      struct cxlmm_page_score *out,
			      unsigned int max);

#endif /* _CXLMM_H */
