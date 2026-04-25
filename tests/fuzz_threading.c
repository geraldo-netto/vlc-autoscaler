/*****************************************************************************
 * fuzz_threading.c - libFuzzer/smoke target for up_threads_decide
 *****************************************************************************
 * Pure-logic fuzzer. Reads (user_pref, total_cores) pairs from the fuzz
 * input and verifies invariants of up_threads_decide:
 *
 *  - Result is always >= 1.
 *  - Result is always <= UP_THREADS_MAX.
 *  - Result is always <= max(total_cores, 1).
 *  - When user_pref == UP_THREADS_AUTO and total_cores >= 3, result equals
 *    min(total_cores - 2, UP_THREADS_MAX).
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

static int run_one(const uint8_t *data, size_t size)
{
    /* Each trial consumes 12 bytes: int user_pref + int64_t total_cores. */
    while (size >= sizeof(int) + sizeof(int64_t)) {
        int user_pref;
        int64_t total_cores;
        memcpy(&user_pref, data, sizeof user_pref);
        data += sizeof user_pref;
        memcpy(&total_cores, data, sizeof total_cores);
        data += sizeof total_cores;
        size -= sizeof user_pref + sizeof total_cores;

        long cores = (long)total_cores;
        int n = up_threads_decide(user_pref, cores);

        if (n < 1) {
            fprintf(stderr, "FAIL: n=%d < 1 (user_pref=%d cores=%ld)\n",
                    n, user_pref, cores);
            return 1;
        }
        if (n > UP_THREADS_MAX) {
            fprintf(stderr, "FAIL: n=%d > MAX=%d (user_pref=%d cores=%ld)\n",
                    n, UP_THREADS_MAX, user_pref, cores);
            return 1;
        }
        long effective_cores = (cores < 1) ? 1 : cores;
        if ((long)n > effective_cores) {
            fprintf(stderr, "FAIL: n=%d > cores=%ld (user_pref=%d)\n",
                    n, effective_cores, user_pref);
            return 1;
        }

        /* Auto path with healthy core count: should equal cores-2 (or MAX). */
        if (user_pref <= UP_THREADS_AUTO && cores >= 3) {
            long expected = cores - 2;
            if (expected > UP_THREADS_MAX) expected = UP_THREADS_MAX;
            if ((long)n != expected) {
                fprintf(stderr,
                    "FAIL: auto on %ld cores got n=%d, expected %ld\n",
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
                    "FAIL: explicit %d on %ld cores got n=%d, expected %ld\n",
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
