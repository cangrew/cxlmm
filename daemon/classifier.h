/*
 * classifier.h : page classification engine
 *
 * Merges kernel-side write scores (from cxlmm_page_score.write_score) with
 * userspace read scores (from pagemap soft-dirty scanning) to decide whether
 * each page should be placed on the CXL node (write-heavy) or the DDR node
 * (read-heavy).
 *
 * Classification is only performed after a page has been observed for at least
 * min_scans cycles, to avoid reacting to transient access patterns.
 */
#ifndef _CLASSIFIER_H
#define _CLASSIFIER_H

#include <stdint.h>
#include <sys/types.h>

#include "../include/cxlmm_uapi.h"

/* Maximum number of page records the classifier tracks simultaneously */
#define CLASSIFIER_MAX_PAGES  (256 * 1024)

/* Classification result for a single page */
typedef enum {
	CLASS_UNKNOWN     = 0,  /* not enough data yet */
	CLASS_WRITE_HEAVY = 1,  /* migrate to CXL node */
	CLASS_READ_HEAVY  = 2,  /* migrate to DDR node */
	CLASS_BALANCED    = 3,  /* leave in place      */
} page_class_t;

/* Internal record for one tracked page */
struct page_record {
	uint64_t     vaddr;
	uint32_t     pid;           /* pid_t stored as uint32 (matches uapi) */
	uint32_t     write_score;   /* cumulative write detections */
	uint32_t     read_score;    /* cumulative read  detections */
	uint8_t      scan_count;    /* number of cycles observed   */
	uint8_t      current_node;  /* last known NUMA node        */
	uint8_t      flags;         /* CXLMM_PFLAG_* */
	uint8_t      _pad;
};

/* Classifier context */
struct classifier {
	struct page_record *records;   /* hash-table entries (open addressing) */
	uint32_t            capacity;  /* must be power of 2                   */
	uint32_t            count;     /* current number of tracked pages      */

	/* Configuration (copied from cxlmm_config at init) */
	uint32_t  write_threshold_pct;
	uint32_t  min_scans;
	int       cxl_node;
	int       ddr_node;
};

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * classifier_init - allocate and initialize a classifier context
 *
 * write_threshold_pct: write%  above which page → CXL (e.g. 60)
 * min_scans:           minimum observations before classifying (e.g. 3)
 * cxl_node:           NUMA node for write-heavy pages
 * ddr_node:           NUMA node for read-heavy pages
 *
 * Returns 0 on success, -ENOMEM on failure.
 */
int classifier_init(struct classifier *cl,
		    uint32_t write_threshold_pct,
		    uint32_t min_scans,
		    int cxl_node,
		    int ddr_node);

/**
 * classifier_fini - free resources
 */
void classifier_fini(struct classifier *cl);

/**
 * classifier_ingest_score - merge one cxlmm_page_score record into the table
 *
 * Called for each entry returned by CXLMM_IOC_FETCH_SCORES.
 * Returns 0 on success, -ENOMEM if the table is full.
 */
int classifier_ingest_score(struct classifier *cl,
			    const struct cxlmm_page_score *score);

/**
 * classifier_ingest_read - increment read_score for a page (pagemap hit)
 *
 * Called for each accessed page found by pagemap_scan.
 * The page must already exist in the table (from ingest_score);
 * if not, it is silently ignored.
 */
void classifier_ingest_read(struct classifier *cl,
			    uint64_t vaddr, uint32_t pid);

/**
 * classifier_classify_page - return classification for one page
 */
page_class_t classifier_classify_page(const struct classifier *cl,
				      uint64_t vaddr, uint32_t pid);

/**
 * classifier_migration_batch - collect pages ready for migration
 *
 * Fills @vaddrs[] and @nodes[] with up to @max entries that are classified
 * as WRITE_HEAVY or READ_HEAVY and are currently on the wrong node.
 *
 * @pid_filter: if non-zero, only include pages for this PID.
 *
 * Returns the number of entries written.
 */
int classifier_migration_batch(struct classifier *cl,
			       uint32_t pid_filter,
			       void    **vaddrs,
			       int      *nodes,
			       int       max);

/**
 * classifier_reset_page - clear scores for a page after successful migration
 */
void classifier_reset_page(struct classifier *cl,
			   uint64_t vaddr, uint32_t pid, int new_node);

/**
 * classifier_clear_pending - clear PENDING_MIG flag only, preserving scores
 *
 * Used when migration fails: marks the page as retryable next cycle without
 * resetting accumulated scores or corrupting current_node.
 */
void classifier_clear_pending(struct classifier *cl,
			      uint64_t vaddr, uint32_t pid);

/**
 * classifier_purge_pid - remove all records for a PID (process exited)
 */
void classifier_purge_pid(struct classifier *cl, uint32_t pid);

#endif /* _CLASSIFIER_H */
