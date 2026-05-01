/*
 * cxlmm_daemon.c : CXL-aware memory manager userspace daemon
 *
 * Usage:
 *   cxlmm_daemon [OPTIONS]
 *
 * Options:
 *   --cxl-node N      NUMA node for CXL memory (required)
 *   --ddr-node N      NUMA node for DDR memory (default: 0)
 *   --track-pid PID   PID to track (may be repeated; or use --track-all)
 *   --scan-ms N       scan interval in ms (default: 1000)
 *   --write-thr N     write threshold percent for CXL classification (default: 60)
 *   --min-scans N     minimum scan cycles before classifying (default: 3)
 *   --batch N         migration batch size per cycle (default: 64)
 *   --dry-run         classify pages but don't migrate
 *   --verbose         extra logging
 *   --help            print this help
 *
 * Main loop:
 *   1. ioctl(FETCH_SCORES) : drain kernel write-score ring
 *   2. pagemap_clear_refs() + sleep + pagemap_scan() : detect reads
 *   3. classifier_migration_batch() : find misplaced pages
 *   4. move_pages(2) per PID batch : migrate pages
 *   5. classifier_reset_page() for successfully migrated pages
 *   6. sleep remaining interval
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <numaif.h>    /* move_pages */
#include <numa.h>      /* numa_available */

#include "../include/cxlmm_uapi.h"
#include "../lib/pagemap_scan.h"
#include "classifier.h"

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

#define MAX_TRACKED_PIDS  64
#define FETCH_BUF_SIZE    (128 * 1024)  /* score records per ioctl call */
#define MIGRATE_BATCH     64    /* pages per move_pages() call  */

struct daemon_config {
	int      cxl_node;
	int      ddr_node;
	pid_t    pids[MAX_TRACKED_PIDS];
	int      pid_count;
	unsigned scan_ms;
	unsigned write_thr;
	unsigned min_scans;
	unsigned batch;
	int      dry_run;
	int      verbose;
};

/* --------------------------------------------------------------------------
 * Global state
 * -------------------------------------------------------------------------- */

static volatile int g_running = 1;
static int          g_dev_fd  = -1;

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* --------------------------------------------------------------------------
 * Pagemap read callback : feeds read detections into classifier
 * -------------------------------------------------------------------------- */

struct pagemap_ctx {
	struct classifier *cl;
	pid_t              pid;
};

static int on_accessed_page(uint64_t vaddr, pid_t pid, void *ctx_ptr)
{
	struct pagemap_ctx *ctx = ctx_ptr;
	classifier_ingest_read(ctx->cl, vaddr, (uint32_t)pid);
	return 0;
}

/* --------------------------------------------------------------------------
 * Migrate one PID's batch using move_pages(2)
 * -------------------------------------------------------------------------- */

static void do_migrate(struct daemon_config *cfg,
		       struct classifier    *cl,
		       pid_t                 pid)
{
	void   *vaddrs[MIGRATE_BATCH];
	int     nodes[MIGRATE_BATCH];
	int     status[MIGRATE_BATCH];
	int n, ret, i;
	int target_nodes[MIGRATE_BATCH];

	n = classifier_migration_batch(cl, (uint32_t)pid, vaddrs, nodes, MIGRATE_BATCH);
	if (n == 0)
		return;

	if (cfg->verbose)
		fprintf(stderr, "[daemon] migrating %d pages for pid %d\n", n, (int)pid);

	if (cfg->dry_run) {
		for (i = 0; i < n; i++) {
			fprintf(stderr, "[dry-run] pid=%d vaddr=%p → node %d\n",
				(int)pid, vaddrs[i], nodes[i]);
		}
		/* Reset pages without migration in dry-run mode */
		for (i = 0; i < n; i++) {
			classifier_reset_page(cl, (uint64_t)(uintptr_t)vaddrs[i],
					      pid, nodes[i]);
		}
		return;
	}

	/* move_pages expects an array of int* for nodes */
	for (i = 0; i < n; i++)
		target_nodes[i] = nodes[i];

	memset(status, 0, sizeof(int) * (size_t)n);

	/*
	 * move_pages(2): move pages of process @pid to specified NUMA nodes.
	 * Signature: move_pages(pid, count, pages[], nodes[], status[], flags)
	 * MPOL_MF_MOVE: move pages owned by this process (not shared).
	 */
	ret = (int)move_pages(pid, (unsigned long)n,
			      vaddrs, target_nodes, status,
			      MPOL_MF_MOVE);
	if (ret < 0) {
		if (errno == ESRCH) {
			/* Process exited; purge its records */
			classifier_purge_pid(cl, (uint32_t)pid);
			return;
		}
		if (cfg->verbose)
			perror("[daemon] move_pages");
		/* Clear PENDING_MIG flags so we retry next cycle */
		for (i = 0; i < n; i++)
			classifier_clear_pending(cl,
						 (uint64_t)(uintptr_t)vaddrs[i],
						 (uint32_t)pid);
		return;
	}

	/* Process per-page status codes */
	for (i = 0; i < n; i++) {
		if (status[i] >= 0) {
			/* Successfully migrated or already on target node */
			classifier_reset_page(cl, (uint64_t)(uintptr_t)vaddrs[i],
					      pid, nodes[i]);
			if (cfg->verbose)
				fprintf(stderr, "[daemon]   migrated %p → node %d\n",
					vaddrs[i], nodes[i]);
		} else {
			/*
			 * Migration failed for this page (e.g. page pinned,
			 * -EACCES for non-owned page). Clear PENDING_MIG so
			 * we retry next cycle. Scores and current_node are
			 * preserved so classification stays accurate.
			 */
			if (cfg->verbose)
				fprintf(stderr, "[daemon]   failed %p: %s\n",
					vaddrs[i], strerror(-status[i]));
			classifier_clear_pending(cl,
						 (uint64_t)(uintptr_t)vaddrs[i],
						 (uint32_t)pid);
		}
	}
}

/* --------------------------------------------------------------------------
 * One classify-and-migrate cycle
 * -------------------------------------------------------------------------- */

static void run_cycle(struct daemon_config *cfg,
		      struct classifier    *cl)
{
	struct cxlmm_page_score *score_buf;
	struct cxlmm_score_fetch fetch;
	int i;

	score_buf = calloc(FETCH_BUF_SIZE, sizeof(*score_buf));
	if (!score_buf) {
		perror("calloc score_buf");
		return;
	}

	/* 1. Fetch kernel write scores */
	memset(&fetch, 0, sizeof(fetch));
	fetch.buf_ptr   = (uint64_t)(uintptr_t)score_buf;
	fetch.count     = FETCH_BUF_SIZE;
	fetch.pid_filter = 0;  /* all PIDs */

	if (ioctl(g_dev_fd, CXLMM_IOC_FETCH_SCORES, &fetch) < 0) {
		perror("[daemon] FETCH_SCORES ioctl");
	} else {
		if (cfg->verbose)
			fprintf(stderr, "[daemon] fetched %u write scores\n",
				fetch.filled);
		for (i = 0; i < (int)fetch.filled; i++)
			classifier_ingest_score(cl, &score_buf[i]);
	}

	free(score_buf);

	/* 2. Userspace read detection via pagemap */
	for (i = 0; i < cfg->pid_count; i++) {
		pid_t pid = cfg->pids[i];
		struct pagemap_ctx ctx = { .cl = cl, .pid = pid };
		int n;

		/* Clear soft-dirty bits */
		if (pagemap_clear_refs(pid) < 0) {
			if (cfg->verbose)
				fprintf(stderr, "[daemon] clear_refs pid %d failed\n",
					(int)pid);
			continue;
		}

		/* Wait a short interval for page accesses to accumulate */
		usleep(100 * 1000);  /* 100 ms */

		/* Scan for accessed (soft-dirty) pages */
		n = pagemap_scan(pid, on_accessed_page, &ctx);
		if (n < 0) {
			/* Process exited */
			classifier_purge_pid(cl, (uint32_t)pid);
		} else if (cfg->verbose) {
			fprintf(stderr, "[daemon] pid %d: %d read pages\n",
				(int)pid, n);
		}
	}

	/* 3 & 4. Classify and migrate per PID */
	if (cfg->verbose) {
		/* Debug: count classification categories */
		uint32_t n_unk = 0, n_wr = 0, n_rd = 0, n_bal = 0;
		for (uint32_t j = 0; j < cl->capacity; j++) {
			if (cl->records[j].pid == 0) continue;
			page_class_t c = classifier_classify_page(cl,
				cl->records[j].vaddr, cl->records[j].pid);
			switch (c) {
			case CLASS_UNKNOWN:     n_unk++; break;
			case CLASS_WRITE_HEAVY: n_wr++;  break;
			case CLASS_READ_HEAVY:  n_rd++;  break;
			case CLASS_BALANCED:    n_bal++; break;
			}
		}
		/* Also sample one record for debugging */
		uint32_t sample_ws = 0, sample_rs = 0, sample_sc = 0;
		for (uint32_t j = 0; j < cl->capacity; j++) {
			if (cl->records[j].pid != 0 && cl->records[j].write_score > 0) {
				sample_ws = cl->records[j].write_score;
				sample_rs = cl->records[j].read_score;
				sample_sc = cl->records[j].scan_count;
				break;
			}
		}
		fprintf(stderr, "[daemon] classify: %u unknown, %u write-heavy, "
			"%u read-heavy, %u balanced (table=%u) "
			"[sample: ws=%u rs=%u sc=%u]\n",
			n_unk, n_wr, n_rd, n_bal, cl->count,
			sample_ws, sample_rs, sample_sc);
	}
	for (i = 0; i < cfg->pid_count; i++) {
		do_migrate(cfg, cl, cfg->pids[i]);
	}
}

/* --------------------------------------------------------------------------
 * Argument parsing
 * -------------------------------------------------------------------------- */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"\n"
		"Options:\n"
		"  --cxl-node N     NUMA node for CXL memory (required)\n"
		"  --ddr-node N     NUMA node for DDR memory (default: 0)\n"
		"  --track-pid PID  PID to track (repeatable)\n"
		"  --scan-ms N      scan interval ms (default: 1000)\n"
		"  --write-thr N    write%% threshold for CXL (default: 60)\n"
		"  --min-scans N    min scans before classifying (default: 3)\n"
		"  --batch N        migration batch size (default: 64)\n"
		"  --dry-run        classify but don't migrate\n"
		"  --verbose        extra logging\n"
		"  --help           show this help\n",
		prog);
}

static int parse_args(int argc, char **argv, struct daemon_config *cfg)
{
	static struct option opts[] = {
		{ "cxl-node",   required_argument, NULL, 'c' },
		{ "ddr-node",   required_argument, NULL, 'd' },
		{ "track-pid",  required_argument, NULL, 'p' },
		{ "scan-ms",    required_argument, NULL, 's' },
		{ "write-thr",  required_argument, NULL, 'w' },
		{ "min-scans",  required_argument, NULL, 'm' },
		{ "batch",      required_argument, NULL, 'b' },
		{ "dry-run",    no_argument,       NULL, 'n' },
		{ "verbose",    no_argument,       NULL, 'v' },
		{ "help",       no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	int opt;

	/* Defaults */
	cfg->cxl_node  = -1;
	cfg->ddr_node  = 0;
	cfg->pid_count = 0;
	cfg->scan_ms   = 1000;
	cfg->write_thr = 60;
	cfg->min_scans = 3;
	cfg->batch     = MIGRATE_BATCH;
	cfg->dry_run   = 0;
	cfg->verbose   = 0;

	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			cfg->cxl_node = atoi(optarg);
			break;
		case 'd':
			cfg->ddr_node = atoi(optarg);
			break;
		case 'p':
			if (cfg->pid_count >= MAX_TRACKED_PIDS) {
				fprintf(stderr, "Too many PIDs (max %d)\n",
					MAX_TRACKED_PIDS);
				return -1;
			}
			cfg->pids[cfg->pid_count++] = (pid_t)atoi(optarg);
			break;
		case 's':
			cfg->scan_ms = (unsigned)atoi(optarg);
			break;
		case 'w':
			cfg->write_thr = (unsigned)atoi(optarg);
			break;
		case 'm':
			cfg->min_scans = (unsigned)atoi(optarg);
			break;
		case 'b':
			cfg->batch = (unsigned)atoi(optarg);
			break;
		case 'n':
			cfg->dry_run = 1;
			break;
		case 'v':
			cfg->verbose = 1;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			return -1;
		}
	}

	if (cfg->cxl_node < 0) {
		fprintf(stderr, "Error: --cxl-node is required\n");
		usage(argv[0]);
		return -1;
	}
	if (cfg->pid_count == 0) {
		fprintf(stderr, "Error: at least one --track-pid is required\n");
		usage(argv[0]);
		return -1;
	}
	if (cfg->write_thr < 1 || cfg->write_thr > 99) {
		fprintf(stderr, "Error: --write-thr must be 1-99\n");
		return -1;
	}

	return 0;
}

/* --------------------------------------------------------------------------
 * Register PIDs with the kernel module
 * -------------------------------------------------------------------------- */

static int register_pids(struct daemon_config *cfg)
{
	int i;

	for (i = 0; i < cfg->pid_count; i++) {
		struct cxlmm_track_req req;
		req.pid   = (uint32_t)cfg->pids[i];
		req.flags = 0;

		if (ioctl(g_dev_fd, CXLMM_IOC_TRACK, &req) < 0) {
			if (errno == EEXIST) {
				if (cfg->verbose)
					fprintf(stderr,
						"[daemon] pid %d already tracked\n",
						(int)cfg->pids[i]);
				continue;
			}
			fprintf(stderr, "[daemon] TRACK pid %d: %s\n",
				(int)cfg->pids[i], strerror(errno));
			return -errno;
		}
		fprintf(stderr, "[daemon] tracking pid %d\n", (int)cfg->pids[i]);
	}
	return 0;
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	struct daemon_config cfg;
	struct classifier    cl;
	struct cxlmm_config  kmod_cfg;
	struct timespec      ts;
	int ret;

	if (parse_args(argc, argv, &cfg) < 0)
		return 1;

	/* Check NUMA availability */
	if (numa_available() < 0) {
		fprintf(stderr, "Error: NUMA not available on this system\n");
		return 1;
	}

	/* Open /dev/cxlmm */
	g_dev_fd = open("/dev/cxlmm", O_RDWR | O_CLOEXEC);
	if (g_dev_fd < 0) {
		perror("open /dev/cxlmm");
		return 1;
	}

	/* Read kernel config to synchronize parameters */
	if (ioctl(g_dev_fd, CXLMM_IOC_GET_CFG, &kmod_cfg) == 0) {
		/* Override CLI params with kernel module's config */
		if (cfg.verbose) {
			fprintf(stderr, "[daemon] kernel config: "
				"scan_ms=%u write_thr=%u cxl=%d ddr=%d\n",
				kmod_cfg.scan_interval_ms,
				kmod_cfg.write_threshold_pct,
				kmod_cfg.cxl_node_id,
				kmod_cfg.ddr_node_id);
		}
		/* Use kernel's node assignments if they match CLI */
		if (kmod_cfg.cxl_node_id != cfg.cxl_node ||
		    kmod_cfg.ddr_node_id  != cfg.ddr_node) {
			fprintf(stderr,
				"[daemon] warning: CLI nodes (%d,%d) differ "
				"from kernel module (%d,%d); using CLI values\n",
				cfg.cxl_node, cfg.ddr_node,
				kmod_cfg.cxl_node_id, kmod_cfg.ddr_node_id);
		}
	}

	/* Register PIDs with kernel module */
	ret = register_pids(&cfg);
	if (ret < 0) {
		close(g_dev_fd);
		return 1;
	}

	/* Initialize classifier */
	ret = classifier_init(&cl,
			      cfg.write_thr,
			      cfg.min_scans,
			      cfg.cxl_node,
			      cfg.ddr_node);
	if (ret < 0) {
		perror("classifier_init");
		close(g_dev_fd);
		return 1;
	}

	/* Signal handling */
	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);

	fprintf(stderr, "[daemon] started: cxl_node=%d ddr_node=%d "
		"scan_ms=%u write_thr=%u%% min_scans=%u%s\n",
		cfg.cxl_node, cfg.ddr_node,
		cfg.scan_ms, cfg.write_thr, cfg.min_scans,
		cfg.dry_run ? " [DRY-RUN]" : "");

	/* Main loop */
	while (g_running) {
		run_cycle(&cfg, &cl);

		/* Sleep for scan_ms */
		ts.tv_sec  = cfg.scan_ms / 1000;
		ts.tv_nsec = (long)(cfg.scan_ms % 1000) * 1000000L;
		nanosleep(&ts, NULL);
	}

	fprintf(stderr, "[daemon] shutting down\n");

	/* Unregister PIDs */
	{
		int i;
		for (i = 0; i < cfg.pid_count; i++) {
			uint32_t pid = (uint32_t)cfg.pids[i];
			ioctl(g_dev_fd, CXLMM_IOC_UNTRACK, &pid);
		}
	}

	classifier_fini(&cl);
	close(g_dev_fd);
	return 0;
}
