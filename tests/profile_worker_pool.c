// SPDX-License-Identifier: GPL-2.0-or-later
#include "profile_internal.h"
#include "cli_parse.h"
#include <sys/resource.h>

typedef struct { alignas(64) uint8_t value; } empty_slot_t;
typedef struct {
    up_worker_pool_t pool;
    up_cpu_topology_t topology;
    int pin, pin_failed;
} empty_pool_t;

typedef struct { double wall; up_profile_frame_t trace; } empty_sample_t;

static int construct(void *owner, int i) { (void)owner; (void)i; return 0; }

static void empty_run(void *owner, int i)
{
    empty_pool_t *p = owner;
    empty_slot_t *slot = up_worker_pool_slot(&p->pool, i);
    slot->value++;
}

static void pin_worker(void *owner, int i, pthread_t thread)
{
    empty_pool_t *p = owner;
    if (!p->pin) return;
    if (!p->topology.pin_count) { p->pin_failed = 1; return; }
    const int cpu = p->topology.pin_ids[i % p->topology.pin_count];
    if (cpu < 0 || cpu >= CPU_SETSIZE) { p->pin_failed = 1; return; }
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (pthread_setaffinity_np(thread, sizeof mask, &mask)) p->pin_failed = 1;
}

static const up_worker_pool_ops_t ops = {
    .construct = construct, .run = empty_run, .on_spawn = pin_worker,
    .all_or_nothing = true,
};

static int dispatch_many(empty_pool_t *p, int frames, empty_sample_t *samples)
{
    for (int i = 0; i < frames; i++) {
        const double start = up_profile_clock(CLOCK_MONOTONIC);
        if (up_profile_dispatch(&p->pool)) return -1;
        if (samples) samples[i] = (empty_sample_t){
            up_profile_clock(CLOCK_MONOTONIC) - start, up_profile_trace.frame };
    }
    return 0;
}

static int check_slots(const empty_pool_t *p, int frames)
{
    for (int i = 0; i < up_worker_pool_count(&p->pool); i++) {
        const empty_slot_t *s = up_worker_pool_slot(&p->pool, i);
        if (s->value != (uint8_t)(frames + 128)) return -1;
    }
    return 0;
}

static void print_samples(const empty_sample_t *samples, int n)
{
    printf("\"samples\":[");
    for (int i = 0; i < n; i++) {
        const up_profile_frame_t *t = &samples[i].trace;
        printf("%s[%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f]",
               i ? "," : "", samples[i].wall, t->dispatch_us, t->first_start_us,
               t->last_start_us, t->min_work_us, t->max_work_us, t->mean_work_us,
               t->handoff_us, t->worker_cpu_us);
    }
    printf("]}\n");
}

static int measure(empty_pool_t *p, int frames, empty_sample_t *samples)
{
    if (dispatch_many(p, 128, NULL)) return -1;
    up_profile_sched_t before = {0}, after = {0};
    if (up_profile_pool_sched(&p->pool, &before)) return -1;
    struct rusage r0, r1;
    if (getrusage(RUSAGE_SELF, &r0)) return -1;
    const double cpu = up_profile_clock(CLOCK_PROCESS_CPUTIME_ID);
    const double start = up_profile_clock(CLOCK_MONOTONIC);
    if (dispatch_many(p, frames, samples)) return -1;
    const double elapsed = up_profile_clock(CLOCK_MONOTONIC) - start;
    const double cpu_us = up_profile_clock(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    if (getrusage(RUSAGE_SELF, &r1)) return -1;
    if (up_profile_pool_sched(&p->pool, &after)) return -1;
    if (check_slots(p, frames)) return -1;
    printf("{\"elapsed_us\":%.6f,\"cpu_us\":%.6f,\"nvcsw\":%ld,\"nivcsw\":%ld,"
           "\"runtime_ns\":%llu,\"runqueue_ns\":%llu,\"slices\":%llu,",
           elapsed, cpu_us, r1.ru_nvcsw - r0.ru_nvcsw, r1.ru_nivcsw - r0.ru_nivcsw,
           (unsigned long long)(after.runtime_ns - before.runtime_ns),
           (unsigned long long)(after.runqueue_ns - before.runqueue_ns),
           (unsigned long long)(after.slices - before.slices));
    print_samples(samples, frames);
    return ferror(stdout) ? -1 : 0;
}

static int parse(int argc, char **argv, long *values)
{
    if (argc != 5) return -1;
    const long low[] = {1, 1, 0, 0}, high[] = {64, 1000000, 1, 2};
    for (int i = 0; i < 4; i++)
        if (!up_cli_parse_long(argv[i + 1], low[i], high[i], &values[i])) return -1;
    return 0;
}

int main(int argc, char **argv)
{
    long values[4];
    if (parse(argc, argv, values)) {
        fprintf(stderr, "usage: %s workers frames pin detail\n", argv[0]);
        return 2;
    }
    empty_pool_t p = { .pin = (int)values[2] };
    up_detect_cpu_topology(&p.topology);
    up_profile_trace.mode = (int)values[3];
    up_worker_pool_config(&p.pool, &ops, &p, (int)values[0], sizeof(empty_slot_t));
    empty_sample_t *samples = calloc((size_t)values[1], sizeof *samples);
    int rc = samples ? up_worker_pool_ensure_started(&p.pool) : -1;
    if (!rc && !p.pin_failed) rc = measure(&p, (int)values[1], samples);
    free(samples);
    const int retired = up_worker_pool_destroy(&p.pool);
    return rc || retired || p.pin_failed ? 1 : 0;
}
