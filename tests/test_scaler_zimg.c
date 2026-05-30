// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_scaler_zimg.c — invariant tests for the slice-threaded zimg backend
 *****************************************************************************
 * scaler_zimg.c had no automated coverage because it is VLC-typed and
 * resamples real frames. This harness drives backend->open/process/close on
 * hand-built picture_t objects (see zimg_test_util.h) so the threaded path
 * runs under ASan/UBSan (and a TSan build) across many (chroma, dims,
 * thread-count, zerocopy) combinations.
 *
 * The per-stripe graphs do NOT produce byte-identical output to a single
 * full-frame resize (independent boundary handling per stripe), so we assert
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

#include <stdio.h>

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
     * (I420/YV12/I422) require even dims, which production always satisfies
     * via up__clamp_even in up_compute_target_dims; odd dst there is not a
     * real case. */
    { VLC_CODEC_I444, "I444 odd 853x481->1281x721 t8", 853, 481, 1281, 721, 8 },
    { VLC_CODEC_I420, "I420 tiny 64x64->128x128 t8",   64,  64,  128,  128, 8 },
    { VLC_CODEC_I420, "I420 clamp 100x16->200x32 t64", 100, 16,  200,  32, 64 },
};
#define NCFG (sizeof(CFGS) / sizeof(CFGS[0]))

/* Run the backend once. Allocates src (filled from seed) and the caller-owned
 * dst (pre-filled dst_init). Returns the process() result, or -2 on
 * allocation/open failure. CCN 4. */
static int run_zimg(const struct zcfg *c, int zc, uint8_t dst_init,
                    uint32_t seed, zt_pic_t *out)
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
    zt_ctx_init(&ctx, c->chroma, c->sw, c->sh, c->dw, c->dh, c->threads, zc);
    int rc = -2;
    if (ctx.backend->open(&ctx) == 0) {
        rc = ctx.backend->process(&ctx, &src.pic, &out->pic);
        ctx.backend->close(&ctx);
    }
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

static void test_full_write(void)
{
    BEGIN("full-write: every visible dst byte written (0x00 init == 0xFF init)");
    for (size_t i = 0; i < NCFG; i++) {
        for (int zc = 0; zc < 2; zc++) {
            zt_pic_t a, b;
            int r0 = run_zimg(&CFGS[i], zc, 0x00, 0xC0FFEEu, &a);
            int r1 = run_zimg(&CFGS[i], zc, 0xFF, 0xC0FFEEu, &b);
            CHECK(r0 == 0);
            CHECK(r1 == 0);
            if (r0 == 0 && r1 == 0)
                CHECK(cmp_visible(&a, &b) == 0);
            zt_pic_free(&a);
            zt_pic_free(&b);
        }
    }
    END();
}

static void test_determinism(void)
{
    BEGIN("determinism: same input twice -> identical output");
    for (size_t i = 0; i < NCFG; i++) {
        zt_pic_t a, b;
        int r0 = run_zimg(&CFGS[i], 0, 0x00, 0xBEEF01u, &a);
        int r1 = run_zimg(&CFGS[i], 0, 0x00, 0xBEEF01u, &b);
        CHECK(r0 == 0 && r1 == 0);
        if (r0 == 0 && r1 == 0)
            CHECK(cmp_visible(&a, &b) == 0);
        zt_pic_free(&a);
        zt_pic_free(&b);
    }
    END();
}

static void test_zerocopy_matches_copyout(void)
{
    BEGIN("zerocopy-dst output byte-identical to copy-out output");
    for (size_t i = 0; i < NCFG; i++) {
        zt_pic_t a, b;
        int r0 = run_zimg(&CFGS[i], 0, 0x00, 0x5EED77u, &a);  /* copy-out */
        int r1 = run_zimg(&CFGS[i], 1, 0x00, 0x5EED77u, &b);  /* zero-copy */
        CHECK(r0 == 0 && r1 == 0);
        if (r0 == 0 && r1 == 0)
            CHECK(cmp_visible(&a, &b) == 0);
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
    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
