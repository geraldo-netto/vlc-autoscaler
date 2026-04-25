/*****************************************************************************
 * fuzz_perfmon.c - libFuzzer/smoke target for up_perfmon_*
 *****************************************************************************
 * Feeds a random sequence of frame-time samples (ns) into a perfmon, and
 * checks the invariants we care about:
 *
 *  - Every call returns 0 or 1 (no other value).
 *  - The "should warn now" return value is 1 at most ONCE per perfmon
 *    lifetime. After it fires, all subsequent calls return 0.
 *  - up_perfmon_ewma_us() never returns negative.
 *  - up_perfmon_budget_us() is constant after init for a given target_fps.
 *
 * Two entry points: LLVMFuzzerTestOneInput (libFuzzer) + a deterministic
 * smoke-fuzz main() that just iterates with a PRNG, used in CI when we
 * don't want to pull in libFuzzer.
 *
 * Build (smoke):
 *   cc -O2 -fsanitize=address -fsanitize=undefined -DFUZZ_MAIN \
 *      -I src tests/fuzz_perfmon.c -o build/fuzz_perfmon_smoke
 * Build (libFuzzer):
 *   clang -O2 -g -fsanitize=address,undefined,fuzzer \
 *      -I src tests/fuzz_perfmon.c -o build/fuzz_perfmon
 *****************************************************************************/

#include "../src/perfmon.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run one trial: parse the fuzz input as a sequence of (target_fps, then
 * a stream of int64_t-encoded sample ns values). Verify invariants. */
static int run_one(const uint8_t *data, size_t size)
{
    /* Need at least target_fps + 1 sample. */
    if (size < sizeof(int) + sizeof(int64_t)) return 0;

    int target_fps;
    memcpy(&target_fps, data, sizeof target_fps);
    data += sizeof target_fps;
    size -= sizeof target_fps;

    /* Constrain to a sane range so the budget arithmetic doesn't overflow.
     * The plugin enforces 0..240 anyway; fuzz a slightly wider band to
     * exercise edge cases. */
    if (target_fps < 0)    target_fps = -target_fps;  /* abs */
    if (target_fps > 1000) target_fps = target_fps % 1001;

    up_perfmon_t pm;
    up_perfmon_init(&pm, target_fps);

    int64_t init_budget = up_perfmon_budget_us(&pm);

    /* Initial EWMA is non-negative. */
    if (up_perfmon_ewma_us(&pm) < 0) return 1;

    int warned_count = 0;
    int n_samples = 0;
    while (size >= sizeof(int64_t)) {
        int64_t sample;
        memcpy(&sample, data, sizeof sample);
        data += sizeof sample;
        size -= sizeof sample;

        int rc = up_perfmon_record_ns(&pm, sample);
        n_samples++;

        if (rc != 0 && rc != 1) {
            fprintf(stderr, "FAIL: rc=%d (must be 0 or 1)\n", rc);
            return 1;
        }
        if (rc == 1) warned_count++;
        if (warned_count > 1) {
            fprintf(stderr, "FAIL: warned %d times (must be at most 1)\n",
                    warned_count);
            return 1;
        }
        if (up_perfmon_ewma_us(&pm) < 0) {
            fprintf(stderr, "FAIL: ewma_us<0 after %d samples\n", n_samples);
            return 1;
        }
        if (up_perfmon_budget_us(&pm) != init_budget) {
            fprintf(stderr, "FAIL: budget changed after sample %d\n", n_samples);
            return 1;
        }
    }
    return 0;
}

#ifdef __AFL_HAVE_MANUAL_CONTROL
__AFL_FUZZ_INIT();
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return run_one(data, size) ? 1 : 0;
}

#ifdef FUZZ_MAIN
/* Deterministic smoke-fuzz entry: PRNG-driven for N iterations. */
static uint64_t xs_state = 0xdeadbeefcafebabeULL;
static uint64_t xs(void)
{
    uint64_t x = xs_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return xs_state = x;
}

int main(int argc, char **argv)
{
    long iters = (argc > 1) ? atol(argv[1]) : 50000;
    uint8_t buf[4096];
    long n_fail = 0;
    for (long i = 0; i < iters; i++) {
        size_t n = (size_t)(xs() % sizeof(buf));
        for (size_t j = 0; j < n; j++) buf[j] = (uint8_t)xs();
        if (run_one(buf, n)) n_fail++;
    }
    if (n_fail) {
        fprintf(stderr, "perfmon smoke FAIL: %ld/%ld trials\n", n_fail, iters);
        return 1;
    }
    fprintf(stderr, "perfmon smoke OK: %ld iterations\n", iters);
    return 0;
}
#endif
