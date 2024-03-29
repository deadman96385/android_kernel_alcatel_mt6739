
#define DEBUG 1
#define SECTOR_SHIFT 12
#define UFS_MTK_BIO_TRACE_LATENCY (unsigned long long)(1000000000)
#define UFS_MTK_BIO_TRACE_TIMEOUT ((UFS_BIO_TRACE_LATENCY)*10)

#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/spinlock_types.h>
#include <linux/vmalloc.h>
#include <linux/smp.h>
#include <mt-plat/mtk_blocktag.h>
#include "ufs-mtk-block.h"

static void ufs_mtk_bio_ctx_count_usage
	(struct ufs_mtk_bio_context *ctx, __u64 start, __u64 end);
static uint64_t ufs_mtk_bio_get_period_busy
	(struct ufs_mtk_bio_context *ctx);

/* ring trace for debugfs */
struct mtk_blocktag *ufs_mtk_btag;

static inline uint16_t chbe16_to_u16(const char *str)
{
	uint16_t ret;

	ret = str[0];
	ret = ret << 8 | str[1];
	return ret;
}

static inline uint32_t chbe32_to_u32(const char *str)
{
	uint32_t ret;

	ret = str[0];
	ret = ret << 8 | str[1];
	ret = ret << 8 | str[2];
	ret = ret << 8 | str[3];
	return ret;
}

#define scsi_cmnd_lba(cmd)  chbe32_to_u32(&cmd->cmnd[2])
#define scsi_cmnd_len(cmd)  chbe16_to_u16(&cmd->cmnd[7])
#define scsi_cmnd_cmd(cmd)  (cmd->cmnd[0])

static struct ufs_mtk_bio_context_task *ufs_mtk_bio_get_task(struct ufs_mtk_bio_context *ctx, unsigned int task_id)
{
	struct ufs_mtk_bio_context_task *tsk = NULL;
	unsigned long flags;
	int i, avail = -1;

	if (!ctx)
		return NULL;

	spin_lock_irqsave(&ctx->lock, flags);

	for (i = 0; i < UFS_BIOLOG_CONTEXT_TASKS; i++) {
		tsk = &ctx->task[i];
		if (tsk->task_id == task_id)
			goto out;
		if ((tsk->task_id < 0) && (avail < 0))
			avail = i;
	}

	if (avail >= 0) {
		tsk = &ctx->task[avail];
		tsk->task_id = task_id;
		tsk->pid = current->pid;
		tsk->cpu_id = smp_processor_id();
		goto out;
	}

	pr_warn("ufs_mtk_bio_get_task: out of task, incoming task id = %d\n", task_id);

	for (i = 0; i < UFS_BIOLOG_CONTEXT_TASKS; i++)
		pr_warn("ufs_mtk_bio_get_task: task[%d]=%d\n", i, ctx->task[i].task_id);

out:
	spin_unlock_irqrestore(&ctx->lock, flags);
	return tsk;
}

static struct ufs_mtk_bio_context *ufs_mtk_bio_curr_ctx(void)
{
	struct ufs_mtk_bio_context *ctx = BTAG_CTX(ufs_mtk_btag);

	return ctx ? &ctx[0] : NULL;
}

static struct ufs_mtk_bio_context_task *ufs_mtk_bio_curr_task(unsigned int task_id,
	struct ufs_mtk_bio_context **curr_ctx)
{
	struct ufs_mtk_bio_context *ctx;

	ctx = ufs_mtk_bio_curr_ctx();
	if (curr_ctx)
		*curr_ctx = ctx;
	return ufs_mtk_bio_get_task(ctx, task_id);
}

int mtk_btag_pidlog_add_ufs(struct request_queue *q, pid_t pid, __u32 len, int rw)
{
	unsigned long flags;
	struct ufs_mtk_bio_context *ctx;

	ctx = ufs_mtk_bio_curr_ctx();
	if (!ctx)
		return 0;

	spin_lock_irqsave(&ctx->lock, flags);
	mtk_btag_pidlog_insert(&ctx->pidlog, pid, len, rw);
	spin_unlock_irqrestore(&ctx->lock, flags);

	return 1;
}
EXPORT_SYMBOL_GPL(mtk_btag_pidlog_add_ufs);


static const char *task_name[tsk_max] = {
	"request_start", "send_cmd", "req_compl", "done_start", "done_end"};

static void ufs_mtk_pr_tsk(struct ufs_mtk_bio_context_task *tsk,
	unsigned stage)
{
	const char *rw = "?";
	int klogen = BTAG_KLOGEN(ufs_mtk_btag);
	char buf[256];
	__u32 bytes;

	if (!((klogen == 2 && stage == tsk_scsi_done_end) || (klogen == 3)))
		return;

	if (tsk->cmd == 0x28)
		rw = "r";
	else if (tsk->cmd == 0x2A)
		rw = "w";

	bytes = ((__u32)tsk->len) << SECTOR_SHIFT;
	mtk_btag_task_timetag(buf, 256, stage, tsk_max, task_name, tsk->t, bytes);

	pr_debug("[BLOCK_TAG] ufs: tsk[%d]-(%d),%d,%s,%02X,pid=%u,len=%d%s\n",
		tsk->task_id, tsk->cpu_id, stage+1,	rw,	tsk->cmd, tsk->pid,	bytes, buf);
}

void ufs_mtk_biolog_queue_command(unsigned int task_id, struct scsi_cmnd *cmd)
{
	unsigned long flags;
	struct ufs_mtk_bio_context *ctx;
	struct ufs_mtk_bio_context_task *tsk;
	int i;

	if (!cmd)
		return;

	tsk = ufs_mtk_bio_curr_task(task_id, &ctx);
	if (!tsk)
		return;

	tsk->lba = scsi_cmnd_lba(cmd);
	tsk->len = scsi_cmnd_len(cmd);
	tsk->cmd = scsi_cmnd_cmd(cmd);

	tsk->t[tsk_request_start] = sched_clock();
	for (i = tsk_send_cmd; i < tsk_max; i++)
		tsk->t[i] = 0;

	spin_lock_irqsave(&ctx->lock, flags);
	if (!ctx->period_start_t)
		ctx->period_start_t = tsk->t[tsk_request_start];
	spin_unlock_irqrestore(&ctx->lock, flags);

	ufs_mtk_pr_tsk(tsk, tsk_request_start);

}

static void ufs_mtk_biolog_update_task(unsigned int task_id, unsigned stage)
{
	struct ufs_mtk_bio_context_task *tsk;

	tsk = ufs_mtk_bio_curr_task(task_id, NULL);
	if (!tsk || stage >= tsk_max)
		return;

	tsk->t[stage] = sched_clock();
	ufs_mtk_pr_tsk(tsk, stage);
}

void ufs_mtk_biolog_send_command(unsigned int task_id)
{
	ufs_mtk_biolog_update_task(task_id, tsk_send_cmd);
}

void ufs_mtk_biolog_transfer_req_compl(unsigned int task_id)
{
	ufs_mtk_biolog_update_task(task_id, tsk_req_compl);
}

void ufs_mtk_biolog_scsi_done_start(unsigned int task_id)
{
	ufs_mtk_biolog_update_task(task_id, tsk_scsi_done_start);
}

void ufs_mtk_biolog_scsi_done_end(unsigned int task_id)
{
	struct ufs_mtk_bio_context *ctx;
	struct ufs_mtk_bio_context_task *tsk;
	struct mtk_btag_throughput_rw *tp = NULL;
	unsigned long flags;
	int rw = -1, i;
	__u64 busy_time;
	__u32 size;

	tsk = ufs_mtk_bio_curr_task(task_id, &ctx);
	if (!tsk)
		return;

	tsk->t[tsk_scsi_done_end] = sched_clock();

	/* return if there's no on-going request  */
	for (i = 0; i < tsk_send_cmd; i++)
		if (!tsk->t[i])
			return;

	spin_lock_irqsave(&ctx->lock, flags);

	if (tsk->cmd == 0x28) {
		rw = 0; /* READ */
		tp = &ctx->throughput.r;
	} else if (tsk->cmd == 0x2A) {
		rw = 1; /* WRITE */
		tp = &ctx->throughput.w;
	}

	/* throughput usage := duration of handling this request */
	busy_time = tsk->t[tsk_scsi_done_end] - tsk->t[tsk_request_start];

	/* workload statistics */
	ctx->workload.count++;

	if (tp) {
		size = tsk->len << SECTOR_SHIFT;
		tp->usage += busy_time;
		tp->size += size;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	ufs_mtk_pr_tsk(tsk, tsk_scsi_done_end);
}

static void ufs_mtk_bio_ctx_count_usage(struct ufs_mtk_bio_context *ctx,
	__u64 start, __u64 end)
{
	if (start <= ctx->period_start_t) {
		ctx->period_end_since_start_t = end;
		ctx->period_start_in_window_t =
		ctx->period_end_in_window_t =
		ctx->period_busy = 0;
	} else {
		if (ctx->period_end_since_start_t) {
			if (start < ctx->period_end_since_start_t)
				ctx->period_end_since_start_t = end;
			else
				goto new_window;
		} else
			goto new_window;
	}

	goto out;

new_window:

	if (ctx->period_start_in_window_t) {
		if (start > ctx->period_end_in_window_t) {
			ctx->period_busy +=
				(ctx->period_end_in_window_t - ctx->period_start_in_window_t);
			ctx->period_start_in_window_t = start;
		}
		ctx->period_end_in_window_t = end;
	} else {
		ctx->period_start_in_window_t = start;
		ctx->period_end_in_window_t = end;
	}

out:
	return;
}

static uint64_t ufs_mtk_bio_get_period_busy(struct ufs_mtk_bio_context *ctx)
{
	uint64_t busy;

	busy = ctx->period_busy;

	if (ctx->period_end_since_start_t) {
		busy +=
			(ctx->period_end_since_start_t - ctx->period_start_t);
	}

	if (ctx->period_start_in_window_t) {
		busy +=
			(ctx->period_end_in_window_t - ctx->period_start_in_window_t);
	}

	return busy;
}

/* evaluate throughput and workload of given context */
static void ufs_mtk_bio_context_eval(struct ufs_mtk_bio_context *ctx)
{
	uint64_t period;

	ctx->workload.usage = ufs_mtk_bio_get_period_busy(ctx);

	if (ctx->workload.period > (ctx->workload.usage * 100)) {
		ctx->workload.percent = 1;
	} else {
		period = ctx->workload.period;
		do_div(period, 100);
		ctx->workload.percent = (__u32)ctx->workload.usage / (__u32)period;
	}
	mtk_btag_throughput_eval(&ctx->throughput);
}

/* print context to trace ring buffer */
static struct mtk_btag_trace *ufs_mtk_bio_print_trace(struct ufs_mtk_bio_context *ctx)
{
	struct mtk_btag_ringtrace *rt = BTAG_RT(ufs_mtk_btag);
	struct mtk_btag_trace *tr;
	unsigned long flags;

	if (!rt)
		return NULL;

	spin_lock_irqsave(&rt->lock, flags);
	tr = mtk_btag_curr_trace(rt);

	if (!tr)
		goto out;

	memset(tr, 0, sizeof(struct mtk_btag_trace));
	tr->pid = ctx->pid;
	tr->qid = ctx->qid;
	mtk_btag_pidlog_eval(&tr->pidlog, &ctx->pidlog);
	mtk_btag_vmstat_eval(&tr->vmstat);
	mtk_btag_cpu_eval(&tr->cpu);
	memcpy(&tr->throughput, &ctx->throughput, sizeof(struct mtk_btag_throughput));
	memcpy(&tr->workload, &ctx->workload, sizeof(struct mtk_btag_workload));

	tr->time = sched_clock();
	mtk_btag_next_trace(rt);
out:
	spin_unlock_irqrestore(&rt->lock, flags);
	return tr;
}

/* Check requests after set/clear mask. */
void ufs_mtk_biolog_check(unsigned long req_mask)
{
	struct ufs_mtk_bio_context *ctx;
	struct mtk_btag_trace *tr = NULL;
	__u64 end_time, period_time;
	unsigned long flags;

	ctx = ufs_mtk_bio_curr_ctx();
	if (!ctx)
		return;

	end_time = sched_clock();

	spin_lock_irqsave(&ctx->lock, flags);

	if (ctx->busy_start_t)
		ufs_mtk_bio_ctx_count_usage(ctx, ctx->busy_start_t, end_time);

	ctx->busy_start_t = (req_mask) ? end_time : 0;

	period_time = end_time - ctx->period_start_t;

	if (period_time >= UFS_MTK_BIO_TRACE_LATENCY) {
		ctx->period_end_t = end_time;
		ctx->workload.period = period_time;
		ufs_mtk_bio_context_eval(ctx);
		tr = ufs_mtk_bio_print_trace(ctx);
		ctx->period_start_t = end_time;
		ctx->period_end_t = 0;
		ctx->period_busy = 0;
		ctx->period_end_since_start_t = 0;
		ctx->period_end_in_window_t = 0;
		ctx->period_start_in_window_t = 0;
		memset(&ctx->throughput, 0, sizeof(struct mtk_btag_throughput));
		memset(&ctx->workload, 0, sizeof(struct mtk_btag_workload));
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	mtk_btag_klog(ufs_mtk_btag, tr);
}

#define SPREAD_PRINTF(buff, size, evt, fmt, args...) \
do { \
	if (buff && size && *(size)) { \
		unsigned long var = snprintf(*(buff), *(size), fmt, ##args); \
		if (var > 0) { \
			*(size) -= var; \
			*(buff) += var; \
		} \
	} \
	if (evt) \
		seq_printf(evt, fmt, ##args); \
	if (!buff && !evt) { \
		pr_info(fmt, ##args); \
	} \
} while (0)

static size_t ufs_mtk_bio_seq_debug_show_info(char **buff, unsigned long *size,
	struct seq_file *seq)
{
	int i;
	struct ufs_mtk_bio_context *ctx = BTAG_CTX(ufs_mtk_btag);

	if (!ctx)
		return 0;

	for (i = 0; i < UFS_BIOLOG_CONTEXTS; i++)	{
		if (ctx[i].pid == 0)
			continue;
		SPREAD_PRINTF(buff, size, seq, "ctx[%d]=ctx_map[%d],pid:%4d,q:%d\n",
			i,
			ctx[i].id,
			ctx[i].pid,
			ctx[i].qid);
	}

	return 0;
}

static void ufs_mtk_bio_init_ctx(struct ufs_mtk_bio_context *ctx)
{
	int i;

	memset(ctx, 0, sizeof(struct ufs_mtk_bio_context));
	spin_lock_init(&ctx->lock);
	ctx->period_start_t = sched_clock();

	for (i = 0; i < UFS_BIOLOG_CONTEXT_TASKS; i++)
		ctx->task[i].task_id = -1;
}

int ufs_mtk_biolog_init(void)
{
	struct mtk_blocktag *btag;

	btag = mtk_btag_alloc("ufs",
		UFS_BIOLOG_RINGBUF_MAX,
		sizeof(struct ufs_mtk_bio_context),
		UFS_BIOLOG_CONTEXTS,
		ufs_mtk_bio_seq_debug_show_info);

	if (btag) {
		struct ufs_mtk_bio_context *ctx;

		ufs_mtk_btag = btag;
		ctx = BTAG_CTX(ufs_mtk_btag);
		ufs_mtk_bio_init_ctx(&ctx[0]);
	}
	return 0;
}

int ufs_mtk_biolog_exit(void)
{
	mtk_btag_free(ufs_mtk_btag);
	return 0;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mediatek UFS Block IO Log");

