// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_scaler_zimg.c — invariant tests for the grid-threaded zimg backend
 *****************************************************************************
 * scaler_zimg.c had no automated coverage because it is VLC-typed and
 * resamples real frames. This harness drives backend->open/process/close on
 * hand-built picture_t objects (see zimg_test_util.h) so the threaded path
 * runs under ASan/UBSan (and a TSan build) across many (chroma, dims,
 * thread-count, zerocopy) combinations.
 *
 * Independent per-cell graphs do NOT produce byte-identical output to a single
 * full-frame resize (each graph can restart resize phase), so we assert
 * INVARIANTS rather than an exact reference:
 *
 *   1. full-write   — every visible destination byte is written. Run with
 *                     dst pre-filled 0x00 and again 0xFF; written bytes are
 *                     init-independent, so the two outputs must match. A
 *                     mismatch means an unwritten band (the partial-stripe
 *                     black-band regression the backend guards against).
 *   2. determinism  — same input twice -> identical output (catches races,
 *                     especially under ThreadSanitizer).
 *   3. zerocopy==copyout — the backend documents the zero-copy-dst path as
 *                     byte-identical to the scratch copy-out path; verify it.
 *
 * This is also the harness PERF-1 / SCAL-3 (parked) need before the threaded
 * dispatch / striping can be changed with confidence.
 *****************************************************************************/
#define ZIMG_TEST_DEFINE_MODULE_NAME
#include "zimg_test_util.h"
#include "barrier_fault_inject.h"

#include <stdio.h>
#include <sys/resource.h>

static int g_run = 0, g_fail = 0, g_cur_fail = 0;
static const char *g_cur = NULL;

#define BEGIN(name) do { g_cur = name; g_cur_fail = 0; g_run++; } while (0)
#define END() do { \
        if (g_cur_fail) { g_fail++; printf("  [FAIL] %s\n", g_cur); } \
        else            { printf("  [ ok ] %s\n", g_cur); } \
    } while (0)
#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_cur_fail = 1; \
        } \
    } while (0)

struct zcfg {
    uint32_t    chroma;
    const char *name;
    int sw, sh, dw, dh, threads;
    /* 1 when up_decide_tile_grid picks cols > 1 for this geometry (short
     * dst vs threads, wide enough for 64px column tiles). Trailing so
     * untiled configs default to 0. SYS-5: with src_zerocopy=0 these now
     * run a rows-only grid, whose output is NOT byte-identical to the
     * tiled grid's (different graph partition, seam-level deltas). */
    int col_tiled;
};

static const struct zcfg CFGS[] = {
    { VLC_CODEC_I420, "I420 640x360->1280x720  t1",  640, 360, 1280, 720,  1 },
    { VLC_CODEC_I420, "I420 640x360->1280x720  t4",  640, 360, 1280, 720,  4 },
    { VLC_CODEC_I420, "I420 854x480->1920x1080 t8",  854, 480, 1920, 1080, 8 },
    { VLC_CODEC_I420, "I420 854x480->1920x1080 t16", 854, 480, 1920, 1080, 16 },
    { VLC_CODEC_YV12, "YV12 640x360->1280x720  t4",  640, 360, 1280, 720,  4 },
    { VLC_CODEC_YV12, "YV12 720x404->1920x1080 t8",  720, 404, 1920, 1080, 8 },
    { VLC_CODEC_I422, "I422 640x360->1280x720  t4",  640, 360, 1280, 720,  4 },
    { VLC_CODEC_I422, "I422 854x480->1920x1080 t8",  854, 480, 1920, 1080, 8 },
    { VLC_CODEC_I444, "I444 640x360->1280x720  t4",  640, 360, 1280, 720,  4 },
    { VLC_CODEC_I444, "I444 480x270->1280x720  t8",  480, 270, 1280, 720,  8 },
    /* Odd dims only with I444 (no chroma subsampling). Subsampled chromas
     * (I420/YV12/I422) require even dims, which production satisfies on
     * BOTH sides: dst via up__clamp_even in up_compute_target_dims, src
     * via the REL-4 even-align in Open (odd visible crops get one
     * row/column cropped before the scaler ever sees them). */
    { VLC_CODEC_I444, "I444 odd 853x481->1281x721 t8", 853, 481, 1281, 721, 8 },
    { VLC_CODEC_I420, "I420 tiny 64x64->128x128 t8",   64,  64,  128,  128, 8 },
    { VLC_CODEC_I420, "I420 clamp 100x16->200x32 t64", 100, 16,  200,  32, 64, 1 },
    /* SCAL-3: wide + short -> row stripes alone can't use all threads, so the
     * grid tiles COLUMNS. dst_h/16 < threads triggers n_cols > 1. */
    { VLC_CODEC_I420, "I420 wide 960x48->1920x96 t16",  960, 48, 1920, 96,  16, 1 },
    { VLC_CODEC_YV12, "YV12 wide 960x48->1920x96 t16",  960, 48, 1920, 96,  16, 1 },
    { VLC_CODEC_I422, "I422 wide 1280x64->2560x96 t16", 1280, 64, 2560, 96, 16, 1 },
    { VLC_CODEC_I444, "I444 wide 640x40->1920x80 t12",  640, 40, 1920, 80,  12, 1 },
};
#define NCFG (sizeof(CFGS) / sizeof(CFGS[0]))

/* Run the backend once. Allocates src (filled from seed) and the caller-owned
 * dst (pre-filled dst_init). Returns the process() result, or -2 on
 * allocation/open failure. CCN 4. */
static int run_zimg(const struct zcfg *c, int src_zc, int dst_zc,
                    uint8_t dst_init, uint32_t seed, zt_pic_t *out)
{
    zt_pic_t src;
    if (zt_pic_alloc(&src, c->chroma, c->sw, c->sh) != 0) return -2;
    if (zt_pic_alloc(out, c->chroma, c->dw, c->dh) != 0) {
        zt_pic_free(&src);
        return -2;
    }
    zt_pic_fill(&src, seed);
    zt_pic_memset(out, dst_init);

    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, c->chroma, c->sw, c->sh, c->dw, c->dh, c->threads, dst_zc);
    ctx.zimg.src_zerocopy = src_zc;
    int rc = -2;
    if (ctx.backend->open(&ctx) == 0) {
        rc = ctx.backend->process(&ctx, &src.pic, &out->pic);
        ctx.backend->close(&ctx);
    }
    zt_pic_free(&src);
    return rc;
}

static int restride_plane(zt_pic_t *pic, int plane, int extra_pitch)
{
    if (plane < 0 || plane >= pic->pic.i_planes || extra_pitch < 0) return -1;
    plane_t *p = &pic->pic.p[plane];
    const int pitch = zt_align_up(p->i_visible_pitch + extra_pitch);
    const size_t bytes = (size_t)pitch * (size_t)p->i_lines;
    uint8_t *data = aligned_alloc(ZT_ALIGN, bytes);
    if (!data) return -1;
    free(pic->buf[plane]);
    pic->buf[plane] = data;
    p->p_pixels = data;
    p->i_pitch = pitch;
    return 0;
}

/* Exercise a legal picture whose second chroma plane has a distinct stride.
 * Returns process() status or -2 on setup/open failure. CCN 6. */
static int run_zimg_asymmetric_pitch(const struct zcfg *c, int src_zc,
                                     int dst_zc, uint32_t seed, zt_pic_t *out)
{
    zt_pic_t src = {0};
    if (zt_pic_alloc(&src, c->chroma, c->sw, c->sh) != 0) return -2;
    if (zt_pic_alloc(out, c->chroma, c->dw, c->dh) != 0
            || restride_plane(&src, 2, 64) != 0
            || restride_plane(out, 2, 128) != 0) {
        zt_pic_free(&src);
        return -2;
    }
    zt_pic_fill(&src, seed);
    zt_pic_memset(out, 0xA5);

    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, c->chroma, c->sw, c->sh, c->dw, c->dh,
                c->threads, dst_zc);
    ctx.zimg.src_zerocopy = src_zc;
    int rc = -2;
    if (ctx.backend->open(&ctx) == 0) {
        rc = ctx.backend->process(&ctx, &src.pic, &out->pic);
        ctx.backend->close(&ctx);
    }
    zt_pic_free(&src);
    return rc;
}

static void set_normalized_crop_format(zt_pic_t *pic, uint32_t chroma,
                                       int physical_w, int physical_h,
                                       int visible_w, int visible_h)
{
    pic->pic.format.i_chroma = chroma;
    pic->pic.format.i_width = (unsigned)physical_w;
    pic->pic.format.i_height = (unsigned)physical_h;
    pic->pic.format.i_x_offset = 0;
    pic->pic.format.i_y_offset = 0;
    pic->pic.format.i_visible_width = (unsigned)visible_w;
    pic->pic.format.i_visible_height = (unsigned)visible_h;
}

static void copy_into_crop(zt_pic_t *dst, const zt_pic_t *src,
                           int x_offset, int y_offset,
                           unsigned sub_w, unsigned sub_h)
{
    for (int k = 0; k < src->pic.i_planes; k++) {
        const unsigned x_shift = k == 0 ? 0 : sub_w;
        const unsigned y_shift = k == 0 ? 0 : sub_h;
        const int x = x_offset >> x_shift;
        const int y = y_offset >> y_shift;
        plane_t *dp = &dst->pic.p[k];
        const plane_t *sp = &src->pic.p[k];
        uint8_t *pixels = dp->p_pixels
                        + (size_t)y * (size_t)dp->i_pitch + (size_t)x;
        up_copy_plane(pixels, dp->i_pitch, sp->p_pixels, sp->i_pitch,
                      sp->i_visible_pitch, sp->i_visible_lines);
    }
}

/* Build physical pictures with poison margins around a visible crop. CCN 6. */
static int run_zimg_cropped(const struct zcfg *c, int src_zc, int dst_zc,
                            uint8_t poison, uint32_t seed, zt_pic_t *out)
{
    const int sx = 9, sy = 7;
    zt_pic_t logical_src = {0}, src = {0};
    memset(out, 0, sizeof *out);
    if (zt_pic_alloc(&logical_src, c->chroma, c->sw, c->sh) != 0
            || zt_pic_alloc(&src, c->chroma, c->sw + 2 * sx,
                            c->sh + 2 * sy) != 0
            || zt_pic_alloc(out, c->chroma, c->dw, c->dh) != 0) {
        zt_pic_free(&logical_src);
        zt_pic_free(&src);
        return -2;
    }

    unsigned sub_w, sub_h;
    int swap;
    if (!up_chroma_to_zimg(c->chroma, &sub_w, &sub_h, &swap)) {
        zt_pic_free(&logical_src);
        zt_pic_free(&src);
        return -2;
    }
    zt_pic_fill(&logical_src, seed);
    zt_pic_memset(&src, poison);
    copy_into_crop(&src, &logical_src, sx, sy, sub_w, sub_h);
    set_normalized_crop_format(&src, c->chroma,
                               c->sw + 2 * sx, c->sh + 2 * sy,
                               c->sw, c->sh);

    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, c->chroma, c->sw, c->sh, c->dw, c->dh,
                c->threads, dst_zc);
    ctx.zimg.src_zerocopy = src_zc;
    ctx.src_coded_w = src.pic.format.i_width;
    ctx.src_coded_h = src.pic.format.i_height;
    ctx.src_x_offset = (unsigned)sx;
    ctx.src_y_offset = (unsigned)sy;
    int rc = -2;
    if (ctx.backend->open(&ctx) == 0) {
        rc = ctx.backend->process(&ctx, &src.pic, &out->pic);
        ctx.backend->close(&ctx);
    }
    zt_pic_free(&logical_src);
    zt_pic_free(&src);
    return rc;
}

/* Count differing bytes in the visible region of two same-geometry pics. */
static size_t cmp_visible(const zt_pic_t *a, const zt_pic_t *b)
{
    size_t diff = 0;
    for (int k = 0; k < a->pic.i_planes; k++) {
        const plane_t *pa = &a->pic.p[k];
        const plane_t *pb = &b->pic.p[k];
        for (int y = 0; y < pa->i_visible_lines; y++) {
            const uint8_t *ra = pa->p_pixels + (size_t)y * (size_t)pa->i_pitch;
            const uint8_t *rb = pb->p_pixels + (size_t)y * (size_t)pb->i_pitch;
            for (int x = 0; x < pa->i_visible_pitch; x++)
                if (ra[x] != rb[x]) diff++;
        }
    }
    return diff;
}

/* Like cmp_visible but also reports the max absolute per-byte delta and the
 * count of "large" diffs (> 4) — distinguishes ±1 rounding noise from a real
 * sub-pixel phase shift / seam. */
static size_t cmp_visible_mag(const zt_pic_t *a, const zt_pic_t *b,
                              int *max_delta, size_t *big_count)
{
    size_t diff = 0; int maxd = 0; size_t big = 0;
    for (int k = 0; k < a->pic.i_planes; k++) {
        const plane_t *pa = &a->pic.p[k];
        const plane_t *pb = &b->pic.p[k];
        for (int y = 0; y < pa->i_visible_lines; y++) {
            const uint8_t *ra = pa->p_pixels + (size_t)y * (size_t)pa->i_pitch;
            const uint8_t *rb = pb->p_pixels + (size_t)y * (size_t)pb->i_pitch;
            for (int x = 0; x < pa->i_visible_pitch; x++) {
                int d = (int)ra[x] - (int)rb[x];
                if (d < 0) d = -d;
                if (d) { diff++; if (d > maxd) maxd = d; if (d > 4) big++; }
            }
        }
    }
    *max_delta = maxd; *big_count = big;
    return diff;
}

/* True if a and b match over their visible region; names the config on diff
 * so a failure points at the offending workload. */
static int same_cfg(const struct zcfg *c, const zt_pic_t *a, const zt_pic_t *b)
{
    size_t diff = cmp_visible(a, b);
    if (diff)
        printf("    [%s] %zu visible bytes differ\n", c->name, diff);
    return diff == 0;
}

/* szc=1 keeps column tiling eligible (REL-10): the tiled path's full-write
 * and determinism are only exercised through the source-direct runs. */
static void test_full_write(void)
{
    BEGIN("full-write: every visible dst byte written (0x00 init == 0xFF init)");
    for (size_t i = 0; i < NCFG; i++) {
        for (int szc = 0; szc < 2; szc++) {
            for (int zc = 0; zc < 2; zc++) {
                zt_pic_t a, b;
                int r0 = run_zimg(&CFGS[i], szc, zc, 0x00, 0xC0FFEEu, &a);
                int r1 = run_zimg(&CFGS[i], szc, zc, 0xFF, 0xC0FFEEu, &b);
                CHECK(r0 == SCALER_PROCESS_OK);
                CHECK(r1 == SCALER_PROCESS_OK);
                if (r0 == SCALER_PROCESS_OK && r1 == SCALER_PROCESS_OK)
                    CHECK(same_cfg(&CFGS[i], &a, &b));
                zt_pic_free(&a);
                zt_pic_free(&b);
            }
        }
    }
    END();
}

static void test_determinism(void)
{
    BEGIN("determinism: same input twice -> identical output");
    for (size_t i = 0; i < NCFG; i++) {
        for (int szc = 0; szc < 2; szc++) {
            zt_pic_t a, b;
            int r0 = run_zimg(&CFGS[i], szc, 0, 0x00, 0xBEEF01u, &a);
            int r1 = run_zimg(&CFGS[i], szc, 0, 0x00, 0xBEEF01u, &b);
            CHECK(r0 == SCALER_PROCESS_OK && r1 == SCALER_PROCESS_OK);
            if (r0 == SCALER_PROCESS_OK && r1 == SCALER_PROCESS_OK)
                CHECK(same_cfg(&CFGS[i], &a, &b));
            zt_pic_free(&a);
            zt_pic_free(&b);
        }
    }
    END();
}

static void test_zerocopy_matches_copyout(void)
{
    BEGIN("zerocopy-dst output byte-identical to copy-out output");
    for (size_t i = 0; i < NCFG; i++) {
        zt_pic_t a, b;
        int r0 = run_zimg(&CFGS[i], 0, 0, 0x00, 0x5EED77u, &a);  /* dst copy-out */
        int r1 = run_zimg(&CFGS[i], 0, 1, 0x00, 0x5EED77u, &b);  /* dst zero-copy */
        CHECK(r0 == SCALER_PROCESS_OK && r1 == SCALER_PROCESS_OK);
        if (r0 == SCALER_PROCESS_OK && r1 == SCALER_PROCESS_OK)
            CHECK(same_cfg(&CFGS[i], &a, &b));
        zt_pic_free(&a);
        zt_pic_free(&b);
    }
    END();
}

/* Source zero-copy (graph reads the src picture directly) must produce output
 * byte-identical to the copy-in path; and full zero-copy (src+dst) must match
 * full copy. Validates the new src_zerocopy plumbing end to end. */
static void test_src_zerocopy_matches_copy(void)
{
    BEGIN("source zero-copy + full zero-copy byte-identical to copy path");
    for (size_t i = 0; i < NCFG; i++) {
        zt_pic_t a, b, c;
        int r0 = run_zimg(&CFGS[i], 0, 0, 0x00, 0x70DDu, &a);  /* all copy */
        int r1 = run_zimg(&CFGS[i], 1, 0, 0x00, 0x70DDu, &b);  /* src zero-copy */
        int r2 = run_zimg(&CFGS[i], 1, 1, 0x00, 0x70DDu, &c);  /* full zero-copy */
        CHECK(r0 == SCALER_PROCESS_OK && r1 == SCALER_PROCESS_OK
              && r2 == SCALER_PROCESS_OK);
        if (r0 == SCALER_PROCESS_OK && r1 == SCALER_PROCESS_OK
                && r2 == SCALER_PROCESS_OK) {
            if (CFGS[i].col_tiled) {
                /* SYS-5: src_zerocopy=0 runs a rows-only grid here — a
                 * DIFFERENT graph partition from the tiled zero-copy
                 * runs, so `a` is not byte-comparable (seam oracle
                 * covers grid-vs-single-graph deltas). The two tiled
                 * runs must still agree exactly, and the copy-path run
                 * must have succeeded (checked above). */
                CHECK(same_cfg(&CFGS[i], &b, &c));
            } else {
                CHECK(same_cfg(&CFGS[i], &a, &b));
                CHECK(same_cfg(&CFGS[i], &a, &c));
            }
        }
        zt_pic_free(&a);
        zt_pic_free(&b);
        zt_pic_free(&c);
    }
    END();
}

static void test_asymmetric_chroma_pitches(void)
{
    BEGIN("independent U/V strides work through graph and copy paths");
    const struct zcfg *configs[] = { &CFGS[1], &CFGS[4] };
    const int modes[][2] = { { 0, 1 }, { 1, 0 } };
    for (size_t c = 0; c < sizeof configs / sizeof configs[0]; c++) {
        for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
            zt_pic_t expected = {0}, actual = {0};
            int expected_rc = run_zimg(configs[c], modes[i][0], modes[i][1],
                                       0xA5, 0xA5C440u, &expected);
            int actual_rc = run_zimg_asymmetric_pitch(
                configs[c], modes[i][0], modes[i][1], 0xA5C440u, &actual);
            CHECK(expected_rc == SCALER_PROCESS_OK
                  && actual_rc == SCALER_PROCESS_OK);
            if (expected_rc == SCALER_PROCESS_OK
                    && actual_rc == SCALER_PROCESS_OK)
                CHECK(same_cfg(configs[c], &expected, &actual));
            zt_pic_free(&expected);
            zt_pic_free(&actual);
        }
    }
    END();
}

static void test_cropped_origin_and_poison_margins(void)
{
    BEGIN("cropped I420/YV12 uses negotiated origin despite zero picture offsets");
    const struct zcfg *configs[] = { &CFGS[1], &CFGS[4] };
    const int modes[][2] = { { 0, 0 }, { 1, 1 } };
    for (size_t c = 0; c < sizeof configs / sizeof configs[0]; c++) {
        for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
            zt_pic_t expected = {0}, actual = {0};
            int expected_rc = run_zimg(configs[c], modes[m][0], modes[m][1],
                                       0x00, 0xC20F11u, &expected);
            int actual_rc = run_zimg_cropped(
                configs[c], modes[m][0], modes[m][1], 0xA5,
                0xC20F11u, &actual);
            CHECK(expected_rc == SCALER_PROCESS_OK
                  && actual_rc == SCALER_PROCESS_OK);
            if (expected_rc == SCALER_PROCESS_OK
                    && actual_rc == SCALER_PROCESS_OK)
                CHECK(cmp_visible(&expected, &actual) == 0);
            zt_pic_free(&expected);
            zt_pic_free(&actual);
        }
    }
    END();
}

static void test_cropped_tiled_alignment_fallback(void)
{
    BEGIN("unaligned crop safely changes a tiled graph to aligned copy I/O");
    const struct zcfg *c = &CFGS[13];
    zt_pic_t expected = {0}, actual = {0};
    int expected_rc = run_zimg(c, 0, 0, 0x00, 0xC20F12u, &expected);
    int actual_rc = run_zimg_cropped(c, 1, 1, 0xA5, 0xC20F12u, &actual);
    CHECK(expected_rc == SCALER_PROCESS_OK && actual_rc == SCALER_PROCESS_OK);
    if (expected_rc == SCALER_PROCESS_OK && actual_rc == SCALER_PROCESS_OK)
        CHECK(cmp_visible(&expected, &actual) == 0);
    zt_pic_free(&expected);
    zt_pic_free(&actual);
    END();
}

/* supports(): planar YUV yes, packed/semiplanar no. Covers zimg_supports
 * (the backend never calls it directly in the other tests). */
static void test_supports(void)
{
    BEGIN("supports(): planar YUV yes, RGB/NV12 no");
    const scaler_backend_t *be = &scaler_backend_zimg_impl;
    CHECK(be->supports(VLC_CODEC_I420, UP_ALGO_LANCZOS));
    CHECK(be->supports(VLC_CODEC_YV12, UP_ALGO_LANCZOS));
    CHECK(be->supports(VLC_CODEC_I422, UP_ALGO_LANCZOS));
    CHECK(be->supports(VLC_CODEC_I444, UP_ALGO_LANCZOS));
    CHECK(!be->supports(VLC_CODEC_NV12, UP_ALGO_LANCZOS));
    CHECK(!be->supports(VLC_CODEC_RGBA, UP_ALGO_LANCZOS));
    END();
}

/* Every resample algorithm builds a graph and processes (covers the
 * AlgoToZimg mapping arms; the config tests all run Lanczos). */
static void test_all_algos(void)
{
    BEGIN("all resample algos build + process");
    const int algos[] = { UP_ALGO_FAST_BILINEAR, UP_ALGO_BICUBIC,
                          UP_ALGO_LANCZOS, UP_ALGO_SPLINE36 };
    for (size_t i = 0; i < sizeof(algos) / sizeof(algos[0]); i++) {
        zt_pic_t src, dst;
        int ok = zt_pic_alloc(&src, VLC_CODEC_I420, 640, 360) == 0
              && zt_pic_alloc(&dst, VLC_CODEC_I420, 1280, 720) == 0;
        CHECK(ok);
        if (ok) {
            zt_pic_fill(&src, 0x99u);
            scaler_ctx_t ctx;
            zt_ctx_init(&ctx, VLC_CODEC_I420, 640, 360, 1280, 720, 4, 1);
            ctx.algo = algos[i];
            CHECK(ctx.backend->open(&ctx) == 0);
            CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
                  == SCALER_PROCESS_OK);
            ctx.backend->close(&ctx);
        }
        zt_pic_free(&src);
        zt_pic_free(&dst);
    }
    END();
}

static void test_transient_preflight_recovers(void)
{
    BEGIN("malformed frame is transient; same backend accepts the next frame");
    zt_pic_t src = {0}, dst = {0};
    int ok = zt_pic_alloc(&src, VLC_CODEC_I420, 640, 360) == 0
          && zt_pic_alloc(&dst, VLC_CODEC_I420, 1280, 720) == 0;
    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, VLC_CODEC_I420, 640, 360, 1280, 720, 4, 1);
    CHECK(ok);
    if (ok) {
        zt_pic_fill(&src, 0x7711u);
        CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
              == SCALER_PROCESS_FATAL);
        int open_rc = ctx.backend->open(&ctx);
        CHECK(open_rc == 0);
        if (open_rc == 0) {
            CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
                  == SCALER_PROCESS_OK);
            uint8_t *src_u = src.pic.p[1].p_pixels;
            src.pic.p[1].p_pixels = NULL;
            CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
                  == SCALER_PROCESS_TRANSIENT);
            src.pic.p[1].p_pixels = src_u;

            int src_lines = src.pic.p[0].i_lines;
            src.pic.p[0].i_lines = ctx.src_h - 1;
            CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
                  == SCALER_PROCESS_TRANSIENT);
            src.pic.p[0].i_lines = src_lines;

            unsigned src_width = src.pic.format.i_width;
            src.pic.format.i_width = (unsigned)ctx.src_w - 1;
            CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
                  == SCALER_PROCESS_TRANSIENT);
            src.pic.format.i_width = src_width;

            int dst_v_pitch = dst.pic.p[2].i_pitch;
            dst.pic.p[2].i_pitch = 0;
            CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
                  == SCALER_PROCESS_TRANSIENT);
            dst.pic.p[2].i_pitch = dst_v_pitch;
            CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
                  == SCALER_PROCESS_OK);
            ctx.backend->close(&ctx);
        }
    }
    zt_pic_free(&src);
    zt_pic_free(&dst);
    END();
}

/* Drop RLIMIT_NPROC so worker pthread_create fails: lazy_init must clean up
 * (sem destroyed, graph/tmp freed) and the backend must fail gracefully and
 * stay failed (sticky), with no leak/crash under ASan. Covers the
 * construction error + lazy-init-failed + process-failure paths. */
static void test_construction_pthread_fail(void)
{
    BEGIN("worker construction survives pthread_create failure (RLIMIT_NPROC)");
    struct rlimit old;
    if (getrlimit(RLIMIT_NPROC, &old) != 0) { END(); return; }
    struct rlimit lim = old;
    lim.rlim_cur = 1;
    if (setrlimit(RLIMIT_NPROC, &lim) != 0) { END(); return; }

    zt_pic_t src, dst;
    int ok = zt_pic_alloc(&src, VLC_CODEC_I420, 854, 480) == 0
          && zt_pic_alloc(&dst, VLC_CODEC_I420, 1920, 1080) == 0;
    int rc1 = SCALER_PROCESS_OK, rc2 = SCALER_PROCESS_OK;
    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, VLC_CODEC_I420, 854, 480, 1920, 1080, 16, 1);
    if (ok && ctx.backend->open(&ctx) == 0) {
        zt_pic_fill(&src, 0x33u);
        rc1 = ctx.backend->process(&ctx, &src.pic, &dst.pic);
        rc2 = ctx.backend->process(&ctx, &src.pic, &dst.pic);  /* sticky */
        ctx.backend->close(&ctx);
    }
    setrlimit(RLIMIT_NPROC, &old);   /* restore before any later test */

    CHECK(ok);
    CHECK(rc1 == SCALER_PROCESS_FATAL);   /* lazy init failed */
    CHECK(rc2 == SCALER_PROCESS_FATAL);   /* sticky */
    zt_pic_free(&src);
    zt_pic_free(&dst);
    END();
}

static void test_barrier_failure_drains_and_sticks(void)
{
    BEGIN("barrier failure drains frame, stops pool, and sticks");
    const struct zcfg *c = &CFGS[1];
    const uint32_t seed = 0xC04B4Au;
    zt_pic_t expected = {0}, src = {0}, dst = {0}, untouched = {0};
    int expected_rc = run_zimg(c, 0, 1, 0x00, seed, &expected);
    int alloc_ok = zt_pic_alloc(&src, c->chroma, c->sw, c->sh) == 0
        && zt_pic_alloc(&dst, c->chroma, c->dw, c->dh) == 0
        && zt_pic_alloc(&untouched, c->chroma, c->dw, c->dh) == 0;

    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, c->chroma, c->sw, c->sh,
                c->dw, c->dh, c->threads, 1);
    ctx.zimg.src_zerocopy = 0;
    int open_rc = alloc_ok ? ctx.backend->open(&ctx) : -1;
    CHECK(expected_rc == SCALER_PROCESS_OK && alloc_ok && open_rc == 0);
    if (expected_rc == SCALER_PROCESS_OK && alloc_ok && open_rc == 0) {
        zt_pic_fill(&src, seed);
        CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
              == SCALER_PROCESS_OK);

        zt_pic_memset(&dst, 0x00);
        barrier_fault_inject_next_dispatch();
        CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
              == SCALER_PROCESS_FATAL);
        CHECK(cmp_visible(&expected, &dst) == 0);
        CHECK(barrier_fault_injection_consumed());

        zt_pic_memset(&dst, 0xA5);
        zt_pic_memset(&untouched, 0xA5);
        CHECK(ctx.backend->process(&ctx, &src.pic, &dst.pic)
              == SCALER_PROCESS_FATAL);
        CHECK(cmp_visible(&untouched, &dst) == 0);
    }
    if (open_rc == 0) ctx.backend->close(&ctx);
    zt_pic_free(&expected);
    zt_pic_free(&src);
    zt_pic_free(&dst);
    zt_pic_free(&untouched);
    END();
}

/* open() rejects a chroma the backend can't handle (the ChromaToZimg gate). */
static void test_open_rejects_unsupported(void)
{
    BEGIN("open() rejects an unsupported chroma");
    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, VLC_CODEC_NV12, 640, 360, 1280, 720, 4, 1);
    CHECK(ctx.backend->open(&ctx) != 0);
    END();
}

/* An extreme src->dst ratio (8 -> 1080 luma rows) drives the stripe-bounds
 * math to its limit. Whether stripe 0's source range collapses to zero rows
 * depends on how many stripes zimg_open creates, which is clamped to the
 * machine's core count (up_threads_decide) — so on a many-core host it
 * degenerates and lazy_init returns -1, while on a 1-2 core CI runner the few
 * fat stripes stay valid and it succeeds. Either way the backend must not
 * crash or leak (ASan/UBSan enforce); accept both results. */
static void test_extreme_ratio_no_crash(void)
{
    BEGIN("extreme src->dst ratio: graceful success or fatal status");
    zt_pic_t src, dst;
    int ok = zt_pic_alloc(&src, VLC_CODEC_I420, 100, 8) == 0
          && zt_pic_alloc(&dst, VLC_CODEC_I420, 1920, 1080) == 0;
    int rc = 0;
    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, VLC_CODEC_I420, 100, 8, 1920, 1080, 64, 1);
    if (ok && ctx.backend->open(&ctx) == 0) {
        zt_pic_fill(&src, 0x55u);
        rc = ctx.backend->process(&ctx, &src.pic, &dst.pic);
        ctx.backend->close(&ctx);
    }
    CHECK(ok);
    CHECK(rc == SCALER_PROCESS_OK || rc == SCALER_PROCESS_FATAL);
    zt_pic_free(&src);
    zt_pic_free(&dst);
    END();
}

/* Smooth 2D gradient — realistic low-frequency content, unlike the xorshift
 * noise of zt_pic_fill. A sub-pixel phase error shows up as a SMALL delta here
 * (proportional to the local slope), vs a huge delta on noise. */
static void zt_pic_fill_smooth(zt_pic_t *tp)
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

/* Run one config with an explicit worker-thread count (overriding c->threads),
 * dst copy-out, src at the production default (source-direct, so column
 * tiling stays eligible — REL-10). Fills `out`. Returns 0 on success. Used by
 * the SCAL-3 seam oracle: threads=1 is the single-graph untiled reference.
 * smooth=1 uses a gradient (realistic), smooth=0 uses noise (worst case). */
static int run_zimg_threads_in(const struct zcfg *c, int threads, int smooth,
                               uint32_t seed, zt_pic_t *out)
{
    zt_pic_t src;
    if (zt_pic_alloc(&src, c->chroma, c->sw, c->sh) != 0) return -2;
    if (zt_pic_alloc(out, c->chroma, c->dw, c->dh) != 0) {
        zt_pic_free(&src);
        return -2;
    }
    if (smooth) zt_pic_fill_smooth(&src);
    else        zt_pic_fill(&src, seed);
    zt_pic_memset(out, 0x00);

    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, c->chroma, c->sw, c->sh, c->dw, c->dh, threads, 0);
    int rc = -2;
    if (ctx.backend->open(&ctx) == 0) {
        rc = ctx.backend->process(&ctx, &src.pic, &out->pic);
        ctx.backend->close(&ctx);
    }
    zt_pic_free(&src);
    return rc;
}

/*
 * SCAL-3 SEAM ORACLE. A tiled resample (N stripes) is NOT byte-identical to
 * the single-graph (threads=1) resample: each tile restarts zimg's resize
 * coordinate origin, so tile output carries a sub-pixel PHASE rounding at the
 * boundary. On real (low-frequency) content that rounding is bounded to a few
 * code values out of 255 — imperceptible; only on uncorrelated NOISE does a
 * sub-pixel shift blow up to large per-pixel deltas.
 *
 * So the seam criterion is NOT byte-identity but a bounded delta on SMOOTH
 * content: maxdelta <= SEAM_MAX_DELTA. This is the gate that must pass before
 * (and after) column tiling is added — column tiles add the same bounded
 * horizontal phase rounding and must stay under the same ceiling.
 */
#define SEAM_MAX_DELTA 6
static void test_tiling_matches_untiled(void)
{
    BEGIN("tiled vs single-graph seam <= 6 (SEAM_MAX_DELTA) on smooth content");
    static const int TCOUNTS[] = { 2, 4, 8, 16 };
    for (size_t i = 0; i < NCFG; i++) {
        zt_pic_t ref;
        if (run_zimg_threads_in(&CFGS[i], 1, 1, 0, &ref)
                != SCALER_PROCESS_OK) { CHECK(0); continue; }
        for (size_t t = 0; t < sizeof TCOUNTS / sizeof *TCOUNTS; t++) {
            zt_pic_t tiled;
            int r = run_zimg_threads_in(&CFGS[i], TCOUNTS[t], 1, 0, &tiled);
            CHECK(r == SCALER_PROCESS_OK);
            if (r == SCALER_PROCESS_OK) {
                int maxd = 0; size_t big = 0;
                cmp_visible_mag(&ref, &tiled, &maxd, &big);
                if (maxd > SEAM_MAX_DELTA)
                    printf("    [%s] t=%d SMOOTH seam maxdelta=%d (>%d) big=%zu\n",
                           CFGS[i].name, TCOUNTS[t], maxd, SEAM_MAX_DELTA, big);
                CHECK(maxd <= SEAM_MAX_DELTA);
            }
            zt_pic_free(&tiled);
        }
        zt_pic_free(&ref);
    }
    END();
}

/* Run one config with CPU pinning enabled (SCAL-4); fills `out`. Mirrors
 * run_zimg but sets ctx.pin_cpus = 1. Returns 0 on success. */
static int run_zimg_pinned(const struct zcfg *c, uint32_t seed, zt_pic_t *out)
{
    zt_pic_t src;
    if (zt_pic_alloc(&src, c->chroma, c->sw, c->sh) != 0) return -2;
    if (zt_pic_alloc(out, c->chroma, c->dw, c->dh) != 0) {
        zt_pic_free(&src);
        return -2;
    }
    zt_pic_fill(&src, seed);
    zt_pic_memset(out, 0x00);

    scaler_ctx_t ctx;
    zt_ctx_init(&ctx, c->chroma, c->sw, c->sh, c->dw, c->dh, c->threads, 0);
    ctx.pin_cpus = 1;
    int rc = -2;
    if (ctx.backend->open(&ctx) == 0) {
        rc = ctx.backend->process(&ctx, &src.pic, &out->pic);
        ctx.backend->close(&ctx);
    }
    zt_pic_free(&src);
    return rc;
}

/* SCAL-4: pinning is an optimization only — output must be byte-identical to
 * the unpinned path, and the pin/spawn/teardown must be crash- and race-free
 * (this case is what the TSan harness exercises for the affinity code). */
static void test_pin_cpus_matches(void)
{
    BEGIN("CPU pinning output byte-identical to unpinned (and race-free)");
    for (size_t i = 0; i < NCFG; i++) {
        zt_pic_t a, b;
        /* src_zc=1 matches run_zimg_pinned's default so both runs use the
         * same (possibly column-tiled) grid — byte-identity requires it. */
        int r0 = run_zimg(&CFGS[i], 1, 0, 0x00, 0xC0FFEEu, &a);
        int r1 = run_zimg_pinned(&CFGS[i], 0xC0FFEEu, &b);
        CHECK(r0 == SCALER_PROCESS_OK && r1 == SCALER_PROCESS_OK);
        if (r0 == SCALER_PROCESS_OK && r1 == SCALER_PROCESS_OK)
            CHECK(same_cfg(&CFGS[i], &a, &b));
        zt_pic_free(&a);
        zt_pic_free(&b);
    }
    END();
}

int main(void)
{
    printf("Running scaler_zimg invariant tests (%zu configs)...\n", NCFG);
    test_full_write();
    test_determinism();
    test_zerocopy_matches_copyout();
    test_src_zerocopy_matches_copy();
    test_asymmetric_chroma_pitches();
    test_cropped_origin_and_poison_margins();
    test_cropped_tiled_alignment_fallback();
    test_supports();
    test_all_algos();
    test_transient_preflight_recovers();
    test_open_rejects_unsupported();
    test_extreme_ratio_no_crash();
    test_construction_pthread_fail();
    test_barrier_failure_drains_and_sticks();
    test_pin_cpus_matches();
    test_tiling_matches_untiled();
    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
