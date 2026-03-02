// SPDX-License-Identifier: GPL-2.0
/*
 * cxlmm_ioctl.c — ioctl dispatch and per-command handlers
 *
 * Commands:
 *   CXLMM_IOC_TRACK         add a PID to the scanner watch list
 *   CXLMM_IOC_UNTRACK       remove a PID from the watch list
 *   CXLMM_IOC_GET_BW        read latest IMC bandwidth snapshot
 *   CXLMM_IOC_GET_CFG       read current module configuration
 *   CXLMM_IOC_SET_CFG       atomically replace module configuration
 *   CXLMM_IOC_FETCH_SCORES  drain score ring into userspace buffer
 *
 * All handlers validate their arguments before touching shared state.
 * copy_from_user / copy_to_user errors return -EFAULT immediately.
 */

#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/nodemask.h>

#include "cxlmm.h"

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static struct cxlmm_tracked_pid *find_tracked(struct cxlmm_state *st, pid_t pid)
{
	struct cxlmm_tracked_pid *tp;

	list_for_each_entry(tp, &st->tracked_pid_list, list) {
		if (tp->pid == pid)
			return tp;
	}
	return NULL;
}

/* --------------------------------------------------------------------------
 * CXLMM_IOC_TRACK
 * -------------------------------------------------------------------------- */

static long ioctl_track(struct cxlmm_state *st, unsigned long arg)
{
	struct cxlmm_track_req req;
	struct cxlmm_tracked_pid *tp;
	unsigned int max_pids;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.pid == 0)
		return -EINVAL;
	if (req.flags != 0)
		return -EINVAL;

	/* Verify the PID exists in the kernel */
	rcu_read_lock();
	if (!find_task_by_vpid(req.pid)) {
		rcu_read_unlock();
		return -ESRCH;
	}
	rcu_read_unlock();

	write_lock(&st->pid_lock);

	/* Read max_tracked_pids under config_lock, then release */
	spin_lock(&st->config_lock);
	max_pids = st->config.max_tracked_pids;
	spin_unlock(&st->config_lock);

	if (st->tracked_pid_count >= max_pids) {
		write_unlock(&st->pid_lock);
		return -ENOSPC;
	}

	if (find_tracked(st, req.pid)) {
		write_unlock(&st->pid_lock);
		return -EEXIST;
	}

	tp = kzalloc(sizeof(*tp), GFP_ATOMIC);
	if (!tp) {
		write_unlock(&st->pid_lock);
		return -ENOMEM;
	}

	tp->pid   = req.pid;
	tp->flags = 0;
	tp->last_scan_jiffies = 0;
	INIT_LIST_HEAD(&tp->list);
	list_add_tail(&tp->list, &st->tracked_pid_list);
	st->tracked_pid_count++;

	write_unlock(&st->pid_lock);

	pr_info("cxlmm: tracking pid %u\n", req.pid);
	return 0;
}

/* --------------------------------------------------------------------------
 * CXLMM_IOC_UNTRACK
 * -------------------------------------------------------------------------- */

static long ioctl_untrack(struct cxlmm_state *st, unsigned long arg)
{
	__u32 pid;
	struct cxlmm_tracked_pid *tp;

	if (copy_from_user(&pid, (void __user *)arg, sizeof(pid)))
		return -EFAULT;

	if (pid == 0)
		return -EINVAL;

	write_lock(&st->pid_lock);

	tp = find_tracked(st, pid);
	if (!tp) {
		write_unlock(&st->pid_lock);
		return -ENOENT;
	}

	list_del(&tp->list);
	st->tracked_pid_count--;
	write_unlock(&st->pid_lock);

	kfree(tp);
	pr_info("cxlmm: untracked pid %u\n", pid);
	return 0;
}

/* --------------------------------------------------------------------------
 * CXLMM_IOC_GET_BW
 * -------------------------------------------------------------------------- */

static long ioctl_get_bw(struct cxlmm_state *st, unsigned long arg)
{
	struct cxlmm_bw_stats snap;
	unsigned long flags;

	spin_lock_irqsave(&st->bw_lock, flags);
	snap = st->bw_snapshot;
	spin_unlock_irqrestore(&st->bw_lock, flags);

	if (copy_to_user((void __user *)arg, &snap, sizeof(snap)))
		return -EFAULT;

	return 0;
}

/* --------------------------------------------------------------------------
 * CXLMM_IOC_GET_CFG
 * -------------------------------------------------------------------------- */

static long ioctl_get_cfg(struct cxlmm_state *st, unsigned long arg)
{
	struct cxlmm_config cfg;

	spin_lock(&st->config_lock);
	cfg = st->config;
	spin_unlock(&st->config_lock);

	if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)))
		return -EFAULT;

	return 0;
}

/* --------------------------------------------------------------------------
 * CXLMM_IOC_SET_CFG
 * -------------------------------------------------------------------------- */

static long ioctl_set_cfg(struct cxlmm_state *st, unsigned long arg)
{
	struct cxlmm_config new_cfg;

	if (copy_from_user(&new_cfg, (void __user *)arg, sizeof(new_cfg)))
		return -EFAULT;

	/* Validate fields */
	if (new_cfg.scan_interval_ms < 10 || new_cfg.scan_interval_ms > 60000)
		return -EINVAL;
	if (new_cfg.write_threshold_pct < 1 || new_cfg.write_threshold_pct > 99)
		return -EINVAL;
	if (new_cfg.min_scans < 1 || new_cfg.min_scans > 100)
		return -EINVAL;
	if (new_cfg.migration_batch_size < 1 || new_cfg.migration_batch_size > 4096)
		return -EINVAL;
	if (new_cfg.max_tracked_pids < 1 || new_cfg.max_tracked_pids > 1024)
		return -EINVAL;
	if (!node_online(new_cfg.cxl_node_id) || !node_online(new_cfg.ddr_node_id))
		return -EINVAL;
	if (new_cfg.cxl_node_id == new_cfg.ddr_node_id)
		return -EINVAL;

	/* Ensure reserved field is zero */
	new_cfg._pad = 0;
	/* score_ring_size is immutable after init; ignore the userspace value */
	new_cfg.score_ring_size = CXLMM_SCORE_RING_SIZE;

	spin_lock(&st->config_lock);
	st->config = new_cfg;   /* atomic replacement (struct copy under lock) */
	spin_unlock(&st->config_lock);

	pr_info("cxlmm: config updated: scan_ms=%u write_thr=%u%% cxl=%d ddr=%d\n",
		new_cfg.scan_interval_ms, new_cfg.write_threshold_pct,
		new_cfg.cxl_node_id, new_cfg.ddr_node_id);
	return 0;
}

/* --------------------------------------------------------------------------
 * CXLMM_IOC_FETCH_SCORES
 * -------------------------------------------------------------------------- */

#define FETCH_CHUNK 256   /* pages per kzalloc chunk to bound kernel memory */

static long ioctl_fetch_scores(struct cxlmm_state *st, unsigned long arg)
{
	struct cxlmm_score_fetch req;
	struct cxlmm_page_score *kbuf;
	unsigned int to_fetch, fetched;
	long ret = 0;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	/* Sanity: cap at a reasonable number to avoid huge kernel allocs */
	if (req.count == 0)
		return 0;
	to_fetch = min_t(unsigned int, req.count, CXLMM_SCORE_RING_SIZE);

	kbuf = kcalloc(to_fetch, sizeof(*kbuf), GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	fetched = cxlmm_ring_drain(&st->score_ring, kbuf, to_fetch);

	if (fetched > 0) {
		void __user *ubuf = (void __user *)(uintptr_t)req.buf_ptr;

		if (!access_ok(ubuf, fetched * sizeof(*kbuf))) {
			ret = -EFAULT;
			goto out;
		}
		if (copy_to_user(ubuf, kbuf, fetched * sizeof(*kbuf))) {
			ret = -EFAULT;
			goto out;
		}
	}

	req.filled = fetched;
	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		ret = -EFAULT;
		goto out;
	}

out:
	kfree(kbuf);
	return ret;
}

/* --------------------------------------------------------------------------
 * Main ioctl dispatcher
 * -------------------------------------------------------------------------- */

long cxlmm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct cxlmm_state *st = g_cxlmm;

	if (!st)
		return -ENODEV;

	if (_IOC_TYPE(cmd) != CXLMM_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > CXLMM_IOC_MAXNR)
		return -ENOTTY;

	/* Check read/write access for each direction */
	if (_IOC_DIR(cmd) & _IOC_READ) {
		if (!access_ok((void __user *)arg, _IOC_SIZE(cmd)))
			return -EFAULT;
	}
	if (_IOC_DIR(cmd) & _IOC_WRITE) {
		if (!access_ok((void __user *)arg, _IOC_SIZE(cmd)))
			return -EFAULT;
	}

	switch (cmd) {
	case CXLMM_IOC_TRACK:
		return ioctl_track(st, arg);
	case CXLMM_IOC_UNTRACK:
		return ioctl_untrack(st, arg);
	case CXLMM_IOC_GET_BW:
		return ioctl_get_bw(st, arg);
	case CXLMM_IOC_GET_CFG:
		return ioctl_get_cfg(st, arg);
	case CXLMM_IOC_SET_CFG:
		return ioctl_set_cfg(st, arg);
	case CXLMM_IOC_FETCH_SCORES:
		return ioctl_fetch_scores(st, arg);
	default:
		return -ENOTTY;
	}
}
