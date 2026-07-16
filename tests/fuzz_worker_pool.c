// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_worker_pool.c — randomized lifecycles of the shared worker pool
 *****************************************************************************
 * The unit test pins the named failure paths; this drives the state machine
 * with arbitrary combinations of them — worker count, spawn policy, which slot
 * refuses to construct, how many threads the OS hands out, how many dispatches
 * follow, whether the pool gets poisoned mid-life — and asserts the invariants
 * that must hold for EVERY lifecycle:
 *
 *   - a started pool has 1..n_pref workers; an all-or-nothing pool has exactly
 *     n_pref (a missing zimg cell would leave part of a frame unwritten),
 *   - a successful dispatch runs each live worker exactly once — none skipped
 *     (torn output), none twice (double-processed stripe),
 *   - a failed start leaves no live slot: every constructed slot is released,
 *     and none twice,
 *   - a poisoned pool sticks and stays destroyable,
 *   - destroy is safe after any prefix of the lifecycle.
 *
 * ASan/UBSan/LSan carry the memory half: a leaked slot array, a leaked thread,
 * or a use-after-free in teardown fails the run.
 *
 * Two entry points: LLVMFuzzerTestOneInput (libFuzzer) + a deterministic
 * smoke-fuzz main() for CI.
 *****************************************************************************/

#include "../src/worker_pool.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Spawn budget: the OS hands out this many threads, then pthread_create
 * fails (EAGAIN — RLIMIT_NPROC, the real-world case). -1 = unlimited. */
static atomic_int g_spawn_budget;

int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *th, const pthread_attr_t *attr,
                          void *(*fn)(void *), void *arg)
{
    int budget = atomic_load_explicit(&g_spawn_budget, memory_order_relaxed);
    if (budget == 0) return EAGAIN;
    if (budget > 0)
        atomic_fetch_sub_explicit(&g_spawn_budget, 1, memory_order_relaxed);
    return __real_pthread_create(th, attr, fn, arg);
}

typedef struct {
    alignas(UP_POOL_CACHELINE) int live;   /* constructed and not yet released */
    atomic_int runs;
} fz_worker_t;

typedef struct {
    up_worker_pool_t pool;
    int construct_fail_at;
    int constructed;
    int released;
    int double_release;      /* released a slot that was not live */
    int finalize_n;
} fz_pool_t;

static fz_worker_t *fz_slots(fz_pool_t *f)
{
    return (fz_worker_t *)up_worker_pool_slot(&f->pool, 0);
}

static int fz_construct(void *owner, int i)
{
    fz_pool_t *f = (fz_pool_t *)owner;
    if (i == f->construct_fail_at) return -1;
    fz_slots(f)[i].live = 1;
    f->constructed++;
    return 0;
}

static void fz_release(void *owner, int i)
{
    fz_pool_t *f = (fz_pool_t *)owner;
    fz_worker_t *w = &fz_slots(f)[i];
    if (!w->live) f->double_release++;
    w->live = 0;
    f->released++;
}

static void fz_run(void *owner, int i)
{
    atomic_fetch_add_explicit(&fz_slots((fz_pool_t *)owner)[i].runs, 1,
                              memory_order_relaxed);
}

static void fz_finalize(void *owner, int n)
{
    ((fz_pool_t *)owner)->finalize_n = n;
}

static const up_worker_pool_ops_t fz_ops_partial = {
    .construct = fz_construct, .run = fz_run, .release = fz_release,
    .finalize = fz_finalize, .all_or_nothing = false,
};
static const up_worker_pool_ops_t fz_ops_strict = {
    .construct = fz_construct, .run = fz_run, .release = fz_release,
    .finalize = fz_finalize, .all_or_nothing = true,
};

#define FZ_FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); return 1; } while (0)

/* Invariants of a pool that came up. */
static int check_started(const fz_pool_t *f, int n_pref, bool strict)
{
    const int n = up_worker_pool_count(&f->pool);
    if (n < 1 || n > n_pref)
        FZ_FAIL("worker count %d outside [1, %d]\n", n, n_pref);
    if (strict && n != n_pref)
        FZ_FAIL("all-or-nothing pool came up with %d of %d\n", n, n_pref);
    if (f->finalize_n != n)
        FZ_FAIL("finalize saw %d, pool has %d\n", f->finalize_n, n);
    if (up_worker_pool_inline(&f->pool) != (n_pref == 1))
        FZ_FAIL("inline must hold exactly when one worker was requested\n");
    return 0;
}

/* Nothing survives a failed start, and the failure never retries. */
static int check_failed(fz_pool_t *f)
{
    if (up_worker_pool_count(&f->pool) != 0)
        FZ_FAIL("failed start kept %d workers\n",
                up_worker_pool_count(&f->pool));
    if (f->released != f->constructed)
        FZ_FAIL("failed start released %d of %d constructed slots\n",
                f->released, f->constructed);
    if (f->double_release)
        FZ_FAIL("failed start released a slot twice\n");
    if (!up_worker_pool_failed(&f->pool))
        FZ_FAIL("start failure is not sticky\n");
    if (up_worker_pool_ensure_started(&f->pool) != -1)
        FZ_FAIL("a pool whose start failed was restarted\n");
    return 0;
}

static int check_dispatch_counts(fz_pool_t *f, int expected)
{
    const int n = up_worker_pool_count(&f->pool);
    for (int i = 0; i < n; i++) {
        const int runs = atomic_load(&fz_slots(f)[i].runs);
        if (runs != expected)
            FZ_FAIL("worker %d ran %d times, expected %d\n", i, runs, expected);
    }
    return 0;
}

static int run_dispatches(fz_pool_t *f, int dispatches)
{
    for (int d = 0; d < dispatches; d++) {
        if (up_worker_pool_dispatch(&f->pool) != 0)
            FZ_FAIL("dispatch failed with no injected barrier fault\n");
        if (check_dispatch_counts(f, d + 1)) return 1;
    }
    return 0;
}

/* Teardown invariants, whichever path got us here. */
static int destroy_and_check(fz_pool_t *f)
{
    if (up_worker_pool_destroy(&f->pool) != 0)
        abort();
    if (f->released != f->constructed) FZ_FAIL("destroy leaked a live slot\n");
    if (f->double_release)             FZ_FAIL("destroy released a slot twice\n");
    return 0;
}

/* The barrier- and worker-failure path: the poison sticks, the threads are
 * gone, and the pool stays destroyable. */
static int poison_pool(fz_pool_t *f)
{
    up_worker_pool_poison(&f->pool);
    if (!up_worker_pool_broken(&f->pool)) FZ_FAIL("poison did not stick\n");
    up_worker_pool_poison(&f->pool);      /* idempotent */
    return 0;
}

static int run_lifecycle(fz_pool_t *f, int n_pref, bool strict, int dispatches,
                         bool poison)
{
    if (up_worker_pool_ensure_started(&f->pool) != 0) {
        if (check_failed(f)) return 1;
        return destroy_and_check(f);
    }
    if (check_started(f, n_pref, strict)) return 1;
    if (run_dispatches(f, dispatches))    return 1;
    if (poison && poison_pool(f))         return 1;
    return destroy_and_check(f);
}

static int run_one(const uint8_t *data, size_t size)
{
    up_worker_pool_t unconfigured = { 0 };
    if (up_worker_pool_destroy(&unconfigured) != 0)
        FZ_FAIL("unconfigured pool was not destroyable\n");
    if (size < 4) return 0;

    const int  n_pref     = 1 + (data[0] % 8);            /* 1..8 workers */
    const bool strict     = (data[1] & 1) != 0;
    const int  fail_at    = (data[1] & 2) ? (data[2] % 9) : -1;
    const int  budget     = (data[1] & 4) ? (data[3] % 9) : -1;
    const int  dispatches = (int)(size % 5);
    const bool poison     = (data[0] & 0x80) != 0;

    atomic_store(&g_spawn_budget, budget);

    fz_pool_t f;
    memset(&f, 0, sizeof f);
    f.construct_fail_at = fail_at;
    f.finalize_n        = -1;
    up_worker_pool_config(&f.pool, strict ? &fz_ops_strict : &fz_ops_partial,
                          &f, n_pref, sizeof(fz_worker_t));

    return run_lifecycle(&f, n_pref, strict, dispatches, poison);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (run_one(data, size)) abort();
    return 0;
}

#ifdef FUZZ_MAIN
#include "fuzz_smoke.h"

static int smoke_iter(long i)
{
    (void)i;
    uint8_t buf[8];
    fuzz_smoke_fill(buf, sizeof buf);
    /* Bias the length so `dispatches` (size % 5) sweeps its whole range. */
    size_t n = 4 + (size_t)(fuzz_smoke_next() % (sizeof buf - 3));
    return run_one(buf, n);
}

int main(int argc, char **argv)
{
    fuzz_smoke_seed(0x7ea1b0015eed7a11ULL);
    return fuzz_smoke_main(argc, argv, 2000, "worker_pool", smoke_iter);
}
#endif
