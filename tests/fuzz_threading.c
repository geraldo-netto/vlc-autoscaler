// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_threading.c - libFuzzer/smoke target for up_threads_decide
 *****************************************************************************
 * Pure-logic fuzzer. Reads (user_pref, total_cores) pairs from the fuzz
 * input and verifies invariants of up_threads_decide:
 *
 *  - Result is always >= 1.
 *  - Result is always <= UP_THREADS_MAX.
 *  - Result is always <= max(total_cores, 1).
 *  - When user_pref == UP_THREADS_AUTO and total_cores >= 1, result equals
 *    clamp(total_cores/2 - 2, 1, UP_THREADS_MAX), but never exceeding
 *    total_cores.
 *  - When user_pref > 0, result is min(user_pref, total_cores, UP_THREADS_MAX).
 *
 * Two entry points: LLVMFuzzerTestOneInput (libFuzzer) + a deterministic
 * smoke-fuzz main() for CI.
 *****************************************************************************/

#include "../src/threading.h"
#include "cli_parse.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static void topology_mask_from_bytes(cpu_set_t *set,
                                     const uint8_t *data, size_t size)
{
    CPU_ZERO(set);
    size_t limit = size;
    if (limit > CPU_SETSIZE / 8) limit = CPU_SETSIZE / 8;
    for (size_t byte = 0; byte < limit; byte++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            if ((data[byte] & (uint8_t)(1u << bit)) != 0)
                CPU_SET((int)(byte * 8 + bit), set);
        }
    }
}

static void topology_expected_counts(const cpu_set_t *set,
                                     int *allowed, int *pins)
{
    *allowed = 0;
    *pins = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, set)) continue;
        if (*allowed < UP_CPU_COUNT_MAX) (*allowed)++;
        if (*pins < UP_THREADS_MAX) (*pins)++;
    }
}

static int topology_ids_valid(const up_cpu_topology_t *topology,
                              const cpu_set_t *set)
{
    int previous = -1;
    for (int i = 0; i < topology->pin_count; i++) {
        int cpu = topology->pin_ids[i];
        if (cpu <= previous || !CPU_ISSET(cpu, set)) return 0;
        previous = cpu;
    }
    return 1;
}

static int check_topology_mask(const uint8_t *data, size_t size)
{
    cpu_set_t set;
    topology_mask_from_bytes(&set, data, size);

    up_cpu_topology_t topology;
    up_cpu_topology_from_set(&topology, &set, sizeof set);
    int expected_allowed, expected_pins;
    topology_expected_counts(&set, &expected_allowed, &expected_pins);
    if (topology.allowed_count != expected_allowed ||
        topology.pin_count != expected_pins) {
        fprintf(stderr, "FAIL: topology counts %d/%d, expected %d/%d\n",
                topology.allowed_count, topology.pin_count,
                expected_allowed, expected_pins);
        return 1;
    }
    if (!topology_ids_valid(&topology, &set)) {
        fprintf(stderr, "FAIL: topology pin IDs are not sparse-mask members\n");
        return 1;
    }
    return 0;
}

/* Parse one trial (int user_pref + int64_t total_cores). Returns 1 on
 * success, 0 if not enough bytes left. Advances *data and *size. */
static int parse_trial(const uint8_t **data, size_t *size,
                       int *user_pref, int64_t *total_cores_64)
{
    if (*size < sizeof(int) + sizeof(int64_t)) return 0;
    memcpy(user_pref, *data, sizeof *user_pref);
    *data += sizeof *user_pref;
    memcpy(total_cores_64, *data, sizeof *total_cores_64);
    *data += sizeof *total_cores_64;
    *size -= sizeof *user_pref + sizeof *total_cores_64;
    return 1;
}

/* Narrow int64_t to int with saturation (mirrors production: up_detect_cores
 * clamps before passing to up_threads_decide). */
static int saturate_cores(int64_t total_cores_64)
{
    if (total_cores_64 > INT_MAX) return INT_MAX;
    if (total_cores_64 < INT_MIN) return INT_MIN;
    return (int)total_cores_64;
}

/* Bound checks common to both paths. Returns 1 on failure. */
static int check_bounds(int n, int user_pref, int cores, int effective_cores)
{
    if (n < 1) {
        fprintf(stderr, "FAIL: n=%d < 1 (user_pref=%d cores=%d)\n",
                n, user_pref, cores);
        return 1;
    }
    if (n > UP_THREADS_MAX) {
        fprintf(stderr, "FAIL: n=%d > MAX=%d (user_pref=%d cores=%d)\n",
                n, UP_THREADS_MAX, user_pref, cores);
        return 1;
    }
    if (n > effective_cores) {
        fprintf(stderr, "FAIL: n=%d > cores=%d (user_pref=%d)\n",
                n, effective_cores, user_pref);
        return 1;
    }
    return 0;
}

/* Auto path: should equal clamp(cores/2 - 2, 1, MAX), then further
 * clamped to <= effective_cores. Returns 1 on failure. */
static int check_auto(int n, int cores, int effective_cores)
{
    long expected = (long)effective_cores / 2 - 2;
    if (expected < 1) expected = 1;
    if (expected > UP_THREADS_MAX) expected = UP_THREADS_MAX;
    if (expected > effective_cores) expected = effective_cores;
    if ((long)n != expected) {
        fprintf(stderr,
            "FAIL: auto on %d cores got n=%d, expected %ld\n",
            cores, n, expected);
        return 1;
    }
    return 0;
}

/* Explicit path: should be min(user_pref, cores, MAX). Returns 1 on failure. */
static int check_explicit(int n, int user_pref, int cores, int effective_cores)
{
    long expected = user_pref;
    if (expected > effective_cores)  expected = effective_cores;
    if (expected > UP_THREADS_MAX)   expected = UP_THREADS_MAX;
    if (expected < 1)                expected = 1;
    if ((long)n != expected) {
        fprintf(stderr,
            "FAIL: explicit %d on %d cores got n=%d, expected %ld\n",
            user_pref, cores, n, expected);
        return 1;
    }
    return 0;
}

/* Run one trial end-to-end. Returns 1 on failure. */
static int run_trial(int user_pref, int64_t total_cores_64)
{
    int cores = saturate_cores(total_cores_64);
    int n = up_threads_decide(user_pref, cores);
    int effective_cores = (cores < 1) ? 1 : cores;

    if (check_bounds(n, user_pref, cores, effective_cores)) return 1;
    if (user_pref <= UP_THREADS_AUTO &&
        check_auto(n, cores, effective_cores)) return 1;
    if (user_pref > UP_THREADS_AUTO &&
        check_explicit(n, user_pref, cores, effective_cores)) return 1;
    return 0;
}

static int run_one(const uint8_t *data, size_t size)
{
    /* Each trial consumes 12 bytes: int user_pref + int64_t total_cores.
     * We accept int64_t for fuzz coverage of overflow-y values, then
     * narrow to int for the call (matching what up_detect_cores does
     * in production). */
    if (check_topology_mask(data, size)) return 1;

    int user_pref;
    int64_t total_cores_64;
    while (parse_trial(&data, &size, &user_pref, &total_cores_64)) {
        if (run_trial(user_pref, total_cores_64)) return 1;
    }
    return 0;
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
    uint8_t buf[256];
    size_t n = (size_t)(fuzz_smoke_next() % sizeof buf);
    fuzz_smoke_fill(buf, n);
    return run_one(buf, n);
}

int main(int argc, char **argv)
{
    fuzz_smoke_seed(0x1234567890abcdefULL);
    return fuzz_smoke_main(argc, argv, 100000, "threading", smoke_iter);
}
#endif
