// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * worker_pool.h — shared persistent worker-pool lifecycle for AutoUpscale
 *****************************************************************************
 * ARCH-2. The USM pool (usm_pool.c) and the zimg worker grid (scaler_zimg.c)
 * ran two parallel state machines with identical semantics: sticky lazy init,
 * poison-on-barrier-failure, cache-line-aligned worker array, partial-spawn
 * teardown, one-broadcast dispatch and wait. Only the gate (threading.h) had
 * been factored out, so every reliability fix had to land twice — and several
 * had landed in one pool only.
 *
 * This header owns everything above the gate:
 *
 *   - the worker payload array (one 64-byte-aligned slot per worker; the
 *     payload TYPE stays private to each pool, the pool only knows its size),
 *   - the thread records (pthread_t + per-worker seen_gen),
 *   - lazy start with sticky failure, partial-spawn policy, teardown,
 *   - dispatch-and-wait, and poison (broken pools stop their threads).
 *
 * Pools plug in through up_worker_pool_ops_t. Only `construct` and `run` are
 * required; the rest are optional hooks.
 *
 * PERF-1 (inline_run): a one-worker pool has no parallelism to buy, so no gate
 * is initialized and no thread is spawned — up_worker_pool_dispatch runs the
 * single slot on the calling thread. The auto thread policy resolves to 1 on
 * every machine with <= 7 cores, so this is the common desktop path.
 *
 * Threading contract (inherited from the gate, see threading.h): the owner
 * writes per-dispatch worker state before up_worker_pool_dispatch (or inside
 * the `arm` hook, under the gate lock); workers publish their results through
 * the done barrier. No worker touches another worker's slot.
 *****************************************************************************/

#ifndef AUTOUPSCALE_WORKER_POOL_H
#define AUTOUPSCALE_WORKER_POOL_H

#include "threading.h"

#include <pthread.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UP_POOL_CACHELINE 64

typedef struct up_worker_pool_s up_worker_pool_t;

/*
 * One pool-owned thread. Cache-line aligned: the worker writes `seen_gen` on
 * every dispatch, so two records sharing a line would invalidate each other's
 * line every frame. _Alignas on the first member also rounds sizeof up to a
 * 64-byte multiple, which keeps an aligned_alloc'd array element-aligned.
 */
typedef struct {
    alignas(UP_POOL_CACHELINE) pthread_t thread;
    bool              started;    /* pthread_create succeeded (pthread_t is
                                   * opaque: no portable "is it a handle?"
                                   * test, so track it explicitly) */
    uint64_t          seen_gen;   /* worker-private: last dispatch handled */
    int               index;
    up_worker_pool_t *pool;
} up_pool_thread_t;

typedef struct {
    /* Required. Set up worker slot `index` (geometry, graphs, buffers).
     * Returns 0, or -1 after releasing whatever it acquired itself. */
    int  (*construct)(void *owner, int index);

    /* Required. One dispatch's work for slot `index`. Runs on that worker's
     * thread, or on the caller's thread when the pool is inline. */
    void (*run)(void *owner, int index);

    /* Optional. One-time pool-wide setup before any slot is allocated
     * (shared scratch). Returns 0 or -1. */
    int  (*prepare)(void *owner);

    /* Optional. Release what construct() acquired for slot `index`. Called
     * for every constructed slot on teardown, and for a slot whose thread
     * failed to spawn. Never called for a slot construct() rejected — that
     * one cleans up after itself. */
    void (*release)(void *owner, int index);

    /* Optional. Called under the gate lock immediately before the wake, with
     * this dispatch's worker count: reset per-dispatch state (result codes)
     * a waking worker must not observe stale. */
    void (*arm)(void *owner, int n_workers);

    /* Optional. Called right after a worker thread is created (CPU pinning). */
    void (*on_spawn)(void *owner, int index, pthread_t thread);

    /* Optional. Called once with the number of workers that actually came up,
     * before the first dispatch (repartition work over the real count). */
    void (*finalize)(void *owner, int n_workers);

    /* true: every requested worker must come up or the pool fails (zimg — a
     * missing grid cell would leave part of the frame unwritten).
     * false: a partial spawn is usable (USM — stripes repartition over
     * whatever came up). */
    bool all_or_nothing;
} up_worker_pool_ops_t;

struct up_worker_pool_s {
    up_pool_gate_t    gate;

    void             *workers;     /* n_pref slots of worker_size bytes */
    up_pool_thread_t *threads;     /* n_pref records; none when inline */
    size_t            worker_size;
    int               n_pref;      /* workers requested */
    int               n_workers;   /* workers that came up (also: live slots) */

    bool              inline_run;  /* PERF-1: single slot, no gate, no thread */
    bool              lazy_done;   /* start() succeeded */
    bool              lazy_failed; /* sticky: never retry a failed start */
    bool              broken;      /* poisoned: barrier or worker failure */

    const up_worker_pool_ops_t *ops;
    void                       *owner;
};

/*
 * Describe the pool. `worker_size` must be a multiple of the cache line (the
 * payload structs carry _Alignas(64) for exactly this reason), so an aligned
 * array keeps every slot on its own line. No allocation happens here: the
 * descriptor stays cheap so VLC's speculative filter-chain probes cost
 * nothing (see up_worker_pool_ensure_started).
 */
static inline void up_worker_pool_config(up_worker_pool_t *p,
                                         const up_worker_pool_ops_t *ops,
                                         void *owner, int n_pref,
                                         size_t worker_size)
{
    p->ops         = ops;
    p->owner       = owner;
    p->n_pref      = n_pref;
    p->n_workers   = 0;
    p->worker_size = worker_size;
}

/* Slot `i`'s payload. The pool never dereferences it — only the owner knows
 * the type. Valid for i in [0, n_pref) once the pool has started. */
static inline void *up_worker_pool_slot(const up_worker_pool_t *p, int i)
{
    return (char *)p->workers + (size_t)i * p->worker_size;
}

static inline int up_worker_pool_count(const up_worker_pool_t *p)
{
    return p->n_workers;
}

static inline bool up_worker_pool_broken(const up_worker_pool_t *p)
{
    return p->broken;
}

static inline bool up_worker_pool_inline(const up_worker_pool_t *p)
{
    return p->inline_run;
}

/* ---------- start ---------- */

static inline void *up__pool_worker_main(void *arg)
{
    up_pool_thread_t *t = (up_pool_thread_t *)arg;
    up_worker_pool_t *p = t->pool;
    for (;;) {
        if (!up_pool_gate_wait_for_go(&p->gate, &t->seen_gen)) break;
        p->ops->run(p->owner, t->index);
        up_pool_gate_worker_done(&p->gate);
    }
    return NULL;
}

/* aligned_alloc, not calloc: the payload structs are _Alignas(64) and calloc
 * only guarantees malloc-default (16) alignment, which would defeat the
 * per-worker cache-line layout. sizeof is already a 64-byte multiple, which
 * satisfies aligned_alloc's C11 size constraint; memset replaces the zeroing.
 * n_pref is bounded by UP_THREADS_MAX, so neither product can overflow. */
static inline int up__pool_alloc(up_worker_pool_t *p)
{
    const size_t wbytes = (size_t)p->n_pref * p->worker_size;
    p->workers = aligned_alloc(UP_POOL_CACHELINE, wbytes);
    if (p->workers == NULL) return -1;
    memset(p->workers, 0, wbytes);

    if (p->n_pref == 1) return 0;   /* inline: no thread records needed */

    const size_t tbytes = (size_t)p->n_pref * sizeof(up_pool_thread_t);
    p->threads = (up_pool_thread_t *)aligned_alloc(UP_POOL_CACHELINE, tbytes);
    if (p->threads == NULL) return -1;
    memset(p->threads, 0, tbytes);
    return 0;
}

static inline int up__pool_spawn(up_worker_pool_t *p, int i)
{
    up_pool_thread_t *t = &p->threads[i];
    t->pool     = p;
    t->index    = i;
    t->seen_gen = p->gate.generation;   /* don't run before the first dispatch */

    if (pthread_create(&t->thread, NULL, up__pool_worker_main, t) != 0)
        return -1;
    t->started = true;
    if (p->ops->on_spawn) p->ops->on_spawn(p->owner, i, t->thread);
    return 0;
}

/* Construct (and, unless inline, spawn) slots in order. Stops at the first
 * failure and returns how many came up whole; the caller applies the pool's
 * all-or-nothing policy. A slot whose thread failed to spawn is released here
 * — it is constructed but unusable. */
static inline int up__pool_construct_all(up_worker_pool_t *p)
{
    const int limit = p->inline_run ? 1 : p->n_pref;
    int built = 0;
    for (int i = 0; i < limit; i++) {
        if (p->ops->construct(p->owner, i) != 0) break;
        if (!p->inline_run && up__pool_spawn(p, i) != 0) {
            if (p->ops->release) p->ops->release(p->owner, i);
            break;
        }
        built++;
    }
    return built;
}

/* Tell every started worker to finish any unseen generation, then exit, and
 * reap them. Safe on a pool that never started or already stopped. */
static inline void up_worker_pool_stop(up_worker_pool_t *p)
{
    if (p->threads == NULL) return;
    up_pool_gate_request_exit(&p->gate);
    for (int i = 0; i < p->n_pref; i++) {
        if (!p->threads[i].started) continue;
        pthread_join(p->threads[i].thread, NULL);
        p->threads[i].started = false;
    }
}

static inline void up__pool_release_slots(up_worker_pool_t *p, int n)
{
    if (p->ops->release == NULL) return;
    for (int i = 0; i < n; i++) p->ops->release(p->owner, i);
}

/* Everything the pool needs before the first slot can be constructed. */
static inline int up__pool_setup(up_worker_pool_t *p)
{
    if (p->n_pref < 1 || p->n_pref > UP_THREADS_MAX) return -1;
    if (p->ops->prepare && p->ops->prepare(p->owner) != 0) return -1;
    if (up__pool_alloc(p) != 0) return -1;

    p->inline_run = (p->n_pref == 1);   /* PERF-1 */

    /* The gate must be live before the first spawn: a worker blocks on the cv
     * immediately (seen_gen == generation). */
    if (!p->inline_run && up_pool_gate_init(&p->gate) != 0) return -1;
    return 0;
}

/* Is a pool of `built` workers usable? Always no if nothing came up; for an
 * all-or-nothing pool, also no unless every requested worker did. */
static inline bool up__pool_build_ok(const up_worker_pool_t *p, int built)
{
    const int need = p->inline_run ? 1 : p->n_pref;
    if (built == 0) return false;
    return !p->ops->all_or_nothing || built >= need;
}

/*
 * Bring the pool up: pool-wide prepare, slot array, gate, then construct and
 * spawn. Returns 0 with n_workers set, or -1 with every worker that did come
 * up stopped and released (the allocations themselves are freed by
 * up_worker_pool_destroy, which the owner calls on its own teardown path).
 */
static inline int up_worker_pool_start(up_worker_pool_t *p)
{
    if (up__pool_setup(p) != 0) return -1;

    const int built = up__pool_construct_all(p);
    if (!up__pool_build_ok(p, built)) {
        up_worker_pool_stop(p);
        up__pool_release_slots(p, built);
        return -1;
    }

    p->n_workers = built;
    if (p->ops->finalize) p->ops->finalize(p->owner, built);
    return 0;
}

/*
 * Lazy start on the first dispatch that has real work. VLC's filter-chain
 * solver instantiates filters speculatively, so a probe that never produces a
 * frame must not pay for threads, graphs or scratch. Failure is sticky: a
 * pool that could not come up is never retried (it would re-allocate on every
 * frame), and the owner reports the failure to its caller.
 */
static inline int up_worker_pool_ensure_started(up_worker_pool_t *p)
{
    if (p->lazy_done)   return 0;
    if (p->lazy_failed) return -1;
    if (up_worker_pool_start(p) != 0) {
        p->lazy_failed = true;
        return -1;
    }
    p->lazy_done = true;
    return 0;
}

static inline bool up_worker_pool_started(const up_worker_pool_t *p)
{
    return p->lazy_done;
}

static inline bool up_worker_pool_failed(const up_worker_pool_t *p)
{
    return p->lazy_failed;
}

/* ---------- dispatch ---------- */

/*
 * Poison the pool: a barrier failure or a fatal worker error means we can
 * neither trust nor use it again. Stop the threads right here — leaving up to
 * UP_THREADS_MAX workers parked on the gate for the rest of playback, holding
 * their graphs and scratch, is exactly the unbounded-hold-after-permanent-
 * failure this exists to prevent (RES-2). Idempotent.
 */
static inline void up_worker_pool_poison(up_worker_pool_t *p)
{
    p->broken = true;
    up_worker_pool_stop(p);
}

/*
 * Run one dispatch and wait for every worker: O(1) syscalls each way. Arm the
 * done-barrier and bump the generation under the gate lock, wake all workers
 * with a single broadcast, then wait once on the counting barrier.
 *
 * Returns 0 once every worker has completed. A non-EINTR barrier failure
 * poisons the pool (threads stopped and joined) and returns -1; the caller
 * must treat the dispatch as fatal, not retry it.
 */
static inline int up_worker_pool_dispatch(up_worker_pool_t *p)
{
    if (p->inline_run) {
        if (p->ops->arm) p->ops->arm(p->owner, 1);
        p->ops->run(p->owner, 0);
        return 0;
    }

    up_pool_gate_lock(&p->gate);
    up_pool_gate_arm_locked(&p->gate, p->n_workers);
    if (p->ops->arm) p->ops->arm(p->owner, p->n_workers);
    up_pool_gate_unlock_broadcast(&p->gate);

    if (up_pool_gate_wait_all(&p->gate) == 0) return 0;

    up_worker_pool_poison(p);
    return -1;
}

/* ---------- teardown ---------- */

/*
 * Stop the threads, release every live slot, and free the pool's own storage.
 * Safe on a pool that was configured but never started, and on one whose start
 * failed part-way (its slots were already released; n_workers is 0).
 */
static inline void up_worker_pool_destroy(up_worker_pool_t *p)
{
    up_worker_pool_stop(p);
    up__pool_release_slots(p, p->n_workers);
    p->n_workers = 0;

    free(p->workers);
    p->workers = NULL;
    free(p->threads);
    p->threads = NULL;
    up_pool_gate_destroy(&p->gate);
}

#endif /* AUTOUPSCALE_WORKER_POOL_H */
