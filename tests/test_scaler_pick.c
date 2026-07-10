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
#include "../src/scaler_status.h"

#include <limits.h>
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

typedef struct {
    const void *attempted[2];
    int result[2];
    int count;
} mock_open_state_t;

static int mock_open_backend(void *context, const void *backend_handle)
{
    mock_open_state_t *state = context;
    int index = state->count++;
    if (index >= 2)
        return -1;
    state->attempted[index] = backend_handle;
    return state->result[index];
}

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
#define CHECK_EQ_INT(got, want) do { \
    int actual_ = (int)(got); \
    int expected_ = (int)(want); \
    if (actual_ != expected_) { \
        printf("    %s:%d: expected %d, got %d\n", __FILE__, __LINE__, \
               expected_, actual_); \
        g_failed_in_test = 1; \
    } \
} while (0)

#define PICK(zh, zs, sh, ss, pref, chroma, algo) \
    up_scaler_pick_with((zh), (zs), (sh), (ss), (pref), (chroma), (algo))
#define OPEN(preferred, fallback, context, callback, allow) \
    up_scaler_open_with_fallback((preferred), (fallback), (context), \
                                 (callback), (allow))

/* ---- backend open fallback ---- */

static void test_open_preferred_success_stops(void)
{
    BEGIN("open preferred success -> preferred, no fallback attempt");
    mock_open_state_t state = { .result = { 0, -1 } };
    const void *r = OPEN(ZIMG_TAG, SWSCALE_TAG, &state,
                         mock_open_backend, true);
    CHECK_ZIMG(r);
    CHECK_EQ_INT(state.count, 1);
    CHECK_EQ_PTR(state.attempted[0], ZIMG_TAG);
    END();
}

static void test_open_failure_without_fallback_stops(void)
{
    BEGIN("open preferred failure + fallback disabled -> NULL");
    mock_open_state_t state = { .result = { -1, 0 } };
    const void *r = OPEN(ZIMG_TAG, SWSCALE_TAG, &state,
                         mock_open_backend, false);
    CHECK_NULL(r);
    CHECK_EQ_INT(state.count, 1);
    CHECK_EQ_PTR(state.attempted[0], ZIMG_TAG);
    END();
}

static void test_open_fallback_success_preserves_order(void)
{
    BEGIN("open preferred failure -> distinct fallback success in order");
    mock_open_state_t state = { .result = { -1, 0 } };
    const void *r = OPEN(ZIMG_TAG, SWSCALE_TAG, &state,
                         mock_open_backend, true);
    CHECK_SWSCALE(r);
    CHECK_EQ_INT(state.count, 2);
    CHECK_EQ_PTR(state.attempted[0], ZIMG_TAG);
    CHECK_EQ_PTR(state.attempted[1], SWSCALE_TAG);
    END();
}

static void test_open_both_fail_returns_null(void)
{
    BEGIN("open preferred and fallback failures -> NULL");
    mock_open_state_t state = { .result = { -1, 1 } };
    const void *r = OPEN(ZIMG_TAG, SWSCALE_TAG, &state,
                         mock_open_backend, true);
    CHECK_NULL(r);
    CHECK_EQ_INT(state.count, 2);
    CHECK_EQ_PTR(state.attempted[0], ZIMG_TAG);
    CHECK_EQ_PTR(state.attempted[1], SWSCALE_TAG);
    END();
}

static void test_open_null_inputs_make_no_calls(void)
{
    BEGIN("open NULL preferred or callback -> NULL without calls");
    mock_open_state_t state = { .result = { 0, 0 } };
    CHECK_NULL(OPEN(NULL, SWSCALE_TAG, &state, mock_open_backend, true));
    CHECK_EQ_INT(state.count, 0);
    CHECK_NULL(OPEN(ZIMG_TAG, SWSCALE_TAG, &state, NULL, true));
    CHECK_EQ_INT(state.count, 0);
    END();
}

static void test_open_null_fallback_is_not_called(void)
{
    BEGIN("open preferred failure + NULL fallback -> NULL after one call");
    mock_open_state_t state = { .result = { -1, 0 } };
    const void *r = OPEN(ZIMG_TAG, NULL, &state, mock_open_backend, true);
    CHECK_NULL(r);
    CHECK_EQ_INT(state.count, 1);
    CHECK_EQ_PTR(state.attempted[0], ZIMG_TAG);
    END();
}

static void test_open_same_handle_is_not_retried(void)
{
    BEGIN("open preferred failure + aliased fallback -> no retry");
    mock_open_state_t state = { .result = { -1, 0 } };
    const void *r = OPEN(ZIMG_TAG, ZIMG_TAG, &state,
                         mock_open_backend, true);
    CHECK_NULL(r);
    CHECK_EQ_INT(state.count, 1);
    CHECK_EQ_PTR(state.attempted[0], ZIMG_TAG);
    END();
}

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

/*
 * Boundary coverage for `--autoupscale-backend` (VLC range 0..2 =
 * AUTO/ZIMG/SWSCALE). Out-of-range values must not crash and must
 * fall through to AUTO. Includes one-below-min, one-above-max,
 * INT_MIN, INT_MAX.
 */
static void test_backend_pref_oob_boundaries(void)
{
    BEGIN("backend pref OOB (-1, 3, INT_MIN, INT_MAX) all fall through to AUTO");
    /* SCALER_PICK_SWSCALE is the highest valid pref (= 2). One past
     * it (3) plus assorted other OOB ints must all fall through to
     * AUTO and pick zimg (since both mocks support everything). */
    int oob[] = { -1, SCALER_PICK_SWSCALE + 1, 99, 1000, INT_MIN, INT_MAX };
    for (size_t i = 0; i < sizeof(oob) / sizeof(oob[0]); i++) {
        const void *r = PICK(ZIMG_TAG, mock_zimg_supports_all,
                             SWSCALE_TAG, mock_sws_supports_all,
                             oob[i], 0, 0);
        if (r != ZIMG_TAG) {
            printf("    pref=%d expected AUTO->zimg, got %p\n",
                   oob[i], r);
            g_failed_in_test = 1;
        }
    }
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

static void test_process_status_contract(void)
{
    BEGIN("process statuses distinguish frame drop from backend fallback");
    CHECK_EQ_INT(SCALER_PROCESS_OK, 0);
    CHECK_EQ_INT(SCALER_PROCESS_TRANSIENT, 1);
    CHECK_EQ_INT(SCALER_PROCESS_FATAL, 2);
    CHECK_EQ_INT(scaler_process_needs_fallback(SCALER_PROCESS_OK), 0);
    CHECK_EQ_INT(scaler_process_needs_fallback(SCALER_PROCESS_TRANSIENT), 0);
    CHECK_EQ_INT(scaler_process_needs_fallback(SCALER_PROCESS_FATAL), 1);
    CHECK_EQ_INT(scaler_process_lines_status(720, 720), SCALER_PROCESS_OK);
    CHECK_EQ_INT(scaler_process_lines_status(719, 720), SCALER_PROCESS_TRANSIENT);
    CHECK_EQ_INT(scaler_process_lines_status(-1, 720), SCALER_PROCESS_TRANSIENT);
    CHECK_EQ_INT(scaler_process_lines_status(721, 720), SCALER_PROCESS_TRANSIENT);
    END();
}

int main(void)
{
    printf("Running scaler_pick tests...\n");

    test_open_preferred_success_stops();
    test_open_failure_without_fallback_stops();
    test_open_fallback_success_preserves_order();
    test_open_both_fail_returns_null();
    test_open_null_inputs_make_no_calls();
    test_open_null_fallback_is_not_called();
    test_open_same_handle_is_not_retried();

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
    test_backend_pref_oob_boundaries();

    test_null_swscale_returns_null();
    test_null_swscale_supports_returns_null();
    test_null_zimg_supports_in_auto_falls_back();
    test_process_status_contract();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
