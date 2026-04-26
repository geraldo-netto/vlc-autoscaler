// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_scaler_pick.c - unit tests for the backend dispatch logic
 *****************************************************************************
 * The backend dispatch (which scaler the user gets for a given
 * preference + chroma + algo) used to live inside scaler.c bound to
 * the actual scaler_backend_t struct, which dragged in VLC headers
 * and made unit testing impossible. We extracted the pure dispatch
 * into src/scaler_pick_logic.h (a duck-typed helper using void
 * pointers + an explicit supports callback), so the same code path
 * is exercised by both production and these tests, without VLC.
 *****************************************************************************/

#include "../src/scaler_pick_logic.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ---- mock supports() callbacks ---- */

static int mock_zimg_supports_all(uint32_t chroma, int algo) {
    (void)chroma; (void)algo;
    return 1;
}
static int mock_zimg_supports_none(uint32_t chroma, int algo) {
    (void)chroma; (void)algo;
    return 0;
}
static int mock_zimg_supports_planar_only(uint32_t chroma, int algo) {
    (void)algo;
    /* Pretend zimg only supports I420 (LE 0x30323449). */
    return chroma == 0x30323449u;
}
static int mock_sws_supports_all(uint32_t chroma, int algo) {
    (void)chroma; (void)algo;
    return 1;
}
static int mock_sws_supports_none(uint32_t chroma, int algo) {
    (void)chroma; (void)algo;
    return 0;
}

/* Mock backend handles. The pointer values are sentinels — the
 * dispatch logic returns one of these handles unchanged, so we just
 * need stable, distinct addresses. */
static const char ZIMG_TAG[]    = "zimg";
static const char SWSCALE_TAG[] = "swscale";

/* ---- test framework ---- */

static int g_run = 0, g_fail = 0, g_failed_in_test = 0;

#define BEGIN(name) do { printf("  [....] %s\n", name); g_failed_in_test = 0; } while (0)
#define END() do { g_run++; if (g_failed_in_test) { g_fail++; } } while (0)
#define CHECK_EQ_PTR(got, want) do { \
    if ((got) != (want)) { \
        printf("    %s:%d: expected %p, got %p\n", __FILE__, __LINE__, \
               (const void*)(want), (const void*)(got)); \
        g_failed_in_test = 1; \
    } \
} while (0)
#define CHECK_NULL(p)    CHECK_EQ_PTR(p, NULL)
#define CHECK_ZIMG(p)    CHECK_EQ_PTR(p, ZIMG_TAG)
#define CHECK_SWSCALE(p) CHECK_EQ_PTR(p, SWSCALE_TAG)

#define PICK(zh, zs, sh, ss, pref, chroma, algo) \
    up_scaler_pick_with((zh), (zs), (sh), (ss), (pref), (chroma), (algo))

/* ---- AUTO preference ---- */

static void test_auto_zimg_supports_returns_zimg(void)
{
    BEGIN("AUTO + zimg supports -> zimg");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_all,
                         SWSCALE_TAG, mock_sws_supports_all,
                         SCALER_PICK_AUTO, 0, 0);
    CHECK_ZIMG(r);
    END();
}

static void test_auto_zimg_unsupported_falls_back_to_swscale(void)
{
    BEGIN("AUTO + zimg declines -> swscale");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_none,
                         SWSCALE_TAG, mock_sws_supports_all,
                         SCALER_PICK_AUTO, 0, 0);
    CHECK_SWSCALE(r);
    END();
}

static void test_auto_zimg_null_falls_back_to_swscale(void)
{
    BEGIN("AUTO + zimg unavailable (NULL handle) -> swscale");
    const void *r = PICK(NULL, NULL,
                         SWSCALE_TAG, mock_sws_supports_all,
                         SCALER_PICK_AUTO, 0, 0);
    CHECK_SWSCALE(r);
    END();
}

static void test_auto_neither_supports_returns_null(void)
{
    BEGIN("AUTO + neither supports -> NULL");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_none,
                         SWSCALE_TAG, mock_sws_supports_none,
                         SCALER_PICK_AUTO, 0, 0);
    CHECK_NULL(r);
    END();
}

/* ---- ZIMG preference ---- */

static void test_zimg_pref_supports_returns_zimg(void)
{
    BEGIN("ZIMG pref + supports -> zimg");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_all,
                         SWSCALE_TAG, mock_sws_supports_all,
                         SCALER_PICK_ZIMG, 0, 0);
    CHECK_ZIMG(r);
    END();
}

static void test_zimg_pref_declines_returns_null_not_swscale(void)
{
    BEGIN("ZIMG pref + zimg declines -> NULL (no fallback to swscale)");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_none,
                         SWSCALE_TAG, mock_sws_supports_all,
                         SCALER_PICK_ZIMG, 0, 0);
    CHECK_NULL(r);
    END();
}

static void test_zimg_pref_zimg_null_returns_null(void)
{
    BEGIN("ZIMG pref + zimg unavailable -> NULL");
    const void *r = PICK(NULL, NULL,
                         SWSCALE_TAG, mock_sws_supports_all,
                         SCALER_PICK_ZIMG, 0, 0);
    CHECK_NULL(r);
    END();
}

/* ---- SWSCALE preference ---- */

static void test_swscale_pref_supports_returns_swscale(void)
{
    BEGIN("SWSCALE pref + supports -> swscale");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_all,
                         SWSCALE_TAG, mock_sws_supports_all,
                         SCALER_PICK_SWSCALE, 0, 0);
    CHECK_SWSCALE(r);
    END();
}

static void test_swscale_pref_declines_returns_null(void)
{
    BEGIN("SWSCALE pref + swscale declines -> NULL");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_all,
                         SWSCALE_TAG, mock_sws_supports_none,
                         SCALER_PICK_SWSCALE, 0, 0);
    CHECK_NULL(r);
    END();
}

static void test_swscale_pref_ignores_zimg(void)
{
    BEGIN("SWSCALE pref doesn't fall back to zimg even if zimg supports");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_all,
                         SWSCALE_TAG, mock_sws_supports_none,
                         SCALER_PICK_SWSCALE, 0, 0);
    CHECK_NULL(r);
    END();
}

/* ---- chroma + algo passed through to supports() ---- */

static void test_chroma_passed_to_supports(void)
{
    BEGIN("AUTO + zimg supports planar only: I420 -> zimg, NV12 -> swscale");
    const void *r1 = PICK(ZIMG_TAG, mock_zimg_supports_planar_only,
                          SWSCALE_TAG, mock_sws_supports_all,
                          SCALER_PICK_AUTO, 0x30323449u, 0);
    CHECK_ZIMG(r1);
    const void *r2 = PICK(ZIMG_TAG, mock_zimg_supports_planar_only,
                          SWSCALE_TAG, mock_sws_supports_all,
                          SCALER_PICK_AUTO, 0x3231564Eu, 0);
    CHECK_SWSCALE(r2);
    END();
}

/* ---- Unknown pref value falls through to AUTO ---- */

static void test_unknown_pref_falls_through_to_auto(void)
{
    BEGIN("Unknown pref value (e.g. 99) behaves like AUTO");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_all,
                         SWSCALE_TAG, mock_sws_supports_all,
                         99, 0, 0);
    CHECK_ZIMG(r);
    END();
}

/* ---- Defensive: NULL handles / callbacks ---- */

static void test_null_swscale_returns_null(void)
{
    BEGIN("NULL swscale handle -> NULL (defensive)");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_all,
                         NULL, NULL,
                         SCALER_PICK_AUTO, 0, 0);
    CHECK_NULL(r);
    END();
}

static void test_null_swscale_supports_returns_null(void)
{
    BEGIN("NULL swscale supports callback -> NULL");
    const void *r = PICK(ZIMG_TAG, mock_zimg_supports_all,
                         SWSCALE_TAG, NULL,
                         SCALER_PICK_AUTO, 0, 0);
    CHECK_NULL(r);
    END();
}

static void test_null_zimg_supports_in_auto_falls_back(void)
{
    BEGIN("AUTO + zimg handle with NULL supports cb -> swscale");
    const void *r = PICK(ZIMG_TAG, NULL,
                         SWSCALE_TAG, mock_sws_supports_all,
                         SCALER_PICK_AUTO, 0, 0);
    CHECK_SWSCALE(r);
    END();
}

int main(void)
{
    printf("Running scaler_pick tests...\n");

    test_auto_zimg_supports_returns_zimg();
    test_auto_zimg_unsupported_falls_back_to_swscale();
    test_auto_zimg_null_falls_back_to_swscale();
    test_auto_neither_supports_returns_null();

    test_zimg_pref_supports_returns_zimg();
    test_zimg_pref_declines_returns_null_not_swscale();
    test_zimg_pref_zimg_null_returns_null();

    test_swscale_pref_supports_returns_swscale();
    test_swscale_pref_declines_returns_null();
    test_swscale_pref_ignores_zimg();

    test_chroma_passed_to_supports();
    test_unknown_pref_falls_through_to_auto();

    test_null_swscale_returns_null();
    test_null_swscale_supports_returns_null();
    test_null_zimg_supports_in_auto_falls_back();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
