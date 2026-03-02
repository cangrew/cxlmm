/*
 * libcxlmm.h — public malloc-like API for CXL-aware memory allocation
 *
 * Applications link against libcxlmm.so and use cxlmm_alloc / cxlmm_free
 * instead of malloc/free to opt in to bandwidth-aware page placement.
 *
 * Internally, cxlmm_alloc:
 *   1. mmap()s anonymous memory
 *   2. registers the calling process with the cxlmm kernel module (via ioctl)
 *   3. returns the pointer to the caller
 *
 * The kernel module + daemon then monitor and migrate pages over time.
 * Applications don't need to change placement logic — cxlmm does it.
 */
#ifndef _LIBCXLMM_H
#define _LIBCXLMM_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Library lifecycle
 * -------------------------------------------------------------------------- */

/**
 * cxlmm_init - connect to the cxlmm kernel module
 *
 * Opens /dev/cxlmm and registers the calling process as a tracked PID.
 * Must be called before cxlmm_alloc / cxlmm_free.
 *
 * Returns 0 on success, -errno on failure.
 * On failure the library enters a passthrough mode: cxlmm_alloc falls back
 * to malloc and cxlmm_free falls back to free.
 */
int cxlmm_init(void);

/**
 * cxlmm_fini - disconnect from the kernel module
 *
 * Unregisters the calling process and closes /dev/cxlmm.
 * All cxlmm_alloc'd memory must be freed before calling this.
 */
void cxlmm_fini(void);

/* --------------------------------------------------------------------------
 * Allocation API
 * -------------------------------------------------------------------------- */

/**
 * cxlmm_alloc - allocate size bytes of bandwidth-monitored memory
 *
 * Returns a page-aligned pointer, or NULL on failure.
 * The memory is zero-initialized (from mmap MAP_ANONYMOUS).
 * Falls back to malloc if the module is not available.
 */
void *cxlmm_alloc(size_t size);

/**
 * cxlmm_free - free memory returned by cxlmm_alloc
 */
void cxlmm_free(void *ptr);

/* --------------------------------------------------------------------------
 * Diagnostic / control API
 * -------------------------------------------------------------------------- */

/**
 * cxlmm_get_fd - return the /dev/cxlmm file descriptor, or -1 if not open
 */
int cxlmm_get_fd(void);

/**
 * cxlmm_is_available - return 1 if the module is loaded and connected
 */
int cxlmm_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* _LIBCXLMM_H */
