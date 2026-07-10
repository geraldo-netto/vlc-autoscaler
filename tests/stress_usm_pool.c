// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * stress_usm_pool.c - concurrency stress test for the threaded USM pool
 *****************************************************************************
 * Different beast from a unit test or fuzzer:
 *
 *   - Drives the worker pool through THOUSANDS of frames at unusual
 *     (thread_count, width, height) combinations, including:
 *
 *       * 64 threads on a 32-line frame: stripe count gets clamped down
 *         to height/8, exposing the clamp logic.
 *       * 1 thread on a 4096x2160 frame: tests single-worker path and
 *         large workspace allocation.
 *       * 854x480 at thread counts in {1, 2, 4, 8, 16, 32, 64}, plus
 *         representative 1920x1080 configurations.
 *
 *   - Verifies BYTE-IDENTICAL output to the single-threaded reference
 *     up_usm_apply_plane() on every single frame, every config. If a
 *     race ever produces a wrong byte, this test will see it.
 *
 *   - Mutates the input data (xorshift32 fill) every frame so workers
 *     must read fresh pointers each call. A bug where workers cached
 *     stale frame pointers would surface as "first frame matches,
 *     subsequent frames diverge."
 *
 *   - Runs under TSan as part of `make stress`. TSan catches
 *     races on the workspace, semaphores, and per-frame pointers even
 *     when the output happens to be correct on this run (which would
 *     otherwise mask intermittent races).
 *
 *   - Exits non-zero on ANY divergence, including a single byte. No
 *     fuzziness in the comparison — we want bit-perfect agreement.
 *
 * `make stress` builds and runs the ASan+UBSan and TSan executables.
 *****************************************************************************/

#include "../src/usm_pool.h"
#include "../src/usm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* xorshift32 — same as the smoke fuzzers. Deterministic across runs. */
static uint32_t xs32(uint32_t *s)
{
    uint32_t v = *s;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    *s = v;
    return v;
}

static void fill_xorshift(uint8_t *buf, size_t n, uint32_t seed)
{
    uint32_t s = seed ? seed : 0xC0FFEEu;
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)xs32(&s);
}

/* Returns 0 on byte-identical, otherwise the count of differing bytes. */
static size_t byte_diff(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t d = 0;
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) d++;
    return d;
}

typedef struct {
    int  n_threads;
    int  width;
    int  height;
    int  frames;
    int  amount_pct;   /* USM strength */
    const char *name;
    int  in_place;     /* 1 = pool runs dst==src (production mode, SYS-4);
                        * trailing so older configs default to 0 */
} stress_config_t;

typedef struct {
    uint8_t *src;
    uint8_t *dst_st;
    uint8_t *dst_mt;
    uint8_t *ws;
} stress_bufs_t;

/* Allocate the four per-config buffers. Returns 0 on success, -1 on
 * malloc failure (in which case any partial allocations are freed). */
static int alloc_bufs(stress_bufs_t *b, size_t plane_size)
{
    b->src    = malloc(plane_size);
    b->dst_st = malloc(plane_size);
    b->dst_mt = malloc(plane_size);
    b->ws     = malloc(plane_size);
    if (!b->src || !b->dst_st || !b->dst_mt || !b->ws) return -1;
    return 0;
}

static void free_bufs(stress_bufs_t *b)
{
    free(b->src); free(b->dst_st); free(b->dst_mt); free(b->ws);
}

/* Run one frame: refill src, run reference + pool, compare. Returns
 * the number of differing bytes, or SIZE_MAX if the pool apply failed. */
static size_t run_one_frame(const stress_config_t *cfg, stress_bufs_t *b,
                            usm_pool_t *pool, int amount, int frame,
                            size_t plane_size)
{
    /* Fresh src each frame — defeats any pointer-caching bug. */
    fill_xorshift(b->src, plane_size, 0xDEADBEEFu + (uint32_t)frame * 7919);

    /* Single-threaded reference. */
    memset(b->dst_st, 0, plane_size);
    memset(b->ws, 0, plane_size);
    up_usm_apply_plane(b->dst_st, cfg->width, b->src, cfg->width,
                       cfg->width, cfg->height, amount, b->ws);

    /* Multi-threaded: same input, must produce identical output. The
     * in-place mode mirrors production (Filter() sharpens the VLC luma
     * plane with dst == src): seed dst_mt with the source and hand the
     * pool the same pointer for both sides. */
    if (cfg->in_place) {
        memcpy(b->dst_mt, b->src, plane_size);
        if (up_usm_pool_apply(pool, b->dst_mt, cfg->width, b->dst_mt,
                              cfg->width, amount) != 0)
            return (size_t)-1;
    } else {
        memset(b->dst_mt, 0xAA, plane_size);  /* poison: catch unwritten regions */
        if (up_usm_pool_apply(pool, b->dst_mt, cfg->width, b->src, cfg->width,
                              amount) != 0)
            return (size_t)-1;
    }

    return byte_diff(b->dst_st, b->dst_mt, plane_size);
}

/* Drive cfg->frames through pool. Sets *diverged_frames and
 * *total_diff_bytes. Returns 0 on success, -1 on apply failure. */
static int drive_frames(const stress_config_t *cfg, stress_bufs_t *b,
                        usm_pool_t *pool, int amount, size_t plane_size,
                        int *diverged_frames, size_t *total_diff_bytes)
{
    *diverged_frames = 0;
    *total_diff_bytes = 0;
    for (int frame = 0; frame < cfg->frames; frame++) {
        size_t d = run_one_frame(cfg, b, pool, amount, frame, plane_size);
        if (d == (size_t)-1) {
            printf("APPLY FAILED at frame %d\n", frame);
            return -1;
        }
        if (d == 0) continue;
        (*diverged_frames)++;
        *total_diff_bytes += d;
        if (*diverged_frames <= 3)
            printf("\n    frame %d: %zu bytes differ", frame, d);
    }
    return 0;
}

static void report_result(const stress_config_t *cfg, int diverged_frames,
                          size_t total_diff_bytes, double elapsed_ms,
                          int *rc)
{
    if (diverged_frames > 0) {
        if (diverged_frames > 3)
            printf("\n    ...total %d frames diverged, %zu bytes",
                   diverged_frames, total_diff_bytes);
        printf("\n    FAIL\n");
        *rc = -1;
    } else if (*rc == 0) {
        printf("OK  (%d frames, %.0f ms, %.2f us/frame)\n",
               cfg->frames, elapsed_ms,
               (elapsed_ms * 1000.0) / cfg->frames);
    }
}

/* Run one stress configuration. Returns 0 on success, -1 on any
 * divergence or pool failure. */
static int run_config(const stress_config_t *cfg)
{
    printf("  [%-30s] ", cfg->name);
    fflush(stdout);

    int amount = up_usm_amount_pct_to_q8(cfg->amount_pct);
    size_t plane_size = (size_t)cfg->width * (size_t)cfg->height;
    if (plane_size == 0) {
        printf("INVALID (w*h=0)\n");
        return -1;
    }

    stress_bufs_t b;
    if (alloc_bufs(&b, plane_size) != 0) {
        free_bufs(&b);
        printf("MALLOC FAILED\n");
        return -1;
    }

    usm_pool_t *pool = up_usm_pool_create(cfg->n_threads, cfg->width, cfg->height, 0);
    if (!pool) {
        free_bufs(&b);
        printf("POOL CREATE FAILED\n");
        return -1;
    }

    int rc = 0;
    int diverged_frames = 0;
    size_t total_diff_bytes = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (drive_frames(cfg, &b, pool, amount, plane_size,
                     &diverged_frames, &total_diff_bytes) != 0) {
        rc = -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                      + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;

    report_result(cfg, diverged_frames, total_diff_bytes, elapsed_ms, &rc);

    up_usm_pool_destroy(pool);
    free_bufs(&b);
    return rc;
}

int main(int argc, char **argv)
{
    /* Allow scaling iterations from the command line for debugging or
     * cranking up the heat. Default is enough to catch races within ~5s
     * total wall time. */
    int frame_mult = 1;
    if (argc > 1) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (end && *end == '\0' && v > 0 && v < 1000) frame_mult = (int)v;
    }

    /* Configurations covering the interesting axes:
     *   - thread count: 1, 2, 4, 8, 16, 32, 64
     *   - width: 8, 64, 128, 256, 853, 854, 855, 1920, 4096
     *   - height: 8, 32, 64, 128, 479, 480, 481, 1080, 2160
     *
     * The pool clamps n_threads to height/8 internally, so configs
     * like "64 threads on 32-line frame" exercise the clamp.
     *
     * Frame counts are tuned so the full ASan run is under ~15s and
     * the TSan run (slower instrumentation) under ~30s. */
    stress_config_t configs[] = {
        /* High thread count, tiny frame -> clamp down to height/8 */
        { 64,   64,   32,   100,  30, "64thr 64x32 (clamp)"     },
        { 64,  128,   64,   100,  30, "64thr 128x64 (clamp)"    },
        { 32,  256,  128,   100,  30, "32thr 256x128 (clamp)"   },

        /* Common video resolutions x typical thread counts */
        {  1,  854,  480,    50,  30, "1thr  854x480"           },
        {  2,  854,  480,    50,  30, "2thr  854x480"           },
        {  4,  854,  480,    50,  30, "4thr  854x480"           },
        {  8,  854,  480,    50,  30, "8thr  854x480"           },
        { 16,  854,  480,    50,  30, "16thr 854x480"           },
        { 32,  854,  480,    50,  30, "32thr 854x480"           },
        { 64,  854,  480,    50,  30, "64thr 854x480"           },

        /* 1080p at 16 threads — most common real-world config */
        { 16, 1920, 1080,    20,  30, "16thr 1920x1080"         },
        { 16, 1920, 1080,    20,   0, "16thr 1080p amount=0"    },  /* identity path */
        { 16, 1920, 1080,    20, 100, "16thr 1080p amount=100"  },
        { 16, 1920, 1080,    20, 200, "16thr 1080p amount=200 (max)" },

        /* Pathological aspects */
        {  8,    8, 1080,    50,  30, "8thr  tall narrow 8x1080" },
        {  8, 4096,    8,    50,  30, "8thr  short wide 4096x8 (clamp)" },

        /* Odd dimensions to flush odd-row handling */
        { 16,  853,  479,    50,  30, "16thr 853x479 (odd)"     },
        { 16,  855,  481,    50,  30, "16thr 855x481 (odd)"     },

        /* Big frame, single thread - tests workspace alloc at scale */
        {  1, 4096, 2160,     5,  30, "1thr  4096x2160"         },

        /* IN-PLACE (dst == src), the production call shape (SYS-4).
         * Multi-thread configs hammer the halo-row snapshots: every
         * stripe boundary is a potential neighbour read/write race that
         * TSan would flag and byte-diff would surface. */
        {  1,  854,  480,    50,  30, "1thr  854x480 IN-PLACE",   1 },
        {  4,  854,  480,    50,  30, "4thr  854x480 IN-PLACE",   1 },
        { 16,  854,  480,    50,  30, "16thr 854x480 IN-PLACE",   1 },
        { 64,  854,  480,    50,  30, "64thr 854x480 IN-PLACE",   1 },
        { 16, 1920, 1080,    20,  30, "16thr 1920x1080 IN-PLACE", 1 },
        { 16, 1920, 1080,    20, 200, "16thr 1080p a=200 IN-PLACE", 1 },
        { 16,  853,  479,    50,  30, "16thr 853x479 odd IN-PLACE", 1 },
        { 64,  128,   64,   100,  30, "64thr 128x64 clamp IN-PLACE", 1 },
    };

    int n_configs = (int)(sizeof configs / sizeof *configs);
    int passed = 0, failed = 0;

    printf("usm_pool concurrency stress test (frame_mult=%d)\n", frame_mult);
    printf("Each config: identical output to single-threaded reference required.\n\n");

    for (int i = 0; i < n_configs; i++) {
        stress_config_t cfg = configs[i];
        cfg.frames *= frame_mult;
        if (run_config(&cfg) == 0) passed++;
        else failed++;
    }

    printf("\n%d passed, %d failed (out of %d configs)\n",
           passed, failed, n_configs);
    return failed == 0 ? 0 : 1;
}
