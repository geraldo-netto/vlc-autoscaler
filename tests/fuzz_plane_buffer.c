// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_plane_buffer.c - fuzz the aligned plane-buffer sizing/alloc seam
 *****************************************************************************
 * Feeds raw 32-bit width/height (full int range, hostile values included)
 * and arbitrary subsample exponents through init_plane_layout ->
 * plane_buffer_bytes -> alloc_plane_buffer and checks:
 *
 *   1. no UB anywhere (UBSan build), and
 *   2. layout planes always agree with the shared up_plane_pitch/lines
 *      helpers (never a private re-derivation drifting), and
 *   3. plane_alloc_bytes is 0 exactly for non-positive extents and the
 *      product otherwise (checked widened, so the check can't wrap), and
 *   4. plane_buffer_bytes equals the widened sum of its plane extents
 *      (saturated to SIZE_MAX), and
 *   5. for small layouts the allocation succeeds, every plane is
 *      UP_PITCH_ALIGN-aligned and fully writable (ASan-verified), and
 *      free_plane_buffer nulls all pointers.
 *
 * Two entry points: LLVMFuzzerTestOneInput + a deterministic smoke main.
 *****************************************************************************/
#include "../src/plane_buffer.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep smoke/libFuzzer allocations bounded; hostile sizing is still fully
 * exercised through the pure byte-math checks above the allocation gate. */
#define FUZZ_ALLOC_LIMIT (1u << 24)

static int layout_matches_helpers(const plane_layout_t *layout,
                                  int w, int h, unsigned sub_w, unsigned sub_h)
{
    if (layout->pitch[PLANE_Y] != up_plane_pitch(w, 0)) return 0;
    if (layout->lines[PLANE_Y] != up_plane_lines(h, 0)) return 0;
    for (int p = PLANE_U; p < PLANE_COUNT; p++) {
        if (layout->pitch[p] != up_plane_pitch(w, (int)sub_w)) return 0;
        if (layout->lines[p] != up_plane_lines(h, (int)sub_h)) return 0;
    }
    return 1;
}

static int alloc_bytes_contract_ok(int lines, int pitch)
{
    const size_t bytes = plane_alloc_bytes(lines, pitch);
    if (lines <= 0 || pitch <= 0) return bytes == 0;
    const unsigned __int128 wide =
        (unsigned __int128)lines * (unsigned __int128)pitch;
    if (wide > SIZE_MAX) return bytes == 0;
    return bytes == (size_t)wide;
}

static int buffer_bytes_contract_ok(const plane_buffer_t *buffer)
{
    unsigned __int128 wide = 0;
    for (int p = 0; p < PLANE_COUNT; p++)
        wide += plane_alloc_bytes(buffer->layout.lines[p],
                                  buffer->layout.pitch[p]);
    const size_t expected = wide > SIZE_MAX ? SIZE_MAX : (size_t)wide;
    return plane_buffer_bytes(buffer) == expected;
}

static int planes_aligned_and_writable(plane_buffer_t *buffer)
{
    int ok = 1;
    for (int p = 0; p < PLANE_COUNT; p++) {
        if ((uintptr_t)buffer->data[p] % UP_PITCH_ALIGN != 0) ok = 0;
        memset(buffer->data[p], 0x5A,
               plane_alloc_bytes(buffer->layout.lines[p],
                                 buffer->layout.pitch[p]));
    }
    return ok;
}

static int alloc_roundtrip_ok(plane_buffer_t *buffer)
{
    const size_t total = plane_buffer_bytes(buffer);
    int all_positive = 1;
    for (int p = 0; p < PLANE_COUNT; p++)
        if (plane_alloc_bytes(buffer->layout.lines[p],
                              buffer->layout.pitch[p]) == 0)
            all_positive = 0;
    if (!all_positive || total == 0 || total > FUZZ_ALLOC_LIMIT) return 1;

    if (alloc_plane_buffer(buffer) != 0) { free_plane_buffer(buffer); return 0; }
    int ok = planes_aligned_and_writable(buffer);
    free_plane_buffer(buffer);
    for (int p = 0; p < PLANE_COUNT; p++)
        if (buffer->data[p] != NULL) ok = 0;
    return ok;
}

static int run_one(const uint8_t *data, size_t size)
{
    if (size < 10) return 0;
    int w;
    int h;
    memcpy(&w, data, sizeof w);
    memcpy(&h, data + 4, sizeof h);
    const unsigned sub_w = data[8] & 3u;
    const unsigned sub_h = data[9] & 3u;

    plane_buffer_t buffer = { 0 };
    init_plane_layout(&buffer.layout, w, h, sub_w, sub_h);

    int failed = 0;
    if (!layout_matches_helpers(&buffer.layout, w, h, sub_w, sub_h)) {
        fprintf(stderr, "FAIL: layout drift w=%d h=%d sub=%u/%u\n",
                w, h, sub_w, sub_h);
        failed = 1;
    }
    for (int p = 0; p < PLANE_COUNT; p++)
        if (!alloc_bytes_contract_ok(buffer.layout.lines[p],
                                     buffer.layout.pitch[p])) {
            fprintf(stderr, "FAIL: alloc-bytes contract plane %d\n", p);
            failed = 1;
        }
    if (!buffer_bytes_contract_ok(&buffer)) {
        fprintf(stderr, "FAIL: buffer-bytes sum w=%d h=%d\n", w, h);
        failed = 1;
    }
    if (!alloc_roundtrip_ok(&buffer)) {
        fprintf(stderr, "FAIL: alloc roundtrip w=%d h=%d\n", w, h);
        failed = 1;
    }
    return failed;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (run_one(data, size)) abort();
    return 0;
}

#ifdef FUZZ_MAIN
#include "fuzz_smoke.h"

static const int BV[] = {
    INT_MIN, -1, 0, 1, 2, 63, 64, 65, 320, 854, 1920, 3840,
    INT_MAX - 63, INT_MAX,
};
#define NBV ((int)(sizeof BV / sizeof BV[0]))

static int sweep_boundaries(void)
{
    int fails = 0;
    for (int a = 0; a < NBV; a++)
    for (int b = 0; b < NBV; b++)
    for (unsigned s = 0; s < 4; s++) {
        uint8_t buf[10];
        memcpy(buf, &BV[a], 4);
        memcpy(buf + 4, &BV[b], 4);
        buf[8] = (uint8_t)s;
        buf[9] = (uint8_t)(3 - s);
        fails += run_one(buf, sizeof buf);
    }
    return fails;
}

static int smoke_iter(long i)
{
    (void)i;
    uint8_t buf[10];
    fuzz_smoke_fill(buf, sizeof buf);
    /* Bias half the trials into allocatable range so the roundtrip arm
     * runs, not just the pure byte math. */
    if (buf[9] & 1u) {
        int w = (int)(fuzz_smoke_next() % 2048u);
        int h = (int)(fuzz_smoke_next() % 1200u);
        memcpy(buf, &w, 4);
        memcpy(buf + 4, &h, 4);
    }
    return run_one(buf, sizeof buf);
}

int main(int argc, char **argv)
{
    if (sweep_boundaries()) {
        fprintf(stderr, "plane_buffer boundary sweep FAILED\n");
        return 1;
    }
    fuzz_smoke_seed(0xB1A5EDB0);
    return fuzz_smoke_main(argc, argv, 50000, "plane_buffer", smoke_iter);
}
#endif
