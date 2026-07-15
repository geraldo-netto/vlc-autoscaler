// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_worker_pool.c — the shared worker-pool lifecycle (ARCH-2)
 *****************************************************************************
 * worker_pool.h is the one lifecycle both real pools now run on (usm_pool.c,
 * scaler_zimg.c): lazy start with sticky failure, partial-spawn policy,
 * inline single-worker path, dispatch-and-wait, poison, teardown. Every one
 * of those failure paths used to exist twice and be tested in at most one of
 * the two copies, so they get direct coverage here with a fake owner: slot
 * allocation failure, thread-spawn failure, per-slot construct failure under
 * both spawn policies, and a broken completion barrier.
 *
 * Fault injection: --wrap on aligned_alloc (slot arrays), pthread_create/join
 * (owned worker lifecycle), pthread_setcancelstate, and the barrier primitives
 * (tests/barrier_fault_inject.h).
 *****************************************************************************/

#include "../src/worker_pool.h"
#include "barrier_fault_inject.h"
#include "test_harness.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---------- fault injection ---------- */

static atomic_int g_fail_aligned_alloc_nth;   /* 1-based; 0 = never */
static atomic_int g_aligned_alloc_calls;
static atomic_int g_spawn_budget;             /* -1 = unlimited */
static atomic_int g_fail_cancel_state = -1;
static atomic_int g_thread_create_calls;
static atomic_int g_thread_join_calls;
static atomic_int g_fail_join_nth;          /* 1-based; 0 = never */
static atomic_int g_join_attempts;

void *__real_aligned_alloc(size_t alignment, size_t size);
void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    const int n = atomic_fetch_add_explicit(&g_aligned_alloc_calls, 1,
                                            memory_order_relaxed) + 1;
    if (atomic_load_explicit(&g_fail_aligned_alloc_nth,
                             memory_order_relaxed) == n) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_aligned_alloc(alignment, size);
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *th, const pthread_attr_t *attr,
                          void *(*fn)(void *), void *arg)
{
    int budget = atomic_load_explicit(&g_spawn_budget, memory_order_relaxed);
    if (budget == 0) return EAGAIN;
    if (budget > 0)
        atomic_fetch_sub_explicit(&g_spawn_budget, 1, memory_order_relaxed);
    const int rc = __real_pthread_create(th, attr, fn, arg);
    if (rc == 0)
        atomic_fetch_add_explicit(&g_thread_create_calls, 1,
                                  memory_order_relaxed);
    return rc;
}

int __real_pthread_join(pthread_t thread, void **retval);
int __wrap_pthread_join(pthread_t thread, void **retval)
{
    const int attempt = atomic_fetch_add_explicit(&g_join_attempts, 1,
                                                   memory_order_relaxed) + 1;
    if (atomic_load_explicit(&g_fail_join_nth, memory_order_relaxed)
        == attempt)
        return EINVAL;

    const int rc = __real_pthread_join(thread, retval);
    if (rc == 0)
        atomic_fetch_add_explicit(&g_thread_join_calls, 1,
                                  memory_order_relaxed);
    return rc;
}

int __real_pthread_setcancelstate(int state, int *old_state);
int __wrap_pthread_setcancelstate(int state, int *old_state)
{
    int expected = state;
    if (atomic_compare_exchange_strong_explicit(
            &g_fail_cancel_state, &expected, -1,
            memory_order_relaxed, memory_order_relaxed))
        return EINVAL;
    return __real_pthread_setcancelstate(state, old_state);
}

static void fault_reset(void)
{
    atomic_store(&g_fail_aligned_alloc_nth, 0);
    atomic_store(&g_aligned_alloc_calls, 0);
    atomic_store(&g_spawn_budget, -1);
    atomic_store(&g_fail_cancel_state, -1);
    atomic_store(&g_thread_create_calls, 0);
    atomic_store(&g_thread_join_calls, 0);
    atomic_store(&g_fail_join_nth, 0);
    atomic_store(&g_join_attempts, 0);
}

/* ---------- fake owner ---------- */

typedef struct {
    alignas(UP_POOL_CACHELINE) int constructed;
    atomic_int runs;
} fake_worker_t;

typedef struct {
    up_worker_pool_t pool;
    int  prepare_rc;
    int  construct_fail_at;   /* slot index that refuses to construct; -1 none */
    int  prepare_calls;
    int  construct_calls;
    int  release_calls;
    int  spawn_calls;
    int  finalize_n;          /* -1 until finalize runs */
    atomic_int total_runs;
    atomic_int run_cancel_enabled;
} fake_pool_t;

static fake_worker_t *fake_slots(fake_pool_t *f)
{
    return (fake_worker_t *)up_worker_pool_slot(&f->pool, 0);
}

static int fake_prepare(void *owner)
{
    fake_pool_t *f = (fake_pool_t *)owner;
    f->prepare_calls++;
    return f->prepare_rc;
}

static int fake_construct(void *owner, int i)
{
    fake_pool_t *f = (fake_pool_t *)owner;
    f->construct_calls++;
    if (i == f->construct_fail_at) return -1;
    fake_slots(f)[i].constructed = 1;
    return 0;
}

static void fake_release(void *owner, int i)
{
    fake_pool_t *f = (fake_pool_t *)owner;
    f->release_calls++;
    fake_slots(f)[i].constructed = 0;
}

static void fake_run(void *owner, int i)
{
    fake_pool_t *f = (fake_pool_t *)owner;
    if (!f->pool.inline_run) {
        int old_state = PTHREAD_CANCEL_ENABLE;
        if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state) != 0
            || old_state != PTHREAD_CANCEL_DISABLE)
            atomic_store_explicit(&f->run_cancel_enabled, 1,
                                  memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&fake_slots(f)[i].runs, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&f->total_runs, 1, memory_order_relaxed);
}

static void fake_on_spawn(void *owner, int i, pthread_t thread)
{
    (void)i; (void)thread;
    ((fake_pool_t *)owner)->spawn_calls++;
}

static void fake_finalize(void *owner, int n_workers)
{
    ((fake_pool_t *)owner)->finalize_n = n_workers;
}

static const up_worker_pool_ops_t partial_ops = {
    .construct      = fake_construct,
    .run            = fake_run,
    .prepare        = fake_prepare,
    .release        = fake_release,
    .on_spawn       = fake_on_spawn,
    .finalize       = fake_finalize,
    .all_or_nothing = false,
};

static const up_worker_pool_ops_t strict_ops = {
    .construct      = fake_construct,
    .run            = fake_run,
    .prepare        = fake_prepare,
    .release        = fake_release,
    .on_spawn       = fake_on_spawn,
    .finalize       = fake_finalize,
    .all_or_nothing = true,
};

/* Minimal ops: only the two required hooks, so the optional-hook NULL guards
 * are exercised too (the USM pool ships without release/on_spawn). */
static const up_worker_pool_ops_t bare_ops = {
    .construct = fake_construct,
    .run       = fake_run,
};

static void fake_init(fake_pool_t *f, const up_worker_pool_ops_t *ops,
                      int n_pref)
{
    memset(f, 0, sizeof *f);
    f->construct_fail_at = -1;
    f->finalize_n        = -1;
    atomic_init(&f->total_runs, 0);
    atomic_init(&f->run_cancel_enabled, 0);
    up_worker_pool_config(&f->pool, ops, f, n_pref, sizeof(fake_worker_t));
    fault_reset();
}

static int fake_start_required(fake_pool_t *f, int expected_workers)
{
    const int start_rc = up_worker_pool_ensure_started(&f->pool);
    CHECK_EQ(start_rc, 0);
    if (start_rc != 0) return -1;

    const int workers = up_worker_pool_count(&f->pool);
    CHECK_EQ(workers, expected_workers);
    return workers == expected_workers ? 0 : -1;
}

static int fake_dispatch_required(fake_pool_t *f)
{
    const int rc = up_worker_pool_dispatch(&f->pool);
    CHECK_EQ(rc, 0);
    return rc;
}

static int mutex_roundtrip(pthread_mutex_t *mutex)
{
    const int lock_rc = pthread_mutex_trylock(mutex);
    CHECK_EQ(lock_rc, 0);
    if (lock_rc != 0) return -1;
    const int unlock_rc = pthread_mutex_unlock(mutex);
    CHECK_EQ(unlock_rc, 0);
    return unlock_rc;
}

/* Every worker of a successful dispatch ran exactly `expected` times. */
static void check_run_counts(fake_pool_t *f, int expected)
{
    const int n = up_worker_pool_count(&f->pool);
    for (int i = 0; i < n; i++)
        CHECK_EQ(atomic_load(&fake_slots(f)[i].runs), expected);
    CHECK_EQ(atomic_load(&f->total_runs), n * expected);
    CHECK_EQ(atomic_load(&f->run_cancel_enabled), 0);
}

/* ---------- tests ---------- */

static void test_threaded_dispatch_cycles(void)
{
    BEGIN("start N workers, M dispatches, each worker runs once per dispatch");
    enum { N = 4, M = 20 };
    fake_pool_t f;
    fake_init(&f, &partial_ops, N);

    if (fake_start_required(&f, N) != 0) {
        up_worker_pool_destroy(&f.pool);
        END();
        return;
    }
    CHECK_EQ(up_worker_pool_inline(&f.pool), 0);
    CHECK_EQ(f.prepare_calls, 1);
    CHECK_EQ(f.spawn_calls, N);
    CHECK_EQ(f.finalize_n, N);

    for (int i = 0; i < M; i++) {
        if (fake_dispatch_required(&f) != 0) {
            up_worker_pool_destroy(&f.pool);
            END();
            return;
        }
    }
    check_run_counts(&f, M);
    CHECK_EQ(up_worker_pool_broken(&f.pool), 0);

    /* Already started: ensure is a no-op, prepare does not run twice. */
    CHECK_EQ(up_worker_pool_ensure_started(&f.pool), 0);
    CHECK_EQ(f.prepare_calls, 1);

    up_worker_pool_destroy(&f.pool);
    CHECK_EQ(f.release_calls, N);
    END();
}

/* PERF-1: a one-worker pool spawns no thread and needs no gate — the work
 * runs on the calling thread. The create wrapper proves it directly. */
static void test_single_worker_runs_inline(void)
{
    BEGIN("single worker: no thread spawned, dispatch runs on the caller");
    fake_pool_t f;
    fake_init(&f, &partial_ops, 1);

    if (fake_start_required(&f, 1) != 0) {
        up_worker_pool_destroy(&f.pool);
        END();
        return;
    }
    CHECK_EQ(up_worker_pool_inline(&f.pool), 1);
    CHECK_EQ(f.spawn_calls, 0);
    CHECK_EQ(atomic_load(&g_thread_create_calls), 0);

    if (fake_dispatch_required(&f) != 0) {
        up_worker_pool_destroy(&f.pool);
        END();
        return;
    }
    check_run_counts(&f, 1);
    CHECK_EQ(up_pool_gate_ready(&f.pool.gate), 0);   /* no gate at all */

    up_worker_pool_destroy(&f.pool);
    CHECK_EQ(atomic_load(&g_thread_create_calls), 0);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 0);
    END();
}

/* The USM pool's policy: whatever came up is usable, and the finalize hook
 * repartitions the work over exactly that count. */
static void test_partial_spawn_is_usable(void)
{
    BEGIN("partial spawn: pool runs with the workers that came up");
    fake_pool_t f;
    fake_init(&f, &partial_ops, 6);
    atomic_store(&g_spawn_budget, 2);   /* the 3rd pthread_create fails */

    if (fake_start_required(&f, 2) != 0) {
        up_worker_pool_destroy(&f.pool);
        END();
        return;
    }
    CHECK_EQ(f.finalize_n, 2);
    /* The slot whose thread failed to spawn was constructed, then released. */
    CHECK_EQ(f.construct_calls, 3);
    CHECK_EQ(f.release_calls, 1);

    if (fake_dispatch_required(&f) != 0) {
        up_worker_pool_destroy(&f.pool);
        END();
        return;
    }
    check_run_counts(&f, 1);

    up_worker_pool_destroy(&f.pool);
    CHECK_EQ(f.release_calls, 3);   /* the 2 live slots, plus the failed one */
    END();
}

/* The zimg grid's policy: a missing cell would leave part of the frame
 * unwritten, so a partial spawn must fail the pool outright — and every slot
 * that did come up must be released, not leaked. */
static void test_all_or_nothing_rejects_partial_spawn(void)
{
    BEGIN("all-or-nothing: a failed spawn fails the pool and releases slots");
    fake_pool_t f;
    fake_init(&f, &strict_ops, 4);
    atomic_store(&g_spawn_budget, 2);

    CHECK_EQ(up_worker_pool_ensure_started(&f.pool), -1);
    CHECK_EQ(up_worker_pool_count(&f.pool), 0);
    CHECK_EQ(f.finalize_n, -1);          /* never reached */
    CHECK_EQ(f.release_calls, 3);        /* 2 spawned + the one that didn't */
    CHECK_EQ(up_worker_pool_started(&f.pool), 0);
    CHECK_EQ(up_worker_pool_failed(&f.pool), 1);

    /* Sticky: a failed start is never retried (it would re-allocate and
     * re-spawn on every frame). */
    CHECK_EQ(up_worker_pool_ensure_started(&f.pool), -1);
    CHECK_EQ(f.prepare_calls, 1);

    up_worker_pool_destroy(&f.pool);
    CHECK_EQ(f.release_calls, 3);        /* no double release on teardown */
    END();
}

static void test_destroy_quarantines_unreaped_worker(void)
{
    BEGIN("join failure: destroy retains every worker-accessible allocation");
    fake_pool_t f;
    fake_init(&f, &partial_ops, 2);

    if (fake_start_required(&f, 2) != 0) {
        (void)up_worker_pool_destroy(&f.pool);
        END();
        return;
    }

    atomic_store(&g_fail_join_nth, 1);
    CHECK_EQ(up_worker_pool_destroy(&f.pool), -1);
    CHECK_EQ(up_worker_pool_broken(&f.pool), 1);
    CHECK_EQ(up_worker_pool_count(&f.pool), 2);
    CHECK(f.pool.workers != NULL);
    CHECK(f.pool.threads != NULL);
    CHECK_EQ(f.pool.threads[0].started, 1);
    CHECK_EQ(f.pool.threads[1].started, 0);
    CHECK_EQ(f.release_calls, 0);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 1);

    atomic_store(&g_fail_join_nth, 0);
    CHECK_EQ(up_worker_pool_destroy(&f.pool), 0);
    CHECK_EQ(f.release_calls, 2);
    CHECK(f.pool.workers == NULL);
    CHECK(f.pool.threads == NULL);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 2);
    END();
}

static void test_failed_start_quarantines_unreaped_worker(void)
{
    BEGIN("partial start join failure: built slots survive until retry");
    fake_pool_t f;
    fake_init(&f, &strict_ops, 4);
    atomic_store(&g_spawn_budget, 2);
    atomic_store(&g_fail_join_nth, 1);

    CHECK_EQ(up_worker_pool_ensure_started(&f.pool), -1);
    CHECK_EQ(up_worker_pool_failed(&f.pool), 1);
    CHECK_EQ(up_worker_pool_count(&f.pool), 2);
    CHECK_EQ(f.release_calls, 1);
    CHECK(f.pool.workers != NULL);
    CHECK(f.pool.threads != NULL);
    CHECK_EQ(f.pool.threads[0].started, 1);
    CHECK_EQ(f.pool.threads[1].started, 0);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 1);

    atomic_store(&g_fail_join_nth, 0);
    CHECK_EQ(up_worker_pool_destroy(&f.pool), 0);
    CHECK_EQ(f.release_calls, 3);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 2);
    END();
}

static void test_construct_failure(void)
{
    BEGIN("construct failure: slot cleans up itself, earlier slots released");
    fake_pool_t f;
    fake_init(&f, &strict_ops, 4);
    f.construct_fail_at = 2;

    CHECK_EQ(up_worker_pool_ensure_started(&f.pool), -1);
    CHECK_EQ(f.construct_calls, 3);
    /* Slots 0 and 1 only: the pool never releases a slot construct() rejected
     * — that one cleans up after itself. */
    CHECK_EQ(f.release_calls, 2);
    up_worker_pool_destroy(&f.pool);
    END();
}

/* A pool where NOTHING comes up is unusable under either policy. */
static void test_no_worker_at_all(void)
{
    BEGIN("zero workers constructed: start fails under the partial policy too");
    fake_pool_t f;
    fake_init(&f, &partial_ops, 4);
    f.construct_fail_at = 0;

    CHECK_EQ(up_worker_pool_ensure_started(&f.pool), -1);
    CHECK_EQ(up_worker_pool_count(&f.pool), 0);
    CHECK_EQ(f.release_calls, 0);
    up_worker_pool_destroy(&f.pool);
    END();
}

static void test_prepare_failure(void)
{
    BEGIN("prepare failure: no slot array, no thread, sticky");
    fake_pool_t f;
    fake_init(&f, &partial_ops, 4);
    f.prepare_rc = -1;

    CHECK_EQ(up_worker_pool_ensure_started(&f.pool), -1);
    CHECK_EQ(f.construct_calls, 0);
    CHECK_EQ(atomic_load(&g_aligned_alloc_calls), 0);
    up_worker_pool_destroy(&f.pool);
    END();
}

/* MEM-1: both allocations the pool makes (worker slots, thread records) must
 * fail cleanly — not half-build a pool. */
static void test_alloc_failure(void)
{
    BEGIN("slot / thread-record allocation failure fails the start cleanly");
    for (int nth = 1; nth <= 2; nth++) {
        fake_pool_t f;
        fake_init(&f, &partial_ops, 4);
        atomic_store(&g_fail_aligned_alloc_nth, nth);

        CHECK_EQ(up_worker_pool_ensure_started(&f.pool), -1);
        CHECK_EQ(f.construct_calls, 0);
        CHECK_EQ(up_worker_pool_count(&f.pool), 0);
        up_worker_pool_destroy(&f.pool);
    }
    END();
}

static void test_worker_count_bounds(void)
{
    BEGIN("worker count outside [1, UP_THREADS_MAX] is rejected");
    fake_pool_t f;

    fake_init(&f, &partial_ops, 0);
    CHECK_EQ(up_worker_pool_ensure_started(&f.pool), -1);
    CHECK_EQ(f.prepare_calls, 0);
    up_worker_pool_destroy(&f.pool);

    fake_init(&f, &partial_ops, UP_THREADS_MAX + 1);
    CHECK_EQ(up_worker_pool_ensure_started(&f.pool), -1);
    CHECK_EQ(f.prepare_calls, 0);
    up_worker_pool_destroy(&f.pool);
    END();
}

/* The optional hooks are genuinely optional: a pool with only construct+run
 * must dispatch and tear down without dereferencing a NULL hook. */
static void test_bare_ops(void)
{
    BEGIN("pool with only the required hooks dispatches and destroys");
    fake_pool_t f;
    fake_init(&f, &bare_ops, 3);

    if (fake_start_required(&f, 3) != 0) {
        up_worker_pool_destroy(&f.pool);
        END();
        return;
    }
    if (fake_dispatch_required(&f) != 0) {
        up_worker_pool_destroy(&f.pool);
        END();
        return;
    }
    check_run_counts(&f, 1);
    CHECK_EQ(f.spawn_calls, 0);
    up_worker_pool_destroy(&f.pool);
    CHECK_EQ(f.release_calls, 0);
    END();
}

/*
 * REGRESSION (ERR-1 / CON-2 / RES-2): a pthread gate failure must poison the pool
 * AND stop its threads. Leaving up to UP_THREADS_MAX workers parked on the gate
 * for the rest of playback — holding their graphs and scratch while every later
 * frame short-circuits — is the retention-after-permanent-failure this pool
 * exists to prevent. Exercise both checked pthread failures on the dispatch
 * path: taking the gate lock and broadcasting the armed generation.
 */
static int check_gate_failure_poisons_and_stops(void (*inject)(void))
{
    fake_pool_t f;
    fake_init(&f, &partial_ops, 3);
    if (fake_start_required(&f, 3) != 0) {
        up_worker_pool_destroy(&f.pool);
        return -1;
    }
    CHECK_EQ(atomic_load(&g_thread_create_calls), 3);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 0);

    inject();
    CHECK_EQ(up_worker_pool_dispatch(&f.pool), -1);
    CHECK_EQ(barrier_fault_injection_consumed(), 1);
    CHECK_EQ(up_worker_pool_broken(&f.pool), 1);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 3);

    /* Poison is idempotent — Close() runs it again through destroy. */
    up_worker_pool_poison(&f.pool);
    up_worker_pool_destroy(&f.pool);
    CHECK_EQ(atomic_load(&g_thread_create_calls), 3);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 3);
    CHECK_EQ(f.release_calls, 3);
    return 0;
}

static void test_gate_failures_poison_and_stop(void)
{
    BEGIN("pthread gate failures: dispatch fails, pool poisoned, threads joined");
    if (check_gate_failure_poisons_and_stops(
            barrier_fault_inject_next_mutex_lock) != 0) {
        END();
        return;
    }
    (void)check_gate_failure_poisons_and_stops(
        barrier_fault_inject_next_dispatch);
    END();
}

/* The failure can occur inside stop itself, after workers are already asleep.
 * Neither a failed lock nor a failed broadcast may strand the subsequent join. */
static int check_exit_failure_cancels_and_stops(void (*inject)(void))
{
    fake_pool_t f;
    fake_init(&f, &partial_ops, 3);
    if (fake_start_required(&f, 3) != 0) {
        up_worker_pool_destroy(&f.pool);
        return -1;
    }
    CHECK_EQ(atomic_load(&g_thread_create_calls), 3);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 0);
    inject();
    up_worker_pool_poison(&f.pool);

    CHECK_EQ(barrier_fault_injection_consumed(), 1);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 3);
    if (mutex_roundtrip(&f.pool.gate.lock) != 0) return -1;
    up_worker_pool_destroy(&f.pool);
    CHECK_EQ(atomic_load(&g_thread_create_calls), 3);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 3);
    CHECK_EQ(f.release_calls, 3);
    return 0;
}

static void test_exit_failures_cancel_and_stop(void)
{
    BEGIN("pthread exit failures: cancellation prevents stuck joins");
    if (check_exit_failure_cancels_and_stops(
            barrier_fault_inject_next_mutex_lock) != 0) {
        END();
        return;
    }
    (void)check_exit_failure_cancels_and_stops(
        barrier_fault_inject_next_dispatch);
    END();
}

static int stop_direct_worker(up_worker_pool_t *pool,
                              up_pool_thread_t *worker)
{
    const int exit_rc = up_pool_gate_request_exit(&pool->gate);
    CHECK_EQ(exit_rc, 0);
    if (exit_rc != 0) {
        const int cancel_rc = pthread_cancel(worker->thread);
        CHECK_EQ(cancel_rc, 0);
        if (cancel_rc != 0) return -1;
    }

    const int join_rc = pthread_join(worker->thread, NULL);
    CHECK_EQ(join_rc, 0);
    if (join_rc == 0) worker->started = false;
    return join_rc;
}

static int check_worker_cancel_state_failure(int state)
{
    up_worker_pool_t pool = {0};
    up_pool_thread_t worker = { .pool = &pool };
    const int init_rc = up_pool_gate_init(&pool.gate);
    CHECK_EQ(init_rc, 0);
    if (init_rc != 0) return -1;

    atomic_store(&g_fail_cancel_state, state);
    const int create_rc = pthread_create(&worker.thread, NULL,
                                         up__pool_worker_main, &worker);
    CHECK_EQ(create_rc, 0);
    if (create_rc != 0) {
        atomic_store(&g_fail_cancel_state, -1);
        up_pool_gate_destroy(&pool.gate);
        return -1;
    }
    worker.started = true;
    if (stop_direct_worker(&pool, &worker) != 0) return -1;

    CHECK_EQ(atomic_load(&g_fail_cancel_state), -1);
    CHECK_EQ(atomic_load(&pool.gate.sync_failed), 1);

    up_pool_gate_destroy(&pool.gate);
    return 0;
}

static void test_worker_cancel_state_failures(void)
{
    BEGIN("worker cancellation-state failures break the pool gate");
    fault_reset();
    if (check_worker_cancel_state_failure(PTHREAD_CANCEL_DISABLE) != 0) {
        END();
        return;
    }
    if (check_worker_cancel_state_failure(PTHREAD_CANCEL_ENABLE) != 0) {
        END();
        return;
    }
    CHECK_EQ(atomic_load(&g_thread_create_calls), 2);
    CHECK_EQ(atomic_load(&g_thread_join_calls), 2);
    END();
}

/* A configured-but-never-started pool is what a VLC filter-chain probe leaves
 * behind: destroy must not touch the gate, the slots or the hooks. */
static void test_destroy_without_start(void)
{
    BEGIN("destroy on a never-started pool is a no-op");
    fake_pool_t f;
    fake_init(&f, &partial_ops, 4);
    up_worker_pool_destroy(&f.pool);
    CHECK_EQ(f.prepare_calls, 0);
    CHECK_EQ(f.release_calls, 0);
    CHECK_EQ(atomic_load(&g_aligned_alloc_calls), 0);
    END();
}

int main(void)
{
    printf("Running worker-pool lifecycle tests...\n");

    test_threaded_dispatch_cycles();
    test_single_worker_runs_inline();
    test_partial_spawn_is_usable();
    test_all_or_nothing_rejects_partial_spawn();
    test_destroy_quarantines_unreaped_worker();
    test_failed_start_quarantines_unreaped_worker();
    test_construct_failure();
    test_no_worker_at_all();
    test_prepare_failure();
    test_alloc_failure();
    test_worker_count_bounds();
    test_bare_ops();
    test_gate_failures_poison_and_stop();
    test_exit_failures_cancel_and_stop();
    test_worker_cancel_state_failures();
    test_destroy_without_start();

    return test_harness_report();
}
