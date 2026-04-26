/*****************************************************************************
 * fuzz_content_probe.c - fuzz the content-aware probe metrics.
 *****************************************************************************
 * The probe reads source pixels directly via pointer arithmetic on a
 * caller-supplied plane. That makes it a memory-safety risk surface:
 * a wrong stride, a w/h that doesn't match the buffer, or a degenerate
 * shape could produce out-of-bounds reads. ASan + UBSan catch those
 * instantly.
 *
 * What's checked on every iteration:
 *
 *   - Both metric functions never crash on any combination of
 *     (stride, w, h) the fuzzer drives, when given a buffer sized to
 *     `stride * h` bytes. Out-of-bounds reads are caught by ASan.
 *
 *   - Both functions are deterministic: calling the same function
 *     twice on the same input produces the same result.
 *
 *   - When n_samples_out is reported as 0, the returned sum is also 0.
 *     (Otherwise we'd be returning sums computed with no observations.)
 *
 *   - The accumulator's `frames` counter advances monotonically with
 *     observe() calls, regardless of the metric values passed in.
 *
 *   - The bypass decision rule is total: it returns 0 or 1 for every
 *     accumulator state, never crashes, never returns other values.
 *
 *   - Cross-property: feeding the SAME accumulator state through
 *     up_should_bypass_for_content() repeatedly is idempotent.
 *
 *****************************************************************************/

#include "../src/content_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(fmt, ...) do { \
    fprintf(stderr, "INVARIANT: " fmt "\n", __VA_ARGS__); \
    fflush(stderr); \
    abort(); \
} while (0)

/* Realistic dimensions to bias toward. Pure random ints almost never
 * land inside a sane shape, so coverage of the real loop bodies is
 * negligible without bias. */
static const int common_w[] = {
    0, 1, 2, 8, 16, 64, 128, 256, 320, 480, 640, 720, 854, 1280, 1920, 4096,
};
static const int common_h[] = {
    0, 1, 2, 4, 8, 16, 64, 128, 240, 360, 480, 720, 1080, 2160, 4320,
};

/* xorshift32 deterministic for the smoke main */
static uint32_t xs_state = 0xC0FFEEu;
static uint32_t xs(void) {
    uint32_t s = xs_state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    xs_state = s;
    return s;
}

/* Pull a value of type T from the fuzz input, defaulting to a random
 * source when the input is too short. */
#define PULL(buf, off, size, T, dst) do { \
    if ((off) + sizeof(T) <= (size)) { memcpy(&(dst), (buf) + (off), sizeof(T)); (off) += sizeof(T); } \
    else { (dst) = (T)xs(); } \
} while (0)

static void run_one(const uint8_t *data, size_t size)
{
    size_t off = 0;

    /* Pull control bytes from the input. */
    uint8_t  shape_sel;   /* selects from common dim arrays vs raw int */
    uint16_t w_raw;
    uint16_t h_raw;
    int16_t  stride_offset;  /* applied to w to get stride; can be < 0 */
    PULL(data, off, size, uint8_t, shape_sel);
    PULL(data, off, size, uint16_t, w_raw);
    PULL(data, off, size, uint16_t, h_raw);
    PULL(data, off, size, int16_t, stride_offset);

    int w, h;
    if ((shape_sel & 1) == 0) {
        w = common_w[(shape_sel >> 1) % (sizeof common_w / sizeof *common_w)];
        h = common_h[(shape_sel >> 4) % (sizeof common_h / sizeof *common_h)];
    } else {
        /* Cap raw values so we don't allocate gigabyte buffers in the
         * fuzzer. Real plane dims fit in [0, 8192]. */
        w = w_raw % 256;   /* keep small to maximize iterations/sec */
        h = h_raw % 256;
    }

    /* Stride must be >= w for valid VLC plane layout; the fuzzer can
     * also drive stride_offset negative or much larger to exercise the
     * function's stride handling. We clamp so we don't read past the
     * allocated buffer. */
    int stride = w + (int)stride_offset;
    if (stride < 0) stride = 0;
    if (stride > 8192) stride = 8192;

    /* Allocate `max(stride, 1) * max(h, 1)` bytes — when w or h is 0,
     * we still need a valid pointer that ASan can range-check against.
     * Fill with deterministic-but-non-trivial bytes so the metrics do
     * real work. */
    size_t alloc = (size_t)(stride > 0 ? stride : 1) * (size_t)(h > 0 ? h : 1);
    if (alloc > 256 * 1024) return;  /* skip pathological; keep iters fast */
    uint8_t *plane = (uint8_t *)malloc(alloc);
    if (!plane) return;
    /* Pattern: pull from fuzz input where possible, fill rest with xorshift. */
    size_t fill_from_input = (size > off) ? (size - off) : 0;
    if (fill_from_input > alloc) fill_from_input = alloc;
    memcpy(plane, data + off, fill_from_input);
    for (size_t i = fill_from_input; i < alloc; i++)
        plane[i] = (uint8_t)xs();

    /* === Metric 1: Laplacian variance ===
     * Determinism: calling twice must produce identical output. */
    uint64_t lap_n_a = 999, lap_n_b = 999;
    uint64_t lap_a = up_laplacian_variance(plane, stride, w, h, &lap_n_a);
    uint64_t lap_b = up_laplacian_variance(plane, stride, w, h, &lap_n_b);
    if (lap_a != lap_b || lap_n_a != lap_n_b) {
        FAIL("laplacian_variance non-deterministic on (stride=%d w=%d h=%d): "
             "a=%llu/%llu b=%llu/%llu",
             stride, w, h,
             (unsigned long long)lap_a, (unsigned long long)lap_n_a,
             (unsigned long long)lap_b, (unsigned long long)lap_n_b);
    }
    /* When n=0, sum must also be 0 — otherwise we'd return totals from
     * unobserved samples. */
    if (lap_n_a == 0 && lap_a != 0) {
        FAIL("laplacian_variance returned sum=%llu with n=0",
             (unsigned long long)lap_a);
    }

    /* === Metric 2: block-edge strength === */
    uint64_t edge_n_a = 999, edge_n_b = 999;
    uint64_t edge_a = up_block_edge_strength(plane, stride, w, h, &edge_n_a);
    uint64_t edge_b = up_block_edge_strength(plane, stride, w, h, &edge_n_b);
    if (edge_a != edge_b || edge_n_a != edge_n_b) {
        FAIL("block_edge_strength non-deterministic on (stride=%d w=%d h=%d)",
             stride, w, h);
    }
    if (edge_n_a == 0 && edge_a != 0) {
        FAIL("block_edge_strength returned sum=%llu with n=0",
             (unsigned long long)edge_a);
    }

    /* === Accumulator: observe() advances frames monotonically. */
    up_probe_accum_t accum;
    memset(&accum, 0, sizeof accum);
    int prior_frames = accum.frames;
    up_probe_observe(&accum, lap_a, lap_n_a, edge_a, edge_n_a);
    if (accum.frames != prior_frames + 1) {
        FAIL("observe didn't advance frames: %d -> %d", prior_frames, accum.frames);
    }
    /* Observing again advances again. */
    up_probe_observe(&accum, 0, 0, 0, 0);
    if (accum.frames != prior_frames + 2) {
        FAIL("second observe didn't advance: %d -> %d", prior_frames, accum.frames);
    }

    /* === Bypass decision: total function over all accumulator states. ===
     * Drive the accumulator with arbitrary fuzz-derived values and
     * verify the decision is always 0 or 1, and idempotent. */
    up_probe_accum_t a2;
    PULL(data, off, size, uint64_t, a2.lap_sum);
    PULL(data, off, size, uint64_t, a2.lap_samples);
    PULL(data, off, size, uint64_t, a2.edge_sum);
    PULL(data, off, size, uint64_t, a2.edge_samples);
    int frames32;
    PULL(data, off, size, int, frames32);
    a2.frames = frames32;

    int d1 = up_should_bypass_for_content(&a2);
    int d2 = up_should_bypass_for_content(&a2);
    if (d1 != 0 && d1 != 1) {
        FAIL("bypass returned %d (not 0 or 1) for accum frames=%d "
             "lap_sum=%llu lap_n=%llu edge_sum=%llu edge_n=%llu",
             d1, a2.frames,
             (unsigned long long)a2.lap_sum, (unsigned long long)a2.lap_samples,
             (unsigned long long)a2.edge_sum, (unsigned long long)a2.edge_samples);
    }
    if (d1 != d2) {
        FAIL("bypass non-idempotent: %d != %d on identical accum", d1, d2);
    }
    /* NULL accumulator must return 0, not crash. Use a runtime-selected
     * pointer (not a literal NULL) so cppcheck can't dead-code-
     * eliminate the failure branch. */
    const up_probe_accum_t *maybe_null = (a2.frames & 1) ? NULL : &a2;
    int dn = up_should_bypass_for_content(maybe_null);
    if (dn != 0 && dn != 1) {
        FAIL("bypass returned %d for NULL/valid accum", dn);
    }
    /* When the pointer was actually NULL, must return 0. */
    if (maybe_null == NULL && dn != 0) {
        FAIL("bypass(NULL) returned %d (must be 0)", dn);
    }

    free(plane);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run_one(data, size);
    return 0;
}

#ifdef FUZZ_MAIN
int main(int argc, char **argv)
{
    long n = 50000;
    if (argc > 1) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (end && *end == '\0' && v > 0) n = v;
    }

    uint8_t buf[64];
    for (long i = 0; i < n; i++) {
        for (size_t j = 0; j < sizeof buf; j += 4) {
            uint32_t r = xs();
            memcpy(buf + j, &r, 4);
        }
        run_one(buf, sizeof buf);
    }
    printf("content_probe smoke OK: %ld iterations\n", n);
    return 0;
}
#endif
