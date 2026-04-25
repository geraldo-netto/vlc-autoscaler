/*****************************************************************************
 * test_usm.c — unit tests for the unsharp-mask logic
 *****************************************************************************/

#include "../src/usm.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define CHECK_EQ(a, b) do { \
        long _a = (long)(a), _b = (long)(b); \
        if (_a != _b) { \
            printf("    %s:%d: %s (=%ld) != %s (=%ld)\n", \
                   __FILE__, __LINE__, #a, _a, #b, _b); \
            g_cur_fail = 1; \
        } \
    } while (0)

/* ---------------------- pct_to_q8 ---------------------- */

static void test_amount_conversion(void)
{
    BEGIN("amount_pct_to_q8: 0/30/100/200 round-trip");
    CHECK_EQ(up_usm_amount_pct_to_q8(0),    0);
    CHECK_EQ(up_usm_amount_pct_to_q8(30),  76);   /* 30*256/100 = 76.8 -> 76 */
    CHECK_EQ(up_usm_amount_pct_to_q8(100), 256);
    CHECK_EQ(up_usm_amount_pct_to_q8(200), 512);

    /* Out of range clamps. */
    CHECK_EQ(up_usm_amount_pct_to_q8(-50),  0);
    CHECK_EQ(up_usm_amount_pct_to_q8(99999), 512);
    END();
}

/* ---------------------- workspace_size ---------------------- */

static void test_workspace_size(void)
{
    BEGIN("workspace_size: typical and overflow");
    CHECK_EQ(up_usm_workspace_size(1920, 1080), (size_t)1920 * 1080);
    CHECK_EQ(up_usm_workspace_size(0, 100), 0);
    CHECK_EQ(up_usm_workspace_size(100, 0), 0);
    CHECK_EQ(up_usm_workspace_size(-1, 100), 0);
    CHECK_EQ(up_usm_workspace_size(INT_MAX, INT_MAX), 0); /* overflow */
    END();
}

/* ---------------------- horizontal blur (helper) ---------------------- */

static void test_hblur_row(void)
{
    BEGIN("hblur_row: known kernel output");
    const uint8_t in[5]  = {0, 0, 100, 0, 0};
    uint8_t out[5] = {0};
    up_usm__hblur_row(out, in, 5);
    /* Manual:
     *   out[0] = (0*3 + 0    + 2)/4 = 0
     *   out[1] = (0   + 2*0  + 100 + 2)/4 = 25
     *   out[2] = (0   + 2*100 +  0 + 2)/4 = 50
     *   out[3] = (100 + 2*0  +  0 + 2)/4 = 25
     *   out[4] = (0   + 0*3  +  2)/4 = 0
     */
    CHECK_EQ(out[0],  0);
    CHECK_EQ(out[1], 25);
    CHECK_EQ(out[2], 50);
    CHECK_EQ(out[3], 25);
    CHECK_EQ(out[4],  0);

    /* width == 1 is identity. */
    uint8_t one_in = 42, one_out = 0;
    up_usm__hblur_row(&one_out, &one_in, 1);
    CHECK_EQ(one_out, 42);
    END();
}

static void test_hblur_constant_invariant(void)
{
    BEGIN("hblur_row: constant input -> constant output");
    uint8_t in[16], out[16];
    for (int v = 0; v < 256; v += 17) {
        memset(in, v, sizeof in);
        memset(out, 0xAB, sizeof out);
        up_usm__hblur_row(out, in, sizeof in);
        for (size_t i = 0; i < sizeof out; i++)
            CHECK_EQ(out[i], v);
    }
    END();
}

/* ---------------------- apply_plane ---------------------- */

static void test_apply_amount_zero_is_identity(void)
{
    BEGIN("apply_plane: amount=0 produces exact copy");
    uint8_t src[8 * 6], dst[8 * 6], ws[8 * 6];
    for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 11);
    memset(dst, 0xAB, sizeof dst);

    int rc = up_usm_apply_plane(dst, 8, src, 8, 8, 6, 0, ws);
    CHECK_EQ(rc, 1);
    CHECK_EQ(memcmp(dst, src, sizeof src), 0);
    END();
}

static void test_apply_constant_input(void)
{
    BEGIN("apply_plane: constant input is unchanged for any amount");
    uint8_t src[8 * 6], dst[8 * 6], ws[8 * 6];
    memset(src, 128, sizeof src);
    /* Try several amounts; high-pass of constant is zero everywhere. */
    int amounts[] = { 0, 76, 256, 512, 4096 };
    for (size_t i = 0; i < sizeof amounts / sizeof amounts[0]; i++) {
        memset(dst, 0xAB, sizeof dst);
        int rc = up_usm_apply_plane(dst, 8, src, 8, 8, 6, amounts[i], ws);
        CHECK_EQ(rc, 1);
        for (size_t j = 0; j < sizeof dst; j++)
            CHECK_EQ(dst[j], 128);
    }
    END();
}

static void test_apply_impulse_amount_one(void)
{
    BEGIN("apply_plane: 3x3 impulse, amount=1.0 (q8=256)");
    /*
     * src:                workspace after pass 1:
     *   0   0   0           0   0   0
     *   0 100   0          25  50  25
     *   0   0   0           0   0   0
     *
     * Vertical blur at (1,1):  (0 + 2*50 + 0 + 2)/4 = 25
     *   high-pass = 100 - 25 = 75
     *   sharpened = 100 + 75 = 175
     *
     * At (0,0): blur = (ws[0][0] + 2*ws[0][0] + ws[1][0] + 2)/4
     *         = (0 + 0 + 25 + 2)/4 = 6
     *   high = 0 - 6 = -6, sharpened = 0 + (-6) = -6 -> clamped 0
     */
    const uint8_t src[3 * 3] = {
        0,   0, 0,
        0, 100, 0,
        0,   0, 0,
    };
    uint8_t dst[3 * 3];
    uint8_t ws[3 * 3];

    memset(dst, 0xAB, sizeof dst);
    int rc = up_usm_apply_plane(dst, 3, src, 3, 3, 3, 256, ws);
    CHECK_EQ(rc, 1);
    /* Centre pixel is sharpened. */
    CHECK_EQ(dst[1 * 3 + 1], 175);
    /* Corner stays 0 (negative high-freq clamped). */
    CHECK_EQ(dst[0 * 3 + 0], 0);
    /* Centre of bottom row also clamps to 0. */
    CHECK_EQ(dst[2 * 3 + 1], 0);
    END();
}

static void test_apply_saturation(void)
{
    BEGIN("apply_plane: extreme high-pass clips at 0 and 255");
    /* Very strong amount should saturate, not wrap. */
    const uint8_t src[3 * 3] = {
        0,   0,    0,
        0, 255,    0,
        0,   0,    0,
    };
    uint8_t dst[3 * 3];
    uint8_t ws[3 * 3];

    int rc = up_usm_apply_plane(dst, 3, src, 3, 3, 3, 4096, ws);
    CHECK_EQ(rc, 1);

    for (int i = 0; i < 9; i++) {
        CHECK(dst[i] <= 255);  /* trivially true for uint8_t but documents intent */
    }
    /* Centre stays at 255. */
    CHECK_EQ(dst[1 * 3 + 1], 255);
    /* Corners go to 0 (large negative high-pass). */
    CHECK_EQ(dst[0], 0);
    CHECK_EQ(dst[8], 0);
    END();
}

static void test_apply_in_place_equals_out_of_place(void)
{
    BEGIN("apply_plane: in-place and out-of-place produce identical output");
    enum { W = 17, H = 13 };  /* odd dims to flush out edge bugs */
    uint8_t src[W * H], a[W * H], b[W * H], ws[W * H];

    /* Pseudo-random fill (deterministic). */
    uint32_t r = 0xC0FFEEu;
    for (int i = 0; i < W * H; i++) {
        r = r * 1103515245u + 12345u;
        src[i] = (uint8_t)(r >> 16);
    }
    memcpy(a, src, sizeof src);  /* in-place input */
    memset(b, 0xAB, sizeof b);   /* out-of-place output */

    int amounts[] = { 50, 256, 1000 };
    for (size_t i = 0; i < sizeof amounts / sizeof amounts[0]; i++) {
        memcpy(a, src, sizeof src);
        int rc1 = up_usm_apply_plane(a, W, a, W, W, H, amounts[i], ws);
        int rc2 = up_usm_apply_plane(b, W, src, W, W, H, amounts[i], ws);
        CHECK_EQ(rc1, 1);
        CHECK_EQ(rc2, 1);
        CHECK_EQ(memcmp(a, b, sizeof a), 0);
    }
    END();
}

static void test_apply_stride_greater_than_width(void)
{
    BEGIN("apply_plane: stride > width works (padded buffers)");
    enum { W = 5, H = 4, STRIDE = 8 };
    uint8_t src[STRIDE * H], dst[STRIDE * H], ws[W * H];

    /* Fill payload with a ramp and padding with sentinel. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) src[y * STRIDE + x] = (uint8_t)(y * 30 + x * 10);
        for (int x = W; x < STRIDE; x++) src[y * STRIDE + x] = 0xCC;
    }
    memset(dst, 0xAB, sizeof dst);

    int rc = up_usm_apply_plane(dst, STRIDE, src, STRIDE, W, H, 256, ws);
    CHECK_EQ(rc, 1);

    /* Padding bytes in dst must NOT have been touched. */
    for (int y = 0; y < H; y++) {
        for (int x = W; x < STRIDE; x++) {
            CHECK_EQ(dst[y * STRIDE + x], 0xAB);
        }
    }
    /* Source padding must be untouched. */
    for (int y = 0; y < H; y++) {
        for (int x = W; x < STRIDE; x++) {
            CHECK_EQ(src[y * STRIDE + x], 0xCC);
        }
    }
    END();
}

static void test_apply_invalid_inputs(void)
{
    BEGIN("apply_plane: invalid inputs return 0");
    uint8_t buf[16], ws[16];

    /* NULL pointers. */
    CHECK_EQ(up_usm_apply_plane(NULL, 4, buf, 4, 4, 4, 256, ws), 0);
    CHECK_EQ(up_usm_apply_plane(buf, 4, NULL, 4, 4, 4, 256, ws), 0);
    /* NULL workspace with non-zero amount. */
    CHECK_EQ(up_usm_apply_plane(buf, 4, buf, 4, 4, 4, 256, NULL), 0);
    /* NULL workspace with amount=0 is OK (identity path). */
    CHECK_EQ(up_usm_apply_plane(buf, 4, buf, 4, 4, 4, 0, NULL), 1);

    /* Zero / negative dims. */
    CHECK_EQ(up_usm_apply_plane(buf, 4, buf, 4, 0, 4, 256, ws), 0);
    CHECK_EQ(up_usm_apply_plane(buf, 4, buf, 4, 4, 0, 256, ws), 0);
    CHECK_EQ(up_usm_apply_plane(buf, 4, buf, 4, -1, 4, 256, ws), 0);

    /* Stride less than width. */
    CHECK_EQ(up_usm_apply_plane(buf, 2, buf, 4, 4, 4, 256, ws), 0);
    CHECK_EQ(up_usm_apply_plane(buf, 4, buf, 2, 4, 4, 256, ws), 0);
    END();
}

static void test_apply_1x1_plane(void)
{
    BEGIN("apply_plane: 1x1 plane is identity for any amount");
    uint8_t src = 42, dst = 0, ws = 0;
    int rc = up_usm_apply_plane(&dst, 1, &src, 1, 1, 1, 256, &ws);
    CHECK_EQ(rc, 1);
    /* hblur 1px = identity; vertical blur of single row = identity;
     * high-pass = 0 -> output = src. */
    CHECK_EQ(dst, 42);
    END();
}

static void test_apply_amount_clamping(void)
{
    BEGIN("apply_plane: out-of-range amount is clamped, not rejected");
    uint8_t src[3 * 3] = { 0,0,0, 0,100,0, 0,0,0 };
    uint8_t a[3*3], b[3*3], ws[3*3];

    /* Negative amount should behave as 0 (identity). */
    int rc = up_usm_apply_plane(a, 3, src, 3, 3, 3, -100, ws);
    CHECK_EQ(rc, 1);
    CHECK_EQ(memcmp(a, src, sizeof src), 0);

    /* Amount > MAX should clamp to MAX, not reject. */
    rc = up_usm_apply_plane(b, 3, src, 3, 3, 3, 999999, ws);
    CHECK_EQ(rc, 1);
    /* Centre should be saturated at 255. */
    CHECK_EQ(b[4], 255);
    END();
}

/* ---------------------- main ---------------------- */

int main(void)
{
    printf("Running usm tests...\n");

    test_amount_conversion();
    test_workspace_size();
    test_hblur_row();
    test_hblur_constant_invariant();
    test_apply_amount_zero_is_identity();
    test_apply_constant_input();
    test_apply_impulse_amount_one();
    test_apply_saturation();
    test_apply_in_place_equals_out_of_place();
    test_apply_stride_greater_than_width();
    test_apply_invalid_inputs();
    test_apply_1x1_plane();
    test_apply_amount_clamping();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
