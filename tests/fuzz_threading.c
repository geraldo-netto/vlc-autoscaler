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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int run_one(const uint8_t *data, size_t size)
{
    /* Each trial consumes 12 bytes: int user_pref + int64_t total_cores.
     * We accept int64_t for fuzz coverage of overflow-y values, then
     * narrow to int for the call (matching what up_detect_cores does
     * in production). */
    while (size >= sizeof(int) + sizeof(int64_t)) {
        int user_pref;
        int64_t total_cores_64;
        memcpy(&user_pref, data, sizeof user_pref);
        data += sizeof user_pref;
        memcpy(&total_cores_64, data, sizeof total_cores_64);
        data += sizeof total_cores_64;
        size -= sizeof user_pref + sizeof total_cores_64;

        /* Narrow to int with saturation (mirrors production: up_detect_cores
         * clamps before passing to up_threads_decide). */
        int cores;
        if (total_cores_64 > INT_MAX) cores = INT_MAX;
        else if (total_cores_64 < INT_MIN) cores = INT_MIN;
        else cores = (int)total_cores_64;

        int n = up_threads_decide(user_pref, cores);

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
        int effective_cores = (cores < 1) ? 1 : cores;
        if (n > effective_cores) {
            fprintf(stderr, "FAIL: n=%d > cores=%d (user_pref=%d)\n",
                    n, effective_cores, user_pref);
            return 1;
        }

        /* Auto path: should equal clamp(cores/2 - 2, 1, MAX), then
         * further clamped to <= effective_cores. We compute the same
         * value here in long to be safe against any cast oddity. */
        if (user_pref <= UP_THREADS_AUTO) {
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
        }
        /* Explicit path: should be min(user_pref, cores, MAX). */
        if (user_pref > UP_THREADS_AUTO) {
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
        }
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return run_one(data, size) ? 1 : 0;
}

#ifdef FUZZ_MAIN
static uint64_t xs_state = 0x1234567890abcdefULL;
static uint64_t xs(void)
{
    uint64_t x = xs_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return xs_state = x;
}

int main(int argc, char **argv)
{
    long iters = (argc > 1) ? atol(argv[1]) : 100000;
    uint8_t buf[256];
    long n_fail = 0;
    for (long i = 0; i < iters; i++) {
        size_t n = (size_t)(xs() % sizeof(buf));
        for (size_t j = 0; j < n; j++) buf[j] = (uint8_t)xs();
        if (run_one(buf, n)) n_fail++;
    }
    if (n_fail) {
        fprintf(stderr, "threading smoke FAIL: %ld/%ld trials\n",
                n_fail, iters);
        return 1;
    }
    fprintf(stderr, "threading smoke OK: %ld iterations\n", iters);
    return 0;
}
#endif
