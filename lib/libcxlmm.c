/*
 * libcxlmm.c — malloc-like API implementation for CXL-aware memory placement
 *
 * Design:
 *   - Uses mmap(MAP_ANONYMOUS|MAP_PRIVATE) so pages are known to the kernel.
 *   - Tracks each allocation in an internal list (mutex-protected) so
 *     cxlmm_free can munmap the correct size.
 *   - Registers the calling process with the kernel module on init so the
 *     scanner kthread starts monitoring its pages.
 *   - Falls back to malloc/free if /dev/cxlmm is not available.
 *
 * Thread safety: cxlmm_alloc/cxlmm_free are thread-safe.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <pthread.h>

#include "libcxlmm.h"
#include "../include/cxlmm_uapi.h"

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

static int          g_dev_fd  = -1;   /* /dev/cxlmm fd          */
static int          g_avail   = 0;    /* 1 if module connected   */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Per-allocation record so cxlmm_free can munmap the right size */
struct alloc_entry {
	void   *ptr;
	size_t  size;           /* original requested size (rounded up) */
	size_t  mmap_size;      /* actual mmap'd size (page-aligned)    */
	struct alloc_entry *next;
};

static struct alloc_entry *g_alloc_list = NULL;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static size_t page_align_up(size_t n)
{
	long page_size = sysconf(_SC_PAGE_SIZE);
	if (page_size <= 0)
		page_size = 4096;
	return (n + (size_t)page_size - 1) & ~((size_t)page_size - 1);
}

static void alloc_list_add(void *ptr, size_t requested, size_t mmap_sz)
{
	struct alloc_entry *entry = malloc(sizeof(*entry));
	if (!entry)
		return;  /* OOM in bookkeeping; ptr will leak on free */
	entry->ptr       = ptr;
	entry->size      = requested;
	entry->mmap_size = mmap_sz;

	pthread_mutex_lock(&g_lock);
	entry->next  = g_alloc_list;
	g_alloc_list = entry;
	pthread_mutex_unlock(&g_lock);
}

/* Returns the mmap_size for ptr, or 0 if not found; removes entry */
static size_t alloc_list_remove(void *ptr)
{
	struct alloc_entry **pp;
	size_t mmap_sz = 0;

	pthread_mutex_lock(&g_lock);
	for (pp = &g_alloc_list; *pp; pp = &(*pp)->next) {
		if ((*pp)->ptr == ptr) {
			struct alloc_entry *e = *pp;
			*pp = e->next;
			mmap_sz = e->mmap_size;
			free(e);
			break;
		}
	}
	pthread_mutex_unlock(&g_lock);
	return mmap_sz;
}

/* --------------------------------------------------------------------------
 * Public: lifecycle
 * -------------------------------------------------------------------------- */

int cxlmm_init(void)
{
	struct cxlmm_track_req req = {};
	int fd, ret;

	pthread_mutex_lock(&g_lock);

	if (g_avail) {
		pthread_mutex_unlock(&g_lock);
		return 0;  /* already initialised */
	}

	fd = open("/dev/cxlmm", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "cxlmm: /dev/cxlmm not available (%s); "
			"falling back to malloc\n", strerror(errno));
		pthread_mutex_unlock(&g_lock);
		return -errno;
	}

	/* Register this process */
	req.pid   = (uint32_t)getpid();
	req.flags = 0;
	ret = ioctl(fd, CXLMM_IOC_TRACK, &req);
	if (ret < 0) {
		int saved = errno;
		fprintf(stderr, "cxlmm: TRACK ioctl failed: %s\n", strerror(saved));
		close(fd);
		pthread_mutex_unlock(&g_lock);
		return -saved;
	}

	g_dev_fd = fd;
	g_avail  = 1;

	pthread_mutex_unlock(&g_lock);
	return 0;
}

void cxlmm_fini(void)
{
	pthread_mutex_lock(&g_lock);

	if (!g_avail) {
		pthread_mutex_unlock(&g_lock);
		return;
	}

	/* Unregister this process */
	{
		uint32_t pid = (uint32_t)getpid();
		ioctl(g_dev_fd, CXLMM_IOC_UNTRACK, &pid);
	}

	close(g_dev_fd);
	g_dev_fd = -1;
	g_avail  = 0;

	pthread_mutex_unlock(&g_lock);
}

/* --------------------------------------------------------------------------
 * Public: allocation
 * -------------------------------------------------------------------------- */

void *cxlmm_alloc(size_t size)
{
	size_t mmap_sz;
	void  *ptr;

	if (size == 0)
		return NULL;

	if (!g_avail)
		return malloc(size);

	mmap_sz = page_align_up(size);

	ptr = mmap(NULL, mmap_sz, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (ptr == MAP_FAILED) {
		perror("cxlmm_alloc: mmap");
		return NULL;
	}

	alloc_list_add(ptr, size, mmap_sz);
	return ptr;
}

void cxlmm_free(void *ptr)
{
	size_t mmap_sz;

	if (!ptr)
		return;

	if (!g_avail) {
		free(ptr);
		return;
	}

	mmap_sz = alloc_list_remove(ptr);
	if (mmap_sz == 0) {
		/* Not in our list — might be a malloc'd fallback ptr */
		free(ptr);
		return;
	}

	munmap(ptr, mmap_sz);
}

/* --------------------------------------------------------------------------
 * Public: diagnostics
 * -------------------------------------------------------------------------- */

int cxlmm_get_fd(void)
{
	return g_dev_fd;
}

int cxlmm_is_available(void)
{
	return g_avail;
}
