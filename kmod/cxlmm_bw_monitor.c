// SPDX-License-Identifier: GPL-2.0
/*
 * cxlmm_bw_monitor.c — Intel IMC CAS counter monitoring via perf_event
 *
 * Reads SPR (Sapphire Rapids) IMC uncore events:
 *   cas_count_read:  event=0x05, umask=0xcf
 *   cas_count_write: event=0x05, umask=0xf0
 *
 * One CAS = 64 bytes.  Scale to MiB: count × 64 / (1024×1024).
 * Source: arch/x86/events/intel/uncore_snbep.c:6053
 *
 * perf_event_create_kernel_counter is EXPORT_SYMBOL_GPL.
 * We create one read + one write event per online CPU that has an IMC,
 * up to CXLMM_MAX_IMC_BOXES.  On non-SPR hardware the events will fail
 * gracefully with ENODEV/EOPNOTSUPP — init returns non-zero (non-fatal).
 *
 * cxlmm_bw_monitor_sample() is called from the scanner kthread each wakeup.
 */

#include <linux/perf_event.h>
#include <linux/cpumask.h>
#include <linux/ktime.h>
#include <linux/errno.h>

#include "cxlmm.h"

/* Bytes per CAS transaction (one 64-byte cache line) */
#define CAS_BYTES  64ULL
/* Divisor to convert bytes → MiB */
#define BYTES_PER_MIB (1024ULL * 1024ULL)

/* --------------------------------------------------------------------------
 * Build a perf_event_attr for an IMC uncore event
 *
 * Uncore events use PERF_TYPE_RAW with the event/umask packed into config.
 * On SPR the PMU type for imc_* is discovered at runtime; here we use
 * RAW which relies on the kernel's uncore infrastructure routing.
 * -------------------------------------------------------------------------- */

static struct perf_event *
create_imc_event(int cpu, u8 event_code, u8 umask)
{
	struct perf_event_attr attr = {};
	struct perf_event *ev;

	attr.type           = PERF_TYPE_RAW;
	attr.size           = sizeof(attr);
	attr.config         = ((u64)umask << 8) | event_code;
	attr.sample_period  = 0;      /* counting mode */
	attr.disabled       = 0;      /* start immediately */
	attr.pinned         = 1;      /* require this to stay on HW */
	attr.exclude_user   = 0;
	attr.exclude_kernel = 0;

	ev = perf_event_create_kernel_counter(&attr, cpu, NULL, NULL, NULL);
	if (IS_ERR(ev))
		return NULL;
	return ev;
}

/* --------------------------------------------------------------------------
 * Public: init
 *
 * Try to open IMC events on each online CPU, up to CXLMM_MAX_IMC_BOXES.
 * Failure on any single CPU is non-fatal — we skip it.
 * Returns 0 if at least one box was opened, -ENODEV otherwise.
 * -------------------------------------------------------------------------- */

int cxlmm_bw_monitor_init(struct cxlmm_state *st)
{
	int cpu;
	unsigned int boxes = 0;

	for_each_online_cpu(cpu) {
		struct cxlmm_imc_box *box;

		if (boxes >= CXLMM_MAX_IMC_BOXES)
			break;

		box = &st->imc_boxes[boxes];
		box->cpu = cpu;

		box->read_event  = create_imc_event(cpu,
						     CXLMM_IMC_EVENT_CODE,
						     CXLMM_IMC_UMASK_READ);
		box->write_event = create_imc_event(cpu,
						     CXLMM_IMC_EVENT_CODE,
						     CXLMM_IMC_UMASK_WRITE);

		if (!box->read_event && !box->write_event) {
			/* Neither event opened on this CPU; skip */
			continue;
		}

		boxes++;
	}

	st->imc_box_count = boxes;

	if (boxes == 0) {
		pr_warn("cxlmm: no IMC perf events opened; BW stats will be zero\n");
		return -ENODEV;
	}

	pr_info("cxlmm: opened IMC perf events on %u CPU(s)\n", boxes);
	return 0;
}

/* --------------------------------------------------------------------------
 * Public: exit — release all perf events
 * -------------------------------------------------------------------------- */

void cxlmm_bw_monitor_exit(struct cxlmm_state *st)
{
	unsigned int i;

	for (i = 0; i < st->imc_box_count; i++) {
		struct cxlmm_imc_box *box = &st->imc_boxes[i];

		if (box->read_event) {
			perf_event_release_kernel(box->read_event);
			box->read_event = NULL;
		}
		if (box->write_event) {
			perf_event_release_kernel(box->write_event);
			box->write_event = NULL;
		}
	}
	st->imc_box_count = 0;
}

/* --------------------------------------------------------------------------
 * Public: sample — read current counters and update bw_snapshot
 *
 * Called from the scanner kthread (not interrupt context).
 * Accumulates raw CAS counts, converts to MiB, computes per-interval rates.
 * -------------------------------------------------------------------------- */

void cxlmm_bw_monitor_sample(struct cxlmm_state *st)
{
	unsigned long flags;
	unsigned int i;
	u64 total_read_cas  = 0;
	u64 total_write_cas = 0;
	u64 now_ns;
	struct cxlmm_bw_stats snap;
	struct cxlmm_bw_stats prev;

	if (st->imc_box_count == 0)
		return;

	/* Sum CAS counts across all IMC boxes */
	for (i = 0; i < st->imc_box_count; i++) {
		struct cxlmm_imc_box *box = &st->imc_boxes[i];
		u64 ev_count;
		u64 ev_enabled, ev_running;

		if (box->read_event) {
			ev_count = perf_event_read_value(box->read_event,
							 &ev_enabled,
							 &ev_running);
			if (ev_running > 0)
				total_read_cas += ev_count;
		}
		if (box->write_event) {
			ev_count = perf_event_read_value(box->write_event,
							 &ev_enabled,
							 &ev_running);
			if (ev_running > 0)
				total_write_cas += ev_count;
		}
	}

	now_ns = ktime_get_ns();

	/*
	 * Convert CAS counts to MiB.
	 * We treat all DDR traffic here; CXL traffic would come from separate
	 * cxlcm/cxldp events (which require MMIO-based uncore setup beyond
	 * PERF_TYPE_RAW). For now, CXL read/write remain 0 until CXL-specific
	 * PMU discovery is implemented.
	 */
	spin_lock_irqsave(&st->bw_lock, flags);

	prev = st->bw_snapshot;

	snap.ddr_read_mb   = (total_read_cas  * CAS_BYTES) / BYTES_PER_MIB;
	snap.ddr_write_mb  = (total_write_cas * CAS_BYTES) / BYTES_PER_MIB;
	snap.cxl_read_mb   = 0;  /* TODO: cxlcm event */
	snap.cxl_write_mb  = 0;  /* TODO: cxldp event */
	snap.timestamp_ns  = now_ns;

	/* Compute interval rates (MiB/s) */
	if (prev.timestamp_ns > 0 && now_ns > prev.timestamp_ns) {
		u64 dt_ns = now_ns - prev.timestamp_ns;
		u64 dt_ms = dt_ns / 1000000ULL;

		if (dt_ms > 0) {
			snap.ddr_read_mb_s  = ((snap.ddr_read_mb  - prev.ddr_read_mb)
					       * 1000ULL) / dt_ms;
			snap.ddr_write_mb_s = ((snap.ddr_write_mb - prev.ddr_write_mb)
					       * 1000ULL) / dt_ms;
			snap.cxl_read_mb_s  = 0;
			snap.cxl_write_mb_s = 0;
		} else {
			snap.ddr_read_mb_s  = 0;
			snap.ddr_write_mb_s = 0;
			snap.cxl_read_mb_s  = 0;
			snap.cxl_write_mb_s = 0;
		}
	} else {
		snap.ddr_read_mb_s  = 0;
		snap.ddr_write_mb_s = 0;
		snap.cxl_read_mb_s  = 0;
		snap.cxl_write_mb_s = 0;
	}

	st->bw_snapshot = snap;

	spin_unlock_irqrestore(&st->bw_lock, flags);
}
