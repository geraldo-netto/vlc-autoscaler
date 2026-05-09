// SPDX-License-Identifier: GPL-2.0-or-later
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

/* Parse fuzz bytes into (w, h, stride). Advances *off. */
static void parse_shape(const uint8_t *data, size_t size, size_t *off,
                        int *w_out, int *h_out, int *stride_out)
{
    uint8_t  shape_sel;
    uint16_t w_raw;
    uint16_t h_raw;
    int16_t  stride_offset;
    PULL(data, *off, size, uint8_t, shape_sel);
    PULL(data, *off, size, uint16_t, w_raw);
    PULL(data, *off, size, uint16_t, h_raw);
    PULL(data, *off, size, int16_t, stride_offset);

    int w, h;
    if ((shape_sel & 1) == 0) {
        w = common_w[(shape_sel >> 1) % (sizeof common_w / sizeof *common_w)];
        h = common_h[(shape_sel >> 4) % (sizeof common_h / sizeof *common_h)];
    } else {
        w = w_raw % 256;
        h = h_raw % 256;
    }

    int stride = w + (int)stride_offset;
    if (stride < 0) stride = 0;
    if (stride > 8192) stride = 8192;

    *w_out = w;
    *h_out = h;
    *stride_out = stride;
}

/* Allocate and fill plane buffer; returns NULL if we should skip this iter.
 * Reads from data+off but does NOT advance off — matches original behavior
 * where fill bytes overlap with subsequent pull_accum reads. */
static uint8_t *alloc_and_fill_plane(const uint8_t *data, size_t size, size_t off,
                                     int stride, int h)
{
    size_t alloc = (size_t)(stride > 0 ? stride : 1) * (size_t)(h > 0 ? h : 1);
    if (alloc > 256 * 1024) return NULL;
    uint8_t *plane = (uint8_t *)calloc(1, alloc);
    if (!plane) return NULL;
    size_t fill_from_input = (size > off) ? (size - off) : 0;
    if (fill_from_input > alloc) fill_from_input = alloc;
    memcpy(plane, data + off, fill_from_input);
    for (size_t i = fill_from_input; i < alloc; i++)
        plane[i] = (uint8_t)xs();
    return plane;
}

/* Metric function pointer signature shared by laplacian + edge probes. */
typedef uint64_t (*metric_fn_t)(const uint8_t *, int, int, int, uint64_t *);

/* Run a metric twice: check determinism + (n==0 -> sum==0). */
static uint64_t check_metric(const char *name, metric_fn_t fn,
                             const uint8_t *plane, int stride, int w, int h,
                             uint64_t *n_out)
{
    uint64_t n_a = 999, n_b = 999;
    uint64_t a = fn(plane, stride, w, h, &n_a);
    uint64_t b = fn(plane, stride, w, h, &n_b);
    if (a != b || n_a != n_b) {
        FAIL("%s non-deterministic on (stride=%d w=%d h=%d): "
             "a=%llu/%llu b=%llu/%llu",
             name, stride, w, h,
             (unsigned long long)a, (unsigned long long)n_a,
             (unsigned long long)b, (unsigned long long)n_b);
    }
    if (n_a == 0 && a != 0) {
        FAIL("%s returned sum=%llu with n=0", name, (unsigned long long)a);
    }
    *n_out = n_a;
    return a;
}

/* Verify observe() advances frames monotonically across two calls. */
static void check_observe_monotonic(uint64_t lap_a, uint64_t lap_n,
                                    uint64_t edge_a, uint64_t edge_n)
{
    up_probe_accum_t accum;
    memset(&accum, 0, sizeof accum);
    int prior_frames = accum.frames;
    up_probe_observe(&accum, lap_a, lap_n, edge_a, edge_n);
    if (accum.frames != prior_frames + 1) {
        FAIL("observe didn't advance frames: %d -> %d", prior_frames, accum.frames);
    }
    up_probe_observe(&accum, 0, 0, 0, 0);
    if (accum.frames != prior_frames + 2) {
        FAIL("second observe didn't advance: %d -> %d", prior_frames, accum.frames);
    }
}

/* Pull a fuzz-driven accumulator state. */
static void pull_accum(const uint8_t *data, size_t size, size_t *off,
                       up_probe_accum_t *a2)
{
    PULL(data, *off, size, uint64_t, a2->lap_sum);
    PULL(data, *off, size, uint64_t, a2->lap_samples);
    PULL(data, *off, size, uint64_t, a2->edge_sum);
    PULL(data, *off, size, uint64_t, a2->edge_samples);
    int frames32;
    PULL(data, *off, size, int, frames32);
    a2->frames = frames32;
}

/* Verify bypass: bool-valued, idempotent, NULL-safe. */
static void check_bypass(const up_probe_accum_t *a2)
{
    int d1 = up_should_bypass_for_content(a2);
    int d2 = up_should_bypass_for_content(a2);
    if (d1 != 0 && d1 != 1) {
        FAIL("bypass returned %d (not 0 or 1) for accum frames=%d "
             "lap_sum=%llu lap_n=%llu edge_sum=%llu edge_n=%llu",
             d1, a2->frames,
             (unsigned long long)a2->lap_sum, (unsigned long long)a2->lap_samples,
             (unsigned long long)a2->edge_sum, (unsigned long long)a2->edge_samples);
    }
    if (d1 != d2) {
        FAIL("bypass non-idempotent: %d != %d on identical accum", d1, d2);
    }
    const up_probe_accum_t *maybe_null = (a2->frames & 1) ? NULL : a2;
    int dn = up_should_bypass_for_content(maybe_null);
    if (dn != 0 && dn != 1) {
        FAIL("bypass returned %d for NULL/valid accum", dn);
    }
    if (maybe_null == NULL && dn != 0) {
        FAIL("bypass(NULL) returned %d (must be 0)", dn);
    }
}

static void run_one(const uint8_t *data, size_t size)
{
    size_t off = 0;
    int w, h, stride;
    parse_shape(data, size, &off, &w, &h, &stride);

    uint8_t *plane = alloc_and_fill_plane(data, size, off, stride, h);
    if (!plane) return;

    uint64_t lap_n, edge_n;
    uint64_t lap_a  = check_metric("laplacian_variance",  up_laplacian_variance,
                                   plane, stride, w, h, &lap_n);
    uint64_t edge_a = check_metric("block_edge_strength", up_block_edge_strength,
                                   plane, stride, w, h, &edge_n);

    check_observe_monotonic(lap_a, lap_n, edge_a, edge_n);

    up_probe_accum_t a2;
    pull_accum(data, size, &off, &a2);
    check_bypass(&a2);

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
    long n = 100000;
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
