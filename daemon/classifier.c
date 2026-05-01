/*
 * classifier.c : page classification engine implementation
 *
 * Uses open-addressing hash table keyed on (vaddr, pid) pairs.
 * The table size is a power of 2 so we use bitmasking instead of modulo.
 *
 * Immutability: records are updated by returning a new value into the slot,
 * not via partial field mutation; entire struct page_record is replaced.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "classifier.h"
#include "../include/cxlmm_uapi.h"

/* --------------------------------------------------------------------------
 * Hash table internals
 * -------------------------------------------------------------------------- */

/* Sentinel: empty slot has pid = 0 (no real process has pid 0) */
#define SLOT_EMPTY_PID 0

static uint32_t hash_key(uint64_t vaddr, uint32_t pid)
{
	/* FNV-1a-inspired mix */
	uint64_t h = 14695981039346656037ULL;
	h ^= vaddr;
	h *= 1099511628211ULL;
	h ^= (uint64_t)(uint32_t)pid;
	h *= 1099511628211ULL;
	return (uint32_t)(h ^ (h >> 32));
}

/* Find a slot for (vaddr, pid); returns pointer to slot (may be empty).
 * Returns NULL only if the table is completely full (should not happen
 * if load factor is maintained below 0.75). */
static struct page_record *table_find(struct page_record *records,
				      uint32_t capacity,
				      uint64_t vaddr, uint32_t pid)
{
	uint32_t idx = hash_key(vaddr, pid) & (capacity - 1);
	uint32_t probe = 0;

	while (probe < capacity) {
		struct page_record *slot = &records[idx];
		if (slot->pid == SLOT_EMPTY_PID)
			return slot;   /* empty slot : caller may insert here */
		if (slot->vaddr == vaddr && slot->pid == pid)
			return slot;   /* found existing record */
		idx = (idx + 1) & (capacity - 1);
		probe++;
	}
	return NULL;  /* table full */
}

/* --------------------------------------------------------------------------
 * Public: init / fini
 * -------------------------------------------------------------------------- */

int classifier_init(struct classifier *cl,
		    uint32_t write_threshold_pct,
		    uint32_t min_scans,
		    int cxl_node,
		    int ddr_node)
{
	memset(cl, 0, sizeof(*cl));

	cl->capacity = CLASSIFIER_MAX_PAGES;
	cl->records  = calloc(cl->capacity, sizeof(struct page_record));
	if (!cl->records)
		return -ENOMEM;

	cl->write_threshold_pct = write_threshold_pct;
	cl->min_scans           = min_scans;
	cl->cxl_node            = cxl_node;
	cl->ddr_node            = ddr_node;
	cl->count               = 0;
	return 0;
}

void classifier_fini(struct classifier *cl)
{
	free(cl->records);
	memset(cl, 0, sizeof(*cl));
}

/* --------------------------------------------------------------------------
 * Public: ingest kernel write score
 * -------------------------------------------------------------------------- */

int classifier_ingest_score(struct classifier *cl,
			    const struct cxlmm_page_score *score)
{
	struct page_record *slot;
	struct page_record  new_rec;

	/* Safety: reject 0-pid entries from a broken ring */
	if (score->pid == 0)
		return 0;

	if (cl->count >= (cl->capacity * 3 / 4)) {
		fprintf(stderr, "classifier: table 75%% full (%u/%u); "
			"some pages dropped\n", cl->count, cl->capacity);
		return -ENOMEM;
	}

	slot = table_find(cl->records, cl->capacity, score->vaddr, score->pid);
	if (!slot)
		return -ENOMEM;

	if (slot->pid == SLOT_EMPTY_PID) {
		/* New entry : build a fresh record (immutable pattern) */
		new_rec.vaddr        = score->vaddr;
		new_rec.pid          = score->pid;
		new_rec.write_score  = score->write_score;
		new_rec.read_score   = 0;
		new_rec.scan_count   = score->scan_count;
		new_rec.current_node = score->current_node;
		new_rec.flags        = score->flags;
		new_rec._pad         = 0;
		*slot = new_rec;
		cl->count++;
	} else {
		/* Update existing record (create new value, assign to slot) */
		new_rec = *slot;
		new_rec.write_score  += score->write_score;
		new_rec.scan_count   += score->scan_count;
		new_rec.current_node  = score->current_node;
		new_rec.flags         = score->flags;
		*slot = new_rec;
	}

	return 0;
}

/* --------------------------------------------------------------------------
 * Public: ingest userspace read (pagemap soft-dirty hit)
 * -------------------------------------------------------------------------- */

void classifier_ingest_read(struct classifier *cl,
			    uint64_t vaddr, uint32_t pid)
{
	struct page_record *slot;
	struct page_record  new_rec;

	slot = table_find(cl->records, cl->capacity, vaddr, pid);
	if (!slot || slot->pid == SLOT_EMPTY_PID)
		return;  /* unknown page; skip */

	/* Immutable update */
	new_rec             = *slot;
	new_rec.read_score += 1;
	*slot = new_rec;
}

/* --------------------------------------------------------------------------
 * Public: classify one page
 * -------------------------------------------------------------------------- */

page_class_t classifier_classify_page(const struct classifier *cl,
				      uint64_t vaddr, uint32_t pid)
{
	const struct page_record *slot;
	uint32_t total;
	uint32_t write_pct;

	slot = table_find(cl->records, cl->capacity, vaddr, pid);
	if (!slot || slot->pid == SLOT_EMPTY_PID)
		return CLASS_UNKNOWN;

	if (slot->scan_count < cl->min_scans)
		return CLASS_UNKNOWN;

	total = slot->write_score + slot->read_score;
	if (total == 0)
		return CLASS_BALANCED;

	write_pct = (slot->write_score * 100) / total;

	if (write_pct >= cl->write_threshold_pct)
		return CLASS_WRITE_HEAVY;
	if (write_pct <= (100 - cl->write_threshold_pct))
		return CLASS_READ_HEAVY;
	return CLASS_BALANCED;
}

/* --------------------------------------------------------------------------
 * Public: build migration batch
 * -------------------------------------------------------------------------- */

int classifier_migration_batch(struct classifier *cl,
			       uint32_t pid_filter,
			       void    **vaddrs,
			       int      *nodes,
			       int       max)
{
	uint32_t i;
	int n = 0;
	uint32_t pid_filter_u32 = (uint32_t)pid_filter;

	for (i = 0; i < cl->capacity && n < max; i++) {
		struct page_record *rec = &cl->records[i];
		page_class_t cls;
		int target_node;

		if (rec->pid == SLOT_EMPTY_PID)
			continue;
		if (pid_filter_u32 && rec->pid != pid_filter_u32)
			continue;
		if (rec->flags & CXLMM_PFLAG_PENDING_MIG)
			continue;

		cls = classifier_classify_page(cl, rec->vaddr, rec->pid);
		if (cls == CLASS_UNKNOWN || cls == CLASS_BALANCED)
			continue;

		if (cls == CLASS_WRITE_HEAVY) {
			if (rec->current_node == (uint8_t)cl->cxl_node)
				continue;  /* already on CXL */
			target_node = cl->cxl_node;
		} else {
			if (rec->current_node == (uint8_t)cl->ddr_node)
				continue;  /* already on DDR */
			target_node = cl->ddr_node;
		}

		vaddrs[n] = (void *)(uintptr_t)rec->vaddr;
		nodes[n]  = target_node;
		n++;

		/* Mark as pending migration (immutable update) */
		{
			struct page_record new_rec = *rec;
			new_rec.flags |= CXLMM_PFLAG_PENDING_MIG;
			*rec = new_rec;
		}
	}

	return n;
}

/* --------------------------------------------------------------------------
 * Public: reset after successful migration
 * -------------------------------------------------------------------------- */

void classifier_reset_page(struct classifier *cl,
			   uint64_t vaddr, uint32_t pid, int new_node)
{
	struct page_record *slot;
	struct page_record  new_rec;

	slot = table_find(cl->records, cl->capacity, vaddr, pid);
	if (!slot || slot->pid == SLOT_EMPTY_PID)
		return;

	/* Immutable update: reset scores, update node, clear pending flag */
	new_rec              = *slot;
	new_rec.write_score  = 0;
	new_rec.read_score   = 0;
	new_rec.scan_count   = 0;
	new_rec.current_node = (uint8_t)new_node;
	new_rec.flags       &= ~CXLMM_PFLAG_PENDING_MIG;
	new_rec.flags       &= ~(CXLMM_PFLAG_WRITE_HEAVY | CXLMM_PFLAG_READ_HEAVY);
	new_rec.flags       |= (new_node == cl->cxl_node)
				? CXLMM_PFLAG_ON_CXL
				: CXLMM_PFLAG_ON_DDR;
	*slot = new_rec;
}

/* --------------------------------------------------------------------------
 * Public: clear pending-migration flag only (migration failed, retry next cycle)
 * -------------------------------------------------------------------------- */

void classifier_clear_pending(struct classifier *cl,
			      uint64_t vaddr, uint32_t pid)
{
	struct page_record *slot;
	struct page_record  new_rec;

	slot = table_find(cl->records, cl->capacity, vaddr, pid);
	if (!slot || slot->pid == SLOT_EMPTY_PID)
		return;

	new_rec        = *slot;
	new_rec.flags &= ~CXLMM_PFLAG_PENDING_MIG;
	*slot = new_rec;
}

/* --------------------------------------------------------------------------
 * Public: purge a PID (process exited)
 * -------------------------------------------------------------------------- */

void classifier_purge_pid(struct classifier *cl, uint32_t pid)
{
	uint32_t i;
	uint32_t purged = 0;

	for (i = 0; i < cl->capacity; i++) {
		struct page_record *rec = &cl->records[i];
		if (rec->pid == (uint32_t)pid) {
			memset(rec, 0, sizeof(*rec));
			purged++;
		}
	}

	if (cl->count >= purged)
		cl->count -= purged;
	else
		cl->count = 0;
}
