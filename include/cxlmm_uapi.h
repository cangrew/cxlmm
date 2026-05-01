/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * cxlmm_uapi.h : shared kernel/userspace ABI for the CXL-aware memory manager
 *
 * All structs use fixed-width types and explicit padding so they are
 * stable across 32/64-bit userspace. The ioctl command numbers live here
 * so userspace does not need to include kernel headers.
 */
#ifndef _CXLMM_UAPI_H
#define _CXLMM_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <linux/types.h>
#include <sys/ioctl.h>
#endif

/* --------------------------------------------------------------------------
 * Per-page observation record
 *
 * Written by the kernel scanner (folio_mkclean path), read_score is filled
 * in by userspace via /proc/<pid>/pagemap, then the record is used for
 * classification in the daemon.
 * -------------------------------------------------------------------------- */

/* flags for cxlmm_page_score.flags */
#define CXLMM_PFLAG_WRITE_HEAVY   (1 << 0)  /* classified write-heavy */
#define CXLMM_PFLAG_READ_HEAVY    (1 << 1)  /* classified read-heavy   */
#define CXLMM_PFLAG_PENDING_MIG   (1 << 2)  /* migration queued        */
#define CXLMM_PFLAG_ON_CXL        (1 << 3)  /* currently on CXL node   */
#define CXLMM_PFLAG_ON_DDR        (1 << 4)  /* currently on DDR node   */

struct cxlmm_page_score {
	__u64  vaddr;        /* page-aligned virtual address              */
	__u32  pid;          /* owning process                            */
	__u16  write_score;  /* incremented when folio_mkclean() > 0     */
	__u16  read_score;   /* incremented by userspace pagemap scanner  */
	__u8   scan_count;   /* number of complete scan cycles observed   */
	__u8   current_node; /* NUMA node from folio_nid() at last scan   */
	__u8   flags;        /* CXLMM_PFLAG_* bitmask                    */
	__u8   _pad;
} __attribute__((packed));

/* --------------------------------------------------------------------------
 * Bandwidth snapshot from IMC perf counters
 * -------------------------------------------------------------------------- */

struct cxlmm_bw_stats {
	__u64  ddr_read_mb;   /* DDR read  bandwidth since module load (MiB) */
	__u64  ddr_write_mb;  /* DDR write bandwidth since module load (MiB) */
	__u64  cxl_read_mb;   /* CXL read  bandwidth since module load (MiB) */
	__u64  cxl_write_mb;  /* CXL write bandwidth since module load (MiB) */
	__u64  timestamp_ns;  /* ktime_get_ns() at snapshot time             */
	/* interval deltas (filled by CXLMM_IOC_GET_BW) */
	__u64  ddr_read_mb_s;  /* MiB/s read  DDR  over last interval */
	__u64  ddr_write_mb_s; /* MiB/s write DDR  over last interval */
	__u64  cxl_read_mb_s;  /* MiB/s read  CXL  over last interval */
	__u64  cxl_write_mb_s; /* MiB/s write CXL  over last interval */
};

/* --------------------------------------------------------------------------
 * Module configuration
 *
 * Immutable snapshot in the kernel; replaced atomically on SET_CFG.
 * -------------------------------------------------------------------------- */

/* flags for cxlmm_config.flags */
#define CXLMM_CFG_FLAG_VERBOSE     (1 << 0)  /* extra dmesg output       */
#define CXLMM_CFG_FLAG_DRY_RUN    (1 << 1)  /* classify but don't migrate */
#define CXLMM_CFG_FLAG_BW_MON_OFF (1 << 2)  /* disable IMC monitoring   */

struct cxlmm_config {
	__u32  scan_interval_ms;     /* kthread wakeup period (default: 1000) */
	__u32  write_threshold_pct;  /* write% above which → CXL (default: 60) */
	__u32  min_scans;            /* minimum scans before classifying (def: 3) */
	__s32  cxl_node_id;          /* NUMA node of CXL memory                */
	__s32  ddr_node_id;          /* NUMA node of local DDR5                */
	__u32  migration_batch_size; /* pages per move_pages() call (def: 64)  */
	__u32  max_tracked_pids;     /* upper bound on tracked PIDs (def: 64)  */
	__u32  score_ring_size;      /* score ring capacity (def: 4096)        */
	__u32  flags;                /* CXLMM_CFG_FLAG_* bitmask               */
	__u32  _pad;
};

/* --------------------------------------------------------------------------
 * TRACK / UNTRACK request
 * -------------------------------------------------------------------------- */

struct cxlmm_track_req {
	__u32  pid;
	__u32  flags;   /* reserved, must be 0 */
};

/* --------------------------------------------------------------------------
 * Score fetch request / response
 *
 * Userspace passes a pointer+count; kernel fills up to 'count' entries and
 * sets 'filled' to the actual number written.
 * -------------------------------------------------------------------------- */

struct cxlmm_score_fetch {
	__u64  buf_ptr;    /* userspace pointer to struct cxlmm_page_score[] */
	__u32  count;      /* capacity of buf_ptr (in records)               */
	__u32  filled;     /* kernel output: records actually written        */
	__u32  pid_filter; /* 0 = all tracked pids; else filter by pid       */
	__u32  _pad;
};

/* --------------------------------------------------------------------------
 * ioctl commands
 * -------------------------------------------------------------------------- */

#define CXLMM_IOC_MAGIC         'X'

/** TRACK: start monitoring a PID */
#define CXLMM_IOC_TRACK         _IOW(CXLMM_IOC_MAGIC, 1, struct cxlmm_track_req)
/** UNTRACK: stop monitoring a PID */
#define CXLMM_IOC_UNTRACK       _IOW(CXLMM_IOC_MAGIC, 2, __u32)
/** GET_BW: read latest IMC bandwidth snapshot */
#define CXLMM_IOC_GET_BW        _IOR(CXLMM_IOC_MAGIC, 3, struct cxlmm_bw_stats)
/** GET_CFG: read current module config */
#define CXLMM_IOC_GET_CFG       _IOR(CXLMM_IOC_MAGIC, 4, struct cxlmm_config)
/** SET_CFG: atomically replace module config */
#define CXLMM_IOC_SET_CFG       _IOW(CXLMM_IOC_MAGIC, 5, struct cxlmm_config)
/** FETCH_SCORES: drain score ring into userspace buffer */
#define CXLMM_IOC_FETCH_SCORES  _IOWR(CXLMM_IOC_MAGIC, 6, struct cxlmm_score_fetch)

#define CXLMM_IOC_MAXNR 6

#endif /* _CXLMM_UAPI_H */
