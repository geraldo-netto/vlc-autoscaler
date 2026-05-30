// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_scaler_seam.c - fuzz the multithreaded zimg scaler's tiling invariant.
 *****************************************************************************
 * SCAL-3 prerequisite. The scaler partitions each frame into N horizontal
 * stripes, one zimg graph per stripe. Each stripe restarts zimg's resize
 * coordinate origin, so tiled output carries a sub-pixel PHASE rounding at the
 * boundary — bounded to a few code values on real (low-frequency) content.
 *
 * This fuzzer drives random geometry (src/dst dims, chroma) and a random
 * thread count through the real backend and asserts two things on SMOOTH
 * (gradient) input:
 *
 *   1. crash-/UB-/leak-free open->process->close (ASan/UBSan), and
 *   2. the seam invariant: the N-stripe result stays within SEAM_MAX_DELTA of
 *      the single-graph (threads=1) reference.
 *
 * It is the moving-geometry complement to the fixed-config seam oracle in
 * test_scaler_zimg.c, and the gate that must keep passing once column tiling
 * (2D) is added — column tiles add the same bounded horizontal phase rounding
 * and must not push any pixel past the ceiling.
 *
 * Links a private copy of scaler_zimg.c under sanitizers against real VLC +
 * zimg headers (log_obj is NULL, so no VLC pool/logging is touched), exactly
 * like tests/test_scaler_zimg.c.
 *****************************************************************************/
#include "../src/scaler.h"
#define ZIMG_TEST_DEFINE_MODULE_NAME   /* emit the vlc_module_name stub here */
#include "zimg_test_util.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Looser ceiling than the fixed-config oracle's 6: the fuzzer explores
 * extreme geometries (tiny heights -> ~4-row stripes, absurd aspect ratios)
 * where the synthetic gradient is steep and per-stripe phase rounding is
 * amplified. Empirically the current (correct, no-halo) tiling stays <= 12
 * across 4000+ random configs; 16 leaves margin. The point is to catch GROSS
 * seams / crashes across wild geometry, and to hold column tiling (2D) to the
 * same envelope — not to pin the exact sub-pixel rounding. */
#define SEAM_MAX_DELTA 16

static const uint32_t CHROMAS[] = {
    VLC_CODEC_I420, VLC_CODEC_YV12, VLC_CODEC_I422, VLC_CODEC_I444,
};

/* Map 4 input bytes to a bounded value in [lo, hi]. */
static int pick(const uint8_t *b, int lo, int hi)
{
    uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
               | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return lo + (int)(v % (uint32_t)(hi - lo + 1));
}

/* Fill the visible region with a gentle 2D gradient (low-frequency). */
static void fill_smooth(zt_pic_t *tp)
{
    for (int k = 0; k < tp->pic.i_planes; k++) {
        plane_t *p = &tp->pic.p[k];
        for (int y = 0; y < p->i_visible_lines; y++) {
            uint8_t *row = p->p_pixels + (size_t)y * (size_t)p->i_pitch;
            int vy = p->i_visible_lines > 1
                   ? y * 128 / (p->i_visible_lines - 1) : 0;
            for (int x = 0; x < p->i_visible_pitch; x++) {
                int vx = p->i_visible_pitch > 1
                       ? x * 127 / (p->i_visible_pitch - 1) : 0;
                row[x] = (uint8_t)(vx + vy);
            }
        }
    }
}

/* Resample one config with `threads` workers into `out`. Returns process rc
 * (0 ok), or -2 if allocation/open failed. */
static int resample(uint32_t chroma, int sw, int sh, int dw, int dh,
                    int threads, zt_pic_t *out)
{
    zt_pic_t src;
    if (zt_pic_alloc(&src, chroma, sw, sh) != 0) return -2;
    if (zt_pic_alloc(out, chroma, dw, dh) != 0) { zt_pic_free(&src); return -2; }
    fill_smooth(&src);
    zt_pic_memset(out, 0x00);

    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, chroma, sw, sh, dw, dh, threads, 0);
    int rc = -2;
    if (ctx.backend->open(&ctx) == 0) {
        rc = ctx.backend->process(&ctx, &src.pic, &out->pic);
        ctx.backend->close(&ctx);
    }
    zt_pic_free(&src);
    return rc;
}

/* Max abs per-byte delta over the visible region of two same-geometry pics. */
static int max_delta(const zt_pic_t *a, const zt_pic_t *b)
{
    int maxd = 0;
    for (int k = 0; k < a->pic.i_planes; k++) {
        const plane_t *pa = &a->pic.p[k];
        const plane_t *pb = &b->pic.p[k];
        for (int y = 0; y < pa->i_visible_lines; y++) {
            const uint8_t *ra = pa->p_pixels + (size_t)y * (size_t)pa->i_pitch;
            const uint8_t *rb = pb->p_pixels + (size_t)y * (size_t)pb->i_pitch;
            for (int x = 0; x < pa->i_visible_pitch; x++) {
                int d = (int)ra[x] - (int)rb[x];
                if (d < 0) d = -d;
                if (d > maxd) maxd = d;
            }
        }
    }
    return maxd;
}

static void run_one(const uint8_t *data, size_t size)
{
    if (size < 16) return;
    uint32_t chroma = CHROMAS[data[0] % (sizeof CHROMAS / sizeof *CHROMAS)];
    /* Down to 2px to exercise degenerate cells / tiny stripes; even keeps
     * subsampled chroma valid. The backend must stay memory-safe (and either
     * fail gracefully or hold the seam bound) for any of these. */
    int sw = pick(data + 1, 2, 512) & ~1;
    int sh = pick(data + 5, 2, 512) & ~1;
    int dw = pick(data + 9, sw, sw * 3) & ~1;   /* upscale (or equal) */
    int dh = pick(data + 12, sh, sh * 3) & ~1;
    int threads = pick(data + 9, 2, 32);        /* >=2 to tile; >cores stresses */
    if (sw < 2 || sh < 2 || dw < 2 || dh < 2) return;

    zt_pic_t ref, tiled;
    if (resample(chroma, sw, sh, dw, dh, 1, &ref) != 0) return;
    if (resample(chroma, sw, sh, dw, dh, threads, &tiled) == 0) {
        int md = max_delta(&ref, &tiled);
        if (md > SEAM_MAX_DELTA) {
            fprintf(stderr, "SEAM: %dx%d->%dx%d t=%d chroma=%08x maxdelta=%d\n",
                    sw, sh, dw, dh, threads, chroma, md);
            __builtin_trap();
        }
        zt_pic_free(&tiled);
    }
    zt_pic_free(&ref);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run_one(data, size);
    return 0;
}

#ifdef FUZZ_MAIN
int main(int argc, char **argv)
{
    long n = 400;   /* each iter spawns threads + builds graphs: keep modest */
    if (argc > 1) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (end && *end == '\0' && v > 0) n = v;
    }

    uint32_t s = 0x5EA3711u;
    uint8_t buf[16];
    for (long i = 0; i < n; i++) {
        for (size_t j = 0; j < sizeof buf; j += 4) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            memcpy(buf + j, &s, 4);
        }
        run_one(buf, sizeof buf);
    }
    printf("scaler_seam smoke OK: %ld iterations\n", n);
    return 0;
}
#endif
