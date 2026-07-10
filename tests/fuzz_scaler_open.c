// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_scaler_open.c - fuzz backend-open fallback sequencing
 *****************************************************************************/

#include "../src/scaler_pick_logic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const char PREFERRED_TAG[] = "preferred";
static const char FALLBACK_TAG[] = "fallback";

typedef struct {
    const void *attempted[2];
    int result[2];
    int count;
    bool overflow;
} open_state_t;

typedef struct {
    const void *selected;
    const void *attempted[2];
    int count;
} expected_open_t;

static int record_open(void *context, const void *backend_handle)
{
    open_state_t *state = context;
    int index = state->count++;
    if (index >= 2) {
        state->overflow = true;
        return -1;
    }
    state->attempted[index] = backend_handle;
    return state->result[index];
}

static expected_open_t model_open(const void *preferred,
                                  const void *fallback,
                                  bool callback_present,
                                  bool allow_fallback,
                                  const int result[2])
{
    expected_open_t expected = { 0 };
    if (preferred == NULL || !callback_present)
        return expected;

    expected.attempted[0] = preferred;
    expected.count = 1;
    if (result[0] == 0) {
        expected.selected = preferred;
        return expected;
    }
    if (!allow_fallback || fallback == NULL || fallback == preferred)
        return expected;

    expected.attempted[1] = fallback;
    expected.count = 2;
    if (result[1] == 0)
        expected.selected = fallback;
    return expected;
}

static void fail_case(uint8_t control, const char *property)
{
    fprintf(stderr, "scaler_open invariant failed: control=0x%02x %s\n",
            control, property);
    abort();
}

static void check_result(uint8_t control, const open_state_t *actual,
                         const expected_open_t *expected,
                         const void *selected)
{
    if (actual->overflow)
        fail_case(control, "more than two attempts");
    if (selected != expected->selected)
        fail_case(control, "wrong selected handle");
    if (actual->count != expected->count)
        fail_case(control, "wrong attempt count");
    for (int i = 0; i < expected->count; ++i) {
        if (actual->attempted[i] != expected->attempted[i])
            fail_case(control, "wrong attempt order");
    }
}

static void run_case(uint8_t control)
{
    const void *preferred = (control & 0x01u) ? NULL : PREFERRED_TAG;
    const void *fallback = (control & 0x02u) ? NULL : FALLBACK_TAG;
    bool callback_present = (control & 0x04u) == 0;
    bool allow_fallback = (control & 0x08u) != 0;

    if (control & 0x10u)
        fallback = preferred;

    open_state_t actual = {
        .result = {
            (control & 0x20u) ? -1 : 0,
            (control & 0x40u) ? 1 : 0,
        },
    };
    expected_open_t expected = model_open(preferred, fallback,
                                          callback_present, allow_fallback,
                                          actual.result);
    up_scaler_open_fn callback = callback_present ? record_open : NULL;
    const void *selected = up_scaler_open_with_fallback(
        preferred, fallback, &actual, callback, allow_fallback);

    check_result(control, &actual, &expected, selected);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) {
        run_case(0);
        return 0;
    }

    size_t count = size < 4096 ? size : 4096;
    for (size_t i = 0; i < count; ++i)
        run_case(data[i]);
    return 0;
}

#ifdef FUZZ_MAIN
int main(int argc, char **argv)
{
    long iterations = 100000;
    if (argc > 1) {
        char *end = NULL;
        long parsed = strtol(argv[1], &end, 10);
        if (end != NULL && *end == '\0' && parsed > 0)
            iterations = parsed;
    }

    for (long i = 0; i < iterations; ++i)
        run_case((uint8_t)i);

    printf("scaler_open smoke OK: %ld iterations\n", iterations);
    return 0;
}
#endif
