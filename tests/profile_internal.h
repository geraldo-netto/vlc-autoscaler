// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_PROFILE_INTERNAL_H
#define AUTOUPSCALE_PROFILE_INTERNAL_H

#include "profile_stage.h"
#include "../src/worker_pool.h"
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct {
    alignas(64) double start, end, cpu;
    long tid;
} up_profile_slot_t;

typedef struct {
    int mode;
    up_worker_pool_ops_t ops;
    const up_worker_pool_ops_t *original;
    up_profile_slot_t slots[UP_THREADS_MAX];
    up_profile_frame_t frame;
    double start, end;
} up_profile_trace_t;

static up_profile_trace_t up_profile_trace;

static double up_profile_clock(clockid_t clock)
{
    struct timespec now;
    if (clock_gettime(clock, &now) != 0) {
        perror("profile clock");
        abort();
    }
    return (double)now.tv_sec * 1e6 + (double)now.tv_nsec / 1e3;
}

static void up_profile_run(void *owner, int index)
{
    up_profile_trace_t *t = &up_profile_trace;
    up_profile_slot_t *s = &t->slots[index];
    if (!s->tid) s->tid = syscall(SYS_gettid);
    double cpu = t->mode == 2 ? up_profile_clock(CLOCK_THREAD_CPUTIME_ID) : 0.0;
    s->start = up_profile_clock(CLOCK_MONOTONIC);
    t->original->run(owner, index);
    s->end = up_profile_clock(CLOCK_MONOTONIC);
    s->cpu = t->mode == 2 ? up_profile_clock(CLOCK_THREAD_CPUTIME_ID) - cpu : 0.0;
}

static up_profile_frame_t up_profile_reduce(const up_profile_trace_t *t, int n)
{
    up_profile_frame_t f = { .min_work_us = 1e100 };
    double first = t->slots[0].start, last = first, done = t->slots[0].end;
    for (int i = 0; i < n; i++) {
        const up_profile_slot_t *s = &t->slots[i];
        const double work = s->end - s->start;
        if (s->start < first) first = s->start;
        if (s->start > last) last = s->start;
        if (s->end > done) done = s->end;
        if (work < f.min_work_us) f.min_work_us = work;
        if (work > f.max_work_us) f.max_work_us = work;
        f.mean_work_us += work / n;
        f.worker_cpu_us += s->cpu;
    }
    f.dispatch_us = t->end - t->start;
    f.first_start_us = first - t->start;
    f.last_start_us = last - t->start;
    f.handoff_us = t->end - done;
    return f;
}

static int up_profile_dispatch(up_worker_pool_t *pool)
{
    up_profile_trace_t *t = &up_profile_trace;
    if (!t->mode) return up_worker_pool_dispatch(pool);
    if (!t->original) {
        t->original = pool->ops;
        t->ops = *pool->ops;
        t->ops.run = up_profile_run;
        pool->ops = &t->ops;
    }
    t->start = up_profile_clock(CLOCK_MONOTONIC);
    const int rc = up_worker_pool_dispatch(pool);
    t->end = up_profile_clock(CLOCK_MONOTONIC);
    if (!rc) t->frame = up_profile_reduce(t, up_worker_pool_count(pool));
    return rc;
}

static int up_profile_read_sched(long tid, up_profile_sched_t *out)
{
    char path[96];
    if (snprintf(path, sizeof path, "/proc/self/task/%ld/schedstat", tid) < 0)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    unsigned long long runtime, runqueue, slices;
    const int fields = fscanf(f, "%llu %llu %llu", &runtime, &runqueue, &slices);
    const int closed = fclose(f);
    if (fields != 3 || closed != 0) return -1;
    out->runtime_ns += (uint64_t)runtime;
    out->runqueue_ns += (uint64_t)runqueue;
    out->slices += (uint64_t)slices;
    return 0;
}

static int up_profile_pool_sched(const up_worker_pool_t *pool,
                                 up_profile_sched_t *out)
{
    out->workers = up_worker_pool_count(pool);
    if (!up_profile_trace.mode) return 0;
    for (int i = 0; i < out->workers; i++)
        if (up_profile_read_sched(up_profile_trace.slots[i].tid, out)) return -1;
    return 0;
}

#endif
