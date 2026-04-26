// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_usm.c — fuzzer for the unsharp-mask logic
 *****************************************************************************
 * Two build modes (controlled by the FUZZ_MAIN macro), same pattern as
 * fuzz_upscale_logic.c:
 *
 *   - libFuzzer target (default with clang -fsanitize=fuzzer)
 *   - Smoke runner (-DFUZZ_MAIN), useful when libFuzzer isn't available.
 *****************************************************************************/

#include "../src/usm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bound the dimensions so we don't allocate gigabytes per fuzz iteration.
 * We're fuzzing the algorithm, not the OOM handler. */
#define FUZZ_MAX_W 96
#define FUZZ_MAX_H 96

/*
 * Run one iteration with input from `data`. Reads a small struct out of
 * the first ~16 bytes; the rest seeds the source plane.
 */
static void run_one(const uint8_t *data, size_t size)
{
    if (size < 12) return;

    uint16_t u_w, u_h, u_dst_pad, u_src_pad;
    int16_t  i_amount;
    uint8_t  flag_in_place;
    uint8_t  flag_zero_amount;

    memcpy(&u_w,        data + 0, 2);
    memcpy(&u_h,        data + 2, 2);
    memcpy(&u_dst_pad,  data + 4, 2);
    memcpy(&u_src_pad,  data + 6, 2);
    memcpy(&i_amount,   data + 8, 2);
    flag_in_place    = data[10];
    flag_zero_amount = data[11];

    int width  = (u_w % FUZZ_MAX_W) + 1;
    int height = (u_h % FUZZ_MAX_H) + 1;
    int dst_pad = u_dst_pad % 8;          /* 0..7 extra stride padding */
    int src_pad = u_src_pad % 8;
    int dst_stride = width + dst_pad;
    int src_stride = width + src_pad;
    int amount = (int)i_amount;
    if (flag_zero_amount & 1) amount = 0;  /* exercise identity path */

    int in_place = (flag_in_place & 1) && (dst_stride == src_stride);

    size_t src_bytes = (size_t)src_stride * (size_t)height;
    size_t dst_bytes = (size_t)dst_stride * (size_t)height;
    size_t ws_bytes  = up_usm_workspace_size(width, height);
    if (ws_bytes == 0) return;

    uint8_t *src = (uint8_t *)malloc(src_bytes + 1);
    uint8_t *dst = NULL;
    uint8_t *ws  = (uint8_t *)malloc(ws_bytes + 1);
    if (!src || !ws) { free(src); free(ws); return; }

    /* Sentinels at the end to catch one-byte overruns. */
    src[src_bytes] = 0x5A;
    ws[ws_bytes]   = 0x5A;

    /* Seed src with whatever's left in `data`, repeated. */
    if (size > 12) {
        const uint8_t *seed = data + 12;
        size_t seed_n = size - 12;
        for (size_t i = 0; i < src_bytes; i++)
            src[i] = seed[i % seed_n];
    } else {
        memset(src, 0, src_bytes);
    }

    if (in_place) {
        dst = src;
    } else {
        dst = (uint8_t *)malloc(dst_bytes + 1);
        if (!dst) { free(src); free(ws); return; }
        dst[dst_bytes] = 0x5A;
        memset(dst, 0xAB, dst_bytes);
    }

    /* Save src for later comparison if amount==0. */
    uint8_t *src_copy = NULL;
    if (amount == 0 && !in_place) {
        src_copy = (uint8_t *)malloc(src_bytes);
        if (src_copy) memcpy(src_copy, src, src_bytes);
    }

    int rc = up_usm_apply_plane(dst, dst_stride, src, src_stride,
                                width, height, amount, ws);

    /* Postconditions. */
    if (rc != 1) abort();  /* All inputs above are valid. */

    /* Sentinels intact. */
    if (src[src_bytes] != 0x5A) abort();
    if (ws[ws_bytes]   != 0x5A) abort();
    if (!in_place && dst[dst_bytes] != 0x5A) abort();

    /* If amount was 0 and we have a copy, dst must equal original src
     * within the live region, ignoring stride padding. */
    if (amount == 0 && src_copy != NULL) {
        for (int y = 0; y < height; y++) {
            if (memcmp(dst + (size_t)y * dst_stride,
                       src_copy + (size_t)y * src_stride,
                       (size_t)width) != 0) abort();
        }
    }

    /* Constant-input check: if all source bytes in the live region equal v,
     * the output's live region must also equal v (high-pass of constant=0). */
    {
        int constant = 1;
        uint8_t v = src[0];
        for (int y = 0; y < height && constant; y++) {
            for (int x = 0; x < width; x++) {
                if (src[(size_t)y * src_stride + x] != v) {
                    constant = 0; break;
                }
            }
        }
        if (constant) {
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    if (dst[(size_t)y * dst_stride + x] != v) abort();
                }
            }
        }
    }

    free(ws);
    free(src);
    if (!in_place) free(dst);
    free(src_copy);
}

/* ---------- libFuzzer entry point ---------- */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run_one(data, size);
    return 0;
}

/* ---------- standalone smoke main ---------- */
#ifdef FUZZ_MAIN
int main(int argc, char **argv)
{
    long n = 50000;
    if (argc > 1) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (end && *end == '\0' && v > 0) n = v;
    }

    uint32_t s = 0xC0FFEEu;
    uint8_t buf[64];
    for (long i = 0; i < n; i++) {
        for (size_t j = 0; j < sizeof buf; j += 4) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            memcpy(buf + j, &s, 4);
        }
        run_one(buf, sizeof buf);
    }
    printf("USM smoke fuzz OK: %ld iterations\n", n);
    return 0;
}
#endif
