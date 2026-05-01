/*
 * cxlmm_bench.c : realistic workload benchmarks for cxlmm evaluation
 *
 * Four workloads that simulate real application memory access patterns,
 * each allocatable via cxlmm_alloc() or plain malloc for comparison.
 *
 * Workloads:
 *   kv       Key-value store: large read-only hash table + write-ahead log
 *   graph    Graph traversal: read-only adjacency list + write-heavy rank array
 *   tsdb     Time-series DB: write-heavy ring buffer + read-heavy index
 *   phase    Phase change: bulk write load → read-heavy query serving
 *
 * Usage:
 *   ./cxlmm_bench --workload=kv [--no-cxlmm] [--rounds=N] [--size-mb=N]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>

#include "libcxlmm.h"

/* -------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------- */

#define DEFAULT_ROUNDS    30
#define DEFAULT_SIZE_MB   2048     /* 2 GB total working set */
#define PAGE_SIZE         4096
#define CACHE_LINE        64

/* -------------------------------------------------------------------------
 * Timing
 * ------------------------------------------------------------------------- */

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* -------------------------------------------------------------------------
 * Simple xorshift64 PRNG (fast, deterministic, no library dependency)
 * ------------------------------------------------------------------------- */

static uint64_t rng_state = 0xdeadbeefcafe1234ULL;

static uint64_t xorshift64(void)
{
	uint64_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

/* -------------------------------------------------------------------------
 * Memory allocation wrapper
 * ------------------------------------------------------------------------- */

static int use_cxlmm = 1;

static void *bench_alloc(size_t size)
{
	if (use_cxlmm)
		return cxlmm_alloc(size);
	return malloc(size);
}

static void bench_free(void *ptr)
{
	if (use_cxlmm)
		cxlmm_free(ptr);
	else
		free(ptr);
}

/* =========================================================================
 * Workload 1: Key-Value Store
 *
 * Simulates Redis/memcached: a large hash table populated once (read-only
 * after init), plus a write-ahead log that receives sequential appends.
 * Operations: 90% random GET (read from table), 10% SET (write to log).
 *
 * Expected: table pages → DDR (read-heavy), log pages → CXL (write-heavy)
 * ========================================================================= */

struct kv_state {
	uint8_t *table;      /* read-only hash table */
	uint8_t *wal;        /* write-ahead log */
	size_t   table_sz;
	size_t   wal_sz;
	size_t   wal_pos;
};

static void kv_init(struct kv_state *kv, size_t total_mb)
{
	/* 75% table, 25% WAL */
	kv->table_sz = (size_t)(total_mb * 3 / 4) * 1024 * 1024;
	kv->wal_sz   = (size_t)(total_mb / 4) * 1024 * 1024;
	kv->wal_pos  = 0;

	kv->table = bench_alloc(kv->table_sz);
	kv->wal   = bench_alloc(kv->wal_sz);

	if (!kv->table || !kv->wal) {
		fprintf(stderr, "kv: allocation failed\n");
		exit(1);
	}

	/* Populate table with pseudo-random data (simulates bulk load) */
	printf("  populating %zu MB hash table...\n", kv->table_sz / (1024*1024));
	for (size_t i = 0; i < kv->table_sz; i += CACHE_LINE)
		*(uint64_t *)(kv->table + i) = xorshift64();
}

static double kv_run_round(struct kv_state *kv, int ops_per_round)
{
	volatile uint64_t sink = 0;
	double t0 = now_ms();

	for (int i = 0; i < ops_per_round; i++) {
		uint64_t r = xorshift64();
		if ((r & 0xF) < 14) {
			/* ~90% GET: random read from hash table */
			size_t off = (xorshift64() % (kv->table_sz / CACHE_LINE)) * CACHE_LINE;
			sink += *(volatile uint64_t *)(kv->table + off);
		} else {
			/* ~10% SET: sequential write to WAL */
			size_t off = kv->wal_pos;
			*(volatile uint64_t *)(kv->wal + off) = r;
			kv->wal_pos = (off + CACHE_LINE) % kv->wal_sz;
		}
	}

	(void)sink;
	return now_ms() - t0;
}

static void kv_fini(struct kv_state *kv)
{
	bench_free(kv->table);
	bench_free(kv->wal);
}

/* =========================================================================
 * Workload 2: Graph Traversal (PageRank-like)
 *
 * CSR adjacency list (read-only, large) + rank array (written every
 * iteration, small). Random reads scatter across adjacency; writes
 * stream through rank array.
 *
 * Expected: adjacency pages → DDR (read-heavy), rank pages → CXL (write)
 * ========================================================================= */

struct graph_state {
	uint32_t *adj;        /* adjacency list (edge targets) */
	uint32_t *offsets;    /* CSR row offsets */
	float    *rank;       /* rank scores (updated each iteration) */
	float    *rank_new;   /* scratch for new scores */
	uint32_t  num_nodes;
	uint64_t  num_edges;
	size_t    adj_sz;
	size_t    off_sz;
	size_t    rank_sz;
};

static void graph_init(struct graph_state *g, size_t total_mb)
{
	/* ~90% adjacency, ~10% rank arrays */
	size_t adj_bytes = (size_t)(total_mb * 9 / 10) * 1024 * 1024;
	g->num_edges = adj_bytes / sizeof(uint32_t);
	g->num_nodes = (uint32_t)(g->num_edges / 16);  /* avg degree ~16 */
	if (g->num_nodes < 1024)
		g->num_nodes = 1024;

	g->adj_sz  = g->num_edges * sizeof(uint32_t);
	g->off_sz  = ((size_t)g->num_nodes + 1) * sizeof(uint32_t);
	g->rank_sz = (size_t)g->num_nodes * sizeof(float);

	g->adj      = bench_alloc(g->adj_sz);
	g->offsets  = bench_alloc(g->off_sz);
	g->rank     = bench_alloc(g->rank_sz);
	g->rank_new = bench_alloc(g->rank_sz);

	if (!g->adj || !g->offsets || !g->rank || !g->rank_new) {
		fprintf(stderr, "graph: allocation failed\n");
		exit(1);
	}

	printf("  generating graph: %u nodes, %lu edges (%zu MB adj)...\n",
	       g->num_nodes, (unsigned long)g->num_edges,
	       g->adj_sz / (1024*1024));

	/* Build power-law-ish CSR: each node gets between 1 and 64 edges */
	uint64_t edge_idx = 0;
	for (uint32_t n = 0; n < g->num_nodes; n++) {
		g->offsets[n] = (uint32_t)edge_idx;
		uint32_t degree = 1 + (xorshift64() % 32);
		for (uint32_t e = 0; e < degree && edge_idx < g->num_edges; e++) {
			g->adj[edge_idx++] = (uint32_t)(xorshift64() % g->num_nodes);
		}
	}
	g->offsets[g->num_nodes] = (uint32_t)edge_idx;
	g->num_edges = edge_idx;

	/* Initial rank: 1/N */
	float init_rank = 1.0f / (float)g->num_nodes;
	for (uint32_t i = 0; i < g->num_nodes; i++)
		g->rank[i] = init_rank;
}

static double graph_run_round(struct graph_state *g)
{
	const float damping = 0.85f;
	const float base = (1.0f - damping) / (float)g->num_nodes;
	double t0 = now_ms();

	/* Zero new ranks (write to rank_new) */
	memset(g->rank_new, 0, g->rank_sz);

	/* Scatter-add: for each node, read adjacency and distribute rank */
	for (uint32_t n = 0; n < g->num_nodes; n++) {
		uint32_t start = g->offsets[n];
		uint32_t end   = g->offsets[n + 1];
		uint32_t deg   = end - start;
		if (deg == 0) continue;

		float contrib = g->rank[n] / (float)deg;
		for (uint32_t e = start; e < end; e++) {
			uint32_t target = g->adj[e];      /* random READ from adj */
			g->rank_new[target] += contrib;    /* WRITE to rank_new */
		}
	}

	/* Apply damping */
	for (uint32_t i = 0; i < g->num_nodes; i++)
		g->rank_new[i] = base + damping * g->rank_new[i];

	/* Swap rank arrays for next iteration */
	float *tmp = g->rank;
	g->rank = g->rank_new;
	g->rank_new = tmp;

	return now_ms() - t0;
}

static void graph_fini(struct graph_state *g)
{
	bench_free(g->adj);
	bench_free(g->offsets);
	bench_free(g->rank);
	bench_free(g->rank_new);
}

/* =========================================================================
 * Workload 3: Time-Series DB (ingestion + query)
 *
 * Circular ring buffer receives streaming writes (telemetry ingestion).
 * A separate index structure is scanned for aggregation queries (reads).
 *
 * Expected: ring buffer → CXL (write-heavy), index → DDR (read-heavy)
 * ========================================================================= */

struct tsdb_state {
	uint8_t *ring;       /* circular write buffer (ingestion) */
	uint8_t *index;      /* read-only query index */
	size_t   ring_sz;
	size_t   index_sz;
	size_t   ring_pos;
};

static void tsdb_init(struct tsdb_state *ts, size_t total_mb)
{
	ts->ring_sz  = (size_t)(total_mb / 2) * 1024 * 1024;
	ts->index_sz = (size_t)(total_mb / 2) * 1024 * 1024;
	ts->ring_pos = 0;

	ts->ring  = bench_alloc(ts->ring_sz);
	ts->index = bench_alloc(ts->index_sz);

	if (!ts->ring || !ts->index) {
		fprintf(stderr, "tsdb: allocation failed\n");
		exit(1);
	}

	/* Populate index with synthetic time-series summaries */
	printf("  populating %zu MB index...\n", ts->index_sz / (1024*1024));
	for (size_t i = 0; i < ts->index_sz; i += sizeof(uint64_t))
		*(uint64_t *)(ts->index + i) = xorshift64();
}

static double tsdb_run_round(struct tsdb_state *ts, int ops_per_round)
{
	volatile uint64_t sink = 0;
	double t0 = now_ms();

	for (int i = 0; i < ops_per_round; i++) {
		uint64_t r = xorshift64();

		/* 60% ingestion: sequential write to ring buffer */
		if ((r & 0xF) < 10) {
			*(volatile uint64_t *)(ts->ring + ts->ring_pos) = r;
			ts->ring_pos = (ts->ring_pos + CACHE_LINE) % ts->ring_sz;
		} else {
			/* 40% query: random read scan from index */
			size_t base = (xorshift64() % (ts->index_sz / PAGE_SIZE)) * PAGE_SIZE;
			/* Scan a 4 KB "block" (one page) for aggregation */
			for (size_t off = 0; off < PAGE_SIZE; off += CACHE_LINE)
				sink += *(volatile uint64_t *)(ts->index + base + off);
		}
	}

	(void)sink;
	return now_ms() - t0;
}

static void tsdb_fini(struct tsdb_state *ts)
{
	bench_free(ts->ring);
	bench_free(ts->index);
}

/* =========================================================================
 * Workload 4: Phase Change (bulk load → query serving)
 *
 * Single region: first half of rounds are write-heavy (bulk import),
 * second half are read-heavy (OLAP queries). Tests re-migration.
 *
 * Expected: pages start on CXL (writes), migrate to DDR (reads)
 * ========================================================================= */

struct phase_state {
	uint8_t *data;
	size_t   data_sz;
};

static void phase_init(struct phase_state *ph, size_t total_mb)
{
	ph->data_sz = total_mb * 1024UL * 1024;
	ph->data = bench_alloc(ph->data_sz);
	if (!ph->data) {
		fprintf(stderr, "phase: allocation failed\n");
		exit(1);
	}
	printf("  allocated %zu MB region\n", total_mb);
}

static double phase_run_round(struct phase_state *ph, int round, int total_rounds)
{
	double t0 = now_ms();
	int is_write_phase = (round < total_rounds / 2);

	if (is_write_phase) {
		/* Bulk load: random writes across entire region */
		size_t npages = ph->data_sz / PAGE_SIZE;
		for (size_t i = 0; i < npages; i++) {
			size_t page = (xorshift64() % npages) * PAGE_SIZE;
			*(volatile uint64_t *)(ph->data + page) = xorshift64();
		}
	} else {
		/* Query serving: random reads across entire region */
		volatile uint64_t sink = 0;
		size_t npages = ph->data_sz / PAGE_SIZE;
		for (size_t i = 0; i < npages; i++) {
			size_t page = (xorshift64() % npages) * PAGE_SIZE;
			sink += *(volatile uint64_t *)(ph->data + page);
		}
		(void)sink;
	}

	return now_ms() - t0;
}

static void phase_fini(struct phase_state *ph)
{
	bench_free(ph->data);
}

/* =========================================================================
 * Main: workload dispatcher
 * ========================================================================= */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s --workload=WORKLOAD [OPTIONS]\n"
		"\n"
		"Workloads:\n"
		"  kv       Key-value store (hash table + WAL)\n"
		"  graph    Graph traversal (PageRank-like)\n"
		"  tsdb     Time-series ingestion + query\n"
		"  phase    Phase change (write → read transition)\n"
		"\n"
		"Options:\n"
		"  --no-cxlmm       Use plain malloc (control mode)\n"
		"  --rounds=N        Number of rounds (default: %d)\n"
		"  --size-mb=N       Total working set in MB (default: %d)\n"
		"  --ops=N           Operations per round for kv/tsdb (default: 2000000)\n"
		"  --hold=N          Seconds to hold alive after benchmark (default: 10)\n"
		"  --csv             Output results in CSV format\n"
		"  --help\n",
		prog, DEFAULT_ROUNDS, DEFAULT_SIZE_MB);
}

int main(int argc, char **argv)
{
	const char *workload = NULL;
	int rounds = DEFAULT_ROUNDS;
	int size_mb = DEFAULT_SIZE_MB;
	int ops_per_round = 2000000;
	int hold_sec = 10;
	int csv_output = 0;

	static struct option long_opts[] = {
		{"workload",  required_argument, NULL, 'w'},
		{"no-cxlmm",  no_argument,      NULL, 'n'},
		{"rounds",    required_argument, NULL, 'r'},
		{"size-mb",   required_argument, NULL, 's'},
		{"ops",       required_argument, NULL, 'o'},
		{"hold",      required_argument, NULL, 'H'},
		{"csv",       no_argument,       NULL, 'c'},
		{"help",      no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "w:nr:s:o:H:ch", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'w': workload = optarg; break;
		case 'n': use_cxlmm = 0; break;
		case 'r': rounds = atoi(optarg); break;
		case 's': size_mb = atoi(optarg); break;
		case 'o': ops_per_round = atoi(optarg); break;
		case 'H': hold_sec = atoi(optarg); break;
		case 'c': csv_output = 1; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	if (!workload) {
		fprintf(stderr, "error: --workload is required\n");
		usage(argv[0]);
		return 1;
	}

	/* Init library */
	if (use_cxlmm) {
		if (cxlmm_init() < 0)
			fprintf(stderr, "warn: cxlmm_init failed; running in passthrough mode\n");
	}

	printf("cxlmm_bench: workload=%s  mode=%s  rounds=%d  size=%d MB\n",
	       workload, use_cxlmm ? "cxlmm" : "malloc", rounds, size_mb);

	if (csv_output)
		printf("round,time_ms,workload,mode\n");
	else {
		printf("%-6s  %12s\n", "round", "time(ms)");
		printf("------  ------------\n");
	}

	/* --- Dispatch to workload --- */

	if (strcmp(workload, "kv") == 0) {
		struct kv_state kv;
		kv_init(&kv, size_mb);
		for (int r = 0; r < rounds; r++) {
			double t = kv_run_round(&kv, ops_per_round);
			if (csv_output)
				printf("%d,%.1f,%s,%s\n", r+1, t, workload,
				       use_cxlmm ? "cxlmm" : "malloc");
			else
				printf("%-6d  %12.1f\n", r+1, t);
			fflush(stdout);
			usleep(200 * 1000);
		}
		printf("\n[holding %d s for daemon cycles]\n", hold_sec);
		fflush(stdout);
		sleep(hold_sec);
		kv_fini(&kv);

	} else if (strcmp(workload, "graph") == 0) {
		struct graph_state g;
		graph_init(&g, size_mb);
		for (int r = 0; r < rounds; r++) {
			double t = graph_run_round(&g);
			if (csv_output)
				printf("%d,%.1f,%s,%s\n", r+1, t, workload,
				       use_cxlmm ? "cxlmm" : "malloc");
			else
				printf("%-6d  %12.1f\n", r+1, t);
			fflush(stdout);
			usleep(200 * 1000);
		}
		printf("\n[holding %d s for daemon cycles]\n", hold_sec);
		fflush(stdout);
		sleep(hold_sec);
		graph_fini(&g);

	} else if (strcmp(workload, "tsdb") == 0) {
		struct tsdb_state ts;
		tsdb_init(&ts, size_mb);
		for (int r = 0; r < rounds; r++) {
			double t = tsdb_run_round(&ts, ops_per_round);
			if (csv_output)
				printf("%d,%.1f,%s,%s\n", r+1, t, workload,
				       use_cxlmm ? "cxlmm" : "malloc");
			else
				printf("%-6d  %12.1f\n", r+1, t);
			fflush(stdout);
			usleep(200 * 1000);
		}
		printf("\n[holding %d s for daemon cycles]\n", hold_sec);
		fflush(stdout);
		sleep(hold_sec);
		tsdb_fini(&ts);

	} else if (strcmp(workload, "phase") == 0) {
		struct phase_state ph;
		phase_init(&ph, size_mb);
		int half = rounds / 2;
		for (int r = 0; r < rounds; r++) {
			double t = phase_run_round(&ph, r, rounds);
			const char *phase_name = (r < half) ? "write" : "read";
			if (csv_output)
				printf("%d,%.1f,%s_%s,%s\n", r+1, t, workload,
				       phase_name, use_cxlmm ? "cxlmm" : "malloc");
			else
				printf("%-6d  %12.1f  [%s phase]\n", r+1, t, phase_name);
			fflush(stdout);
			usleep(200 * 1000);
		}
		printf("\n[holding %d s for daemon cycles]\n", hold_sec);
		fflush(stdout);
		sleep(hold_sec);
		phase_fini(&ph);

	} else {
		fprintf(stderr, "error: unknown workload '%s'\n", workload);
		usage(argv[0]);
		return 1;
	}

	if (use_cxlmm)
		cxlmm_fini();

	return 0;
}
