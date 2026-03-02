// SPDX-License-Identifier: GPL-2.0
/*
 * cxlmm_procfs.c — /proc/cxlmm/{stats,config,pids} seq_file interface
 *
 * /proc/cxlmm/stats   — bandwidth snapshot from IMC counters
 * /proc/cxlmm/config  — current module configuration
 * /proc/cxlmm/pids    — list of currently tracked PIDs
 */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "cxlmm.h"

/* --------------------------------------------------------------------------
 * /proc/cxlmm/stats
 * -------------------------------------------------------------------------- */

static int stats_show(struct seq_file *m, void *v)
{
	struct cxlmm_state *st = g_cxlmm;
	struct cxlmm_bw_stats snap;
	unsigned long flags;

	if (!st) {
		seq_puts(m, "module not loaded\n");
		return 0;
	}

	spin_lock_irqsave(&st->bw_lock, flags);
	snap = st->bw_snapshot;
	spin_unlock_irqrestore(&st->bw_lock, flags);

	seq_puts(m, "# CXL Memory Manager — Bandwidth Statistics\n");
	seq_printf(m, "# timestamp_ns       %llu\n", snap.timestamp_ns);
	seq_printf(m, "ddr_read_mb          %llu\n", snap.ddr_read_mb);
	seq_printf(m, "ddr_write_mb         %llu\n", snap.ddr_write_mb);
	seq_printf(m, "cxl_read_mb          %llu\n", snap.cxl_read_mb);
	seq_printf(m, "cxl_write_mb         %llu\n", snap.cxl_write_mb);
	seq_printf(m, "ddr_read_mb_s        %llu\n", snap.ddr_read_mb_s);
	seq_printf(m, "ddr_write_mb_s       %llu\n", snap.ddr_write_mb_s);
	seq_printf(m, "cxl_read_mb_s        %llu\n", snap.cxl_read_mb_s);
	seq_printf(m, "cxl_write_mb_s       %llu\n", snap.cxl_write_mb_s);

	/* Ring buffer statistics */
	{
		unsigned long dropped;
		unsigned int used;
		spin_lock_irqsave(&st->score_ring.lock, flags);
		dropped = st->score_ring.dropped;
		used = (st->score_ring.tail - st->score_ring.head
			+ st->score_ring.capacity) % st->score_ring.capacity;
		spin_unlock_irqrestore(&st->score_ring.lock, flags);

		seq_printf(m, "score_ring_used      %u\n", used);
		seq_printf(m, "score_ring_capacity  %u\n", st->score_ring.capacity);
		seq_printf(m, "score_ring_dropped   %lu\n", dropped);
	}

	seq_printf(m, "scan_generation      %d\n",
		   atomic_read(&st->scan_generation));
	seq_printf(m, "imc_boxes            %u\n", st->imc_box_count);

	return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, stats_show, NULL);
}

static const struct proc_ops stats_fops = {
	.proc_open    = stats_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* --------------------------------------------------------------------------
 * /proc/cxlmm/config
 * -------------------------------------------------------------------------- */

static int config_show(struct seq_file *m, void *v)
{
	struct cxlmm_state *st = g_cxlmm;
	struct cxlmm_config cfg;

	if (!st) {
		seq_puts(m, "module not loaded\n");
		return 0;
	}

	spin_lock(&st->config_lock);
	cfg = st->config;
	spin_unlock(&st->config_lock);

	seq_puts(m, "# CXL Memory Manager — Configuration\n");
	seq_printf(m, "scan_interval_ms     %u\n", cfg.scan_interval_ms);
	seq_printf(m, "write_threshold_pct  %u\n", cfg.write_threshold_pct);
	seq_printf(m, "min_scans            %u\n", cfg.min_scans);
	seq_printf(m, "cxl_node_id          %d\n", cfg.cxl_node_id);
	seq_printf(m, "ddr_node_id          %d\n", cfg.ddr_node_id);
	seq_printf(m, "migration_batch_size %u\n", cfg.migration_batch_size);
	seq_printf(m, "max_tracked_pids     %u\n", cfg.max_tracked_pids);
	seq_printf(m, "score_ring_size      %u\n", cfg.score_ring_size);
	seq_printf(m, "flags                0x%08x\n", cfg.flags);

	return 0;
}

static int config_open(struct inode *inode, struct file *file)
{
	return single_open(file, config_show, NULL);
}

static const struct proc_ops config_fops = {
	.proc_open    = config_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* --------------------------------------------------------------------------
 * /proc/cxlmm/pids
 * -------------------------------------------------------------------------- */

static int pids_show(struct seq_file *m, void *v)
{
	struct cxlmm_state *st = g_cxlmm;
	struct cxlmm_tracked_pid *tp;

	if (!st) {
		seq_puts(m, "module not loaded\n");
		return 0;
	}

	seq_puts(m, "# CXL Memory Manager — Tracked PIDs\n");
	seq_puts(m, "# pid    last_scan_jiffies\n");

	read_lock(&st->pid_lock);
	list_for_each_entry(tp, &st->tracked_pid_list, list) {
		seq_printf(m, "%-8u %lu\n", tp->pid, tp->last_scan_jiffies);
	}
	read_unlock(&st->pid_lock);

	return 0;
}

static int pids_open(struct inode *inode, struct file *file)
{
	return single_open(file, pids_show, NULL);
}

static const struct proc_ops pids_fops = {
	.proc_open    = pids_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* --------------------------------------------------------------------------
 * Public: init / exit
 * -------------------------------------------------------------------------- */

int cxlmm_procfs_init(struct cxlmm_state *st)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir(CXLMM_PROC_DIR, NULL);
	if (!dir) {
		pr_err("cxlmm: failed to create /proc/%s\n", CXLMM_PROC_DIR);
		return -ENOMEM;
	}

	if (!proc_create("stats",  0444, dir, &stats_fops)  ||
	    !proc_create("config", 0444, dir, &config_fops) ||
	    !proc_create("pids",   0444, dir, &pids_fops)) {
		pr_err("cxlmm: failed to create /proc/%s entries\n", CXLMM_PROC_DIR);
		remove_proc_subtree(CXLMM_PROC_DIR, NULL);
		return -ENOMEM;
	}

	st->proc_dir = dir;
	pr_info("cxlmm: /proc/%s created\n", CXLMM_PROC_DIR);
	return 0;
}

void cxlmm_procfs_exit(struct cxlmm_state *st)
{
	if (st->proc_dir) {
		remove_proc_subtree(CXLMM_PROC_DIR, NULL);
		st->proc_dir = NULL;
		pr_info("cxlmm: /proc/%s removed\n", CXLMM_PROC_DIR);
	}
}
