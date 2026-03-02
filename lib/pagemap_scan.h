/*
 * pagemap_scan.h — /proc/pid/pagemap reader for access-bit detection
 *
 * Usage pattern:
 *   1. pagemap_clear_refs(pid)       — write "4" to /proc/pid/clear_refs
 *      (clears the Accessed bit in all PTEs without affecting page contents)
 *   2. <wait one scan interval>
 *   3. pagemap_scan(pid, cb, ctx)    — reads /proc/pid/pagemap and /proc/pid/maps,
 *      calls cb() for each page whose Present + SoftDirty flags are set.
 *
 * Note: clear_refs type 4 = soft-dirty tracking reset (requires CONFIG_MEM_SOFT_DIRTY).
 * Fallback: type 1 clears all accessed bits (broader, may affect page reclaim).
 */
#ifndef _PAGEMAP_SCAN_H
#define _PAGEMAP_SCAN_H

#include <stdint.h>
#include <sys/types.h>

/* Callback invoked for each accessed page found in pagemap.
 * vaddr:   page-aligned virtual address
 * pid:     process being scanned
 * ctx:     caller-supplied opaque context
 * Returns: 0 to continue, non-zero to stop the scan early.
 */
typedef int (*pagemap_page_cb)(uint64_t vaddr, pid_t pid, void *ctx);

/**
 * pagemap_clear_refs - reset soft-dirty tracking for all pages in a process
 *
 * Writes "4\n" to /proc/<pid>/clear_refs.
 * Returns 0 on success, -errno on failure.
 */
int pagemap_clear_refs(pid_t pid);

/**
 * pagemap_scan - iterate accessed pages via /proc/pid/pagemap
 *
 * Reads /proc/<pid>/maps to enumerate VMAs, then reads /proc/<pid>/pagemap
 * to find pages with the Present flag set and the Soft-Dirty flag set
 * (indicating a read or write since the last clear_refs).
 *
 * For each such page, calls cb(vaddr, pid, ctx).
 *
 * Returns total number of accessed pages found, or -errno on fatal error.
 */
int pagemap_scan(pid_t pid, pagemap_page_cb cb, void *ctx);

/* --------------------------------------------------------------------------
 * Pagemap entry bit layout (from linux/Documentation/admin-guide/mm/pagemap.rst)
 * -------------------------------------------------------------------------- */

/* Bit 63: page is present in RAM */
#define PAGEMAP_BIT_PRESENT   (1ULL << 63)
/* Bit 62: page is in swap */
#define PAGEMAP_BIT_SWAPPED   (1ULL << 62)
/* Bit 55: soft-dirty (written/accessed since last clear_refs=4) */
#define PAGEMAP_BIT_SOFT_DIRTY (1ULL << 55)
/* Bit 61: page exclusively mapped */
#define PAGEMAP_BIT_EXCL      (1ULL << 61)

#endif /* _PAGEMAP_SCAN_H */
