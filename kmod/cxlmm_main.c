// SPDX-License-Identifier: GPL-2.0
/*
 * cxlmm_main.c — module init/exit, misc device, score ring helpers
 *
 * Initialization order:
 *   1. Validate NUMA nodes
 *   2. Allocate & zero global state
 *   3. Init score ring
 *   4. cxlmm_bw_monitor_init()   (non-fatal on failure)
 *   5. cxlmm_procfs_init()
 *   6. misc_register()           → /dev/cxlmm
 *   7. cxlmm_scanner_init()      → kthread starts
 *
 * Teardown is the exact reverse.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/numa.h>
#include <linux/nodemask.h>

#include "cxlmm.h"

/* --------------------------------------------------------------------------
 * Module parameters
 * -------------------------------------------------------------------------- */

static int cxl_node = -1;
module_param(cxl_node, int, 0444);
MODULE_PARM_DESC(cxl_node, "NUMA node id of CXL memory (required)");

static int ddr_node;
module_param(ddr_node, int, 0444);
MODULE_PARM_DESC(ddr_node, "NUMA node id of local DDR5 (default: 0)");

static int scan_interval_ms = CXLMM_DEFAULT_SCAN_MS;
module_param(scan_interval_ms, int, 0644);
MODULE_PARM_DESC(scan_interval_ms, "Scanner wakeup interval in ms (default: 1000)");

static int write_threshold_pct = CXLMM_DEFAULT_WRITE_THRESH_PCT;
module_param(write_threshold_pct, int, 0644);
MODULE_PARM_DESC(write_threshold_pct, "Write %% above which page → CXL (default: 60)");

/* --------------------------------------------------------------------------
 * Global state
 * -------------------------------------------------------------------------- */

struct cxlmm_state *g_cxlmm;
EXPORT_SYMBOL_GPL(g_cxlmm);

/* --------------------------------------------------------------------------
 * Score ring helpers
 * -------------------------------------------------------------------------- */

int cxlmm_ring_push(struct cxlmm_score_ring *ring,
		    const struct cxlmm_page_score *score)
{
	unsigned long flags;
	unsigned int next_tail;
	int ret = 0;

	spin_lock_irqsave(&ring->lock, flags);

	next_tail = (ring->tail + 1) % ring->capacity;
	if (next_tail == ring->head) {
		/* ring full */
		ring->dropped++;
		ret = -ENOSPC;
	} else {
		ring->buf[ring->tail] = *score;
		ring->tail = next_tail;
	}

	spin_unlock_irqrestore(&ring->lock, flags);
	return ret;
}

unsigned int cxlmm_ring_drain(struct cxlmm_score_ring *ring,
			      struct cxlmm_page_score *out,
			      unsigned int max)
{
	unsigned long flags;
	unsigned int count = 0;

	spin_lock_irqsave(&ring->lock, flags);

	while (count < max && ring->head != ring->tail) {
		out[count++] = ring->buf[ring->head];
		ring->head = (ring->head + 1) % ring->capacity;
	}

	spin_unlock_irqrestore(&ring->lock, flags);
	return count;
}

/* --------------------------------------------------------------------------
 * File operations for /dev/cxlmm
 * -------------------------------------------------------------------------- */

static int cxlmm_open(struct inode *inode, struct file *filp)
{
	/* No per-fd state needed; just allow access */
	return 0;
}

static int cxlmm_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations cxlmm_fops = {
	.owner          = THIS_MODULE,
	.open           = cxlmm_open,
	.release        = cxlmm_release,
	.unlocked_ioctl = cxlmm_ioctl,
};

/* --------------------------------------------------------------------------
 * Config defaults
 * -------------------------------------------------------------------------- */

static void cxlmm_config_defaults(struct cxlmm_config *cfg, int cxl_nid, int ddr_nid)
{
	cfg->scan_interval_ms     = (u32)scan_interval_ms;
	cfg->write_threshold_pct  = (u32)write_threshold_pct;
	cfg->min_scans            = CXLMM_DEFAULT_MIN_SCANS;
	cfg->cxl_node_id          = cxl_nid;
	cfg->ddr_node_id          = ddr_nid;
	cfg->migration_batch_size = CXLMM_DEFAULT_BATCH_SIZE;
	cfg->max_tracked_pids     = CXLMM_DEFAULT_MAX_PIDS;
	cfg->score_ring_size      = CXLMM_SCORE_RING_SIZE;
	cfg->flags                = 0;
	cfg->_pad                 = 0;
}

/* --------------------------------------------------------------------------
 * Module init
 * -------------------------------------------------------------------------- */

static int __init cxlmm_init(void)
{
	struct cxlmm_state *st;
	int ret;

	/* Validate cxl_node parameter */
	if (cxl_node < 0) {
		pr_err("cxlmm: cxl_node parameter is required (e.g. cxl_node=2)\n");
		return -EINVAL;
	}
	if (!node_online(cxl_node)) {
		pr_err("cxlmm: cxl_node=%d is not online\n", cxl_node);
		return -EINVAL;
	}
	if (!node_online(ddr_node)) {
		pr_err("cxlmm: ddr_node=%d is not online\n", ddr_node);
		return -EINVAL;
	}
	if (cxl_node == ddr_node) {
		pr_err("cxlmm: cxl_node and ddr_node must be different\n");
		return -EINVAL;
	}
	if (write_threshold_pct < 1 || write_threshold_pct > 99) {
		pr_err("cxlmm: write_threshold_pct must be 1-99\n");
		return -EINVAL;
	}

	/* Allocate global state */
	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->cxl_node = cxl_node;
	st->ddr_node = ddr_node;

	/* Locks */
	spin_lock_init(&st->config_lock);
	rwlock_init(&st->pid_lock);
	spin_lock_init(&st->bw_lock);
	INIT_LIST_HEAD(&st->tracked_pid_list);
	atomic_set(&st->scan_generation, 0);

	/* Default configuration */
	cxlmm_config_defaults(&st->config, cxl_node, ddr_node);

	/* Score ring */
	spin_lock_init(&st->score_ring.lock);
	st->score_ring.capacity = CXLMM_SCORE_RING_SIZE;
	st->score_ring.buf = kcalloc(CXLMM_SCORE_RING_SIZE,
				     sizeof(struct cxlmm_page_score),
				     GFP_KERNEL);
	if (!st->score_ring.buf) {
		ret = -ENOMEM;
		goto err_free_state;
	}

	/* Misc device */
	st->misc_dev.minor = MISC_DYNAMIC_MINOR;
	st->misc_dev.name  = CXLMM_DEVICE_NAME;
	st->misc_dev.fops  = &cxlmm_fops;
	st->misc_dev.mode  = 0600;

	/* BW monitor (non-fatal) */
	ret = cxlmm_bw_monitor_init(st);
	if (ret)
		pr_warn("cxlmm: BW monitoring unavailable (ret=%d); continuing\n", ret);

	/* Procfs */
	ret = cxlmm_procfs_init(st);
	if (ret) {
		pr_err("cxlmm: procfs init failed: %d\n", ret);
		goto err_bw_exit;
	}

	/* Register misc device */
	ret = misc_register(&st->misc_dev);
	if (ret) {
		pr_err("cxlmm: misc_register failed: %d\n", ret);
		goto err_procfs_exit;
	}

	/* Publish global state pointer BEFORE starting the kthread */
	g_cxlmm = st;

	/* Scanner kthread */
	ret = cxlmm_scanner_init(st);
	if (ret) {
		pr_err("cxlmm: scanner init failed: %d\n", ret);
		goto err_misc_deregister;
	}

	pr_info("cxlmm: loaded — cxl_node=%d ddr_node=%d scan_ms=%d write_thr=%d%%\n",
		cxl_node, ddr_node, scan_interval_ms, write_threshold_pct);
	return 0;

err_misc_deregister:
	g_cxlmm = NULL;
	misc_deregister(&st->misc_dev);
err_procfs_exit:
	cxlmm_procfs_exit(st);
err_bw_exit:
	cxlmm_bw_monitor_exit(st);
	kfree(st->score_ring.buf);
err_free_state:
	kfree(st);
	return ret;
}

/* --------------------------------------------------------------------------
 * Module exit
 * -------------------------------------------------------------------------- */

static void __exit cxlmm_exit(void)
{
	struct cxlmm_state *st = g_cxlmm;

	if (!st)
		return;

	/* Stop scanner first so no new scores are pushed */
	cxlmm_scanner_exit(st);

	/* Deregister device and procfs */
	misc_deregister(&st->misc_dev);
	cxlmm_procfs_exit(st);

	/* Release IMC perf events */
	cxlmm_bw_monitor_exit(st);

	/* Free tracked PID list */
	{
		struct cxlmm_tracked_pid *entry, *tmp;

		write_lock(&st->pid_lock);
		list_for_each_entry_safe(entry, tmp, &st->tracked_pid_list, list) {
			list_del(&entry->list);
			kfree(entry);
		}
		write_unlock(&st->pid_lock);
	}

	/* Clear global pointer before freeing */
	g_cxlmm = NULL;
	kfree(st->score_ring.buf);
	kfree(st);

	pr_info("cxlmm: unloaded\n");
}

module_init(cxlmm_init);
module_exit(cxlmm_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("CXL Memory Manager Project");
MODULE_DESCRIPTION("CXL-aware bandwidth-optimized page placement");
MODULE_VERSION("0.1");
