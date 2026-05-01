/*
 * pagemap_scan.c : /proc/pid/pagemap reader
 *
 * _GNU_SOURCE enables pread, fopen, etc. in strict C11 mode.
 *
 * Reads /proc/<pid>/maps to enumerate VMAs, then for each writable VMA
 * reads /proc/<pid>/pagemap to detect accessed pages (soft-dirty bit).
 *
 * This implements the userspace side of read detection. The kernel module
 * handles write detection via folio_mkclean; here we detect any access
 * (read or write) since the last clear_refs.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>

#include "pagemap_scan.h"

#define MAPS_LINE_MAX  512
#define PAGEMAP_BATCH  512   /* entries read per pread call */

/* --------------------------------------------------------------------------
 * pagemap_clear_refs
 * -------------------------------------------------------------------------- */

int pagemap_clear_refs(pid_t pid)
{
	char path[64];
	int fd, ret = 0;
	const char *msg = "4\n";

	snprintf(path, sizeof(path), "/proc/%d/clear_refs", (int)pid);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		/* Process may have exited */
		return -errno;
	}

	if (write(fd, msg, strlen(msg)) < 0)
		ret = -errno;

	close(fd);
	return ret;
}

/* --------------------------------------------------------------------------
 * pagemap_scan helpers
 * -------------------------------------------------------------------------- */

struct scan_range {
	uint64_t start;  /* page-aligned start VA */
	uint64_t end;    /* exclusive end VA      */
};

/*
 * parse_maps_line: extract the [start, end) VA range from one /proc/maps line.
 * Returns 1 on success, 0 if the line should be skipped, -1 on parse error.
 *
 * We skip:
 *   - non-anonymous and non-writable regions (no read traffic expected)
 *   - [vvar]/[vdso]/[stack]/[heap] special regions are included
 *     (they can hold writable data we care about)
 *   - lines backed by special files like /dev/ are skipped
 */
static int parse_maps_line(const char *line, struct scan_range *range)
{
	unsigned long start, end;
	char perms[8];
	unsigned long offset;
	unsigned int dev_major, dev_minor;
	unsigned long inode;
	char path[MAPS_LINE_MAX];

	path[0] = '\0';

	if (sscanf(line, "%lx-%lx %7s %lx %x:%x %lu %511s",
		   &start, &end, perms, &offset,
		   &dev_major, &dev_minor, &inode, path) < 5)
		return -1;

	/* Skip non-readable regions */
	if (perms[0] != 'r' && perms[1] != 'w')
		return 0;

	/* Skip device-backed mappings */
	if (path[0] == '/' && strncmp(path, "/dev/", 5) == 0)
		return 0;

	range->start = (uint64_t)start;
	range->end   = (uint64_t)end;
	return 1;
}

/* --------------------------------------------------------------------------
 * pagemap_scan
 * -------------------------------------------------------------------------- */

int pagemap_scan(pid_t pid, pagemap_page_cb cb, void *ctx)
{
	char maps_path[64];
	char pagemap_path[64];
	FILE *maps_fp;
	int pagemap_fd;
	char line[MAPS_LINE_MAX];
	int total_accessed = 0;
	int ret = 0;

	snprintf(maps_path,    sizeof(maps_path),    "/proc/%d/maps",    (int)pid);
	snprintf(pagemap_path, sizeof(pagemap_path), "/proc/%d/pagemap", (int)pid);

	maps_fp = fopen(maps_path, "r");
	if (!maps_fp)
		return -errno;

	pagemap_fd = open(pagemap_path, O_RDONLY);
	if (pagemap_fd < 0) {
		fclose(maps_fp);
		return -errno;
	}

	while (fgets(line, sizeof(line), maps_fp)) {
		struct scan_range range;
		uint64_t vaddr;
		int parse_ret;

		parse_ret = parse_maps_line(line, &range);
		if (parse_ret <= 0)
			continue;

		/* Walk each page in this VMA range */
		vaddr = range.start;
		while (vaddr < range.end) {
			uint64_t pagemap_entries[PAGEMAP_BATCH];
			uint64_t page_index = vaddr >> 12;  /* divide by PAGE_SIZE */
			size_t n_pages = (range.end - vaddr) >> 12;
			size_t batch = (n_pages < PAGEMAP_BATCH) ? n_pages : PAGEMAP_BATCH;
			ssize_t bytes_read;
			size_t i;

			bytes_read = pread(pagemap_fd,
					   pagemap_entries,
					   batch * sizeof(uint64_t),
					   (off_t)(page_index * sizeof(uint64_t)));

			if (bytes_read <= 0) {
				/* Page may have been unmapped; skip VMA */
				break;
			}

			batch = (size_t)bytes_read / sizeof(uint64_t);

			for (i = 0; i < batch; i++) {
				uint64_t entry = pagemap_entries[i];
				uint64_t page_vaddr = vaddr + (uint64_t)(i << 12);

				/* Page must be present AND soft-dirty */
				if ((entry & PAGEMAP_BIT_PRESENT) &&
				    (entry & PAGEMAP_BIT_SOFT_DIRTY)) {
					total_accessed++;

					if (cb) {
						ret = cb(page_vaddr, pid, ctx);
						if (ret != 0) {
							/* Caller requested early stop */
							goto done;
						}
					}
				}
			}

			vaddr += (uint64_t)(batch << 12);
		}
	}

done:
	close(pagemap_fd);
	fclose(maps_fp);

	return (ret < 0) ? ret : total_accessed;
}
