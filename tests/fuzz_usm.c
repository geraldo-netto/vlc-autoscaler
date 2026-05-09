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

typedef struct {
    int width;
    int height;
    int dst_stride;
    int src_stride;
    int amount;
    int in_place;
    size_t src_bytes;
    size_t dst_bytes;
    size_t ws_bytes;
} fuzz_params_t;

/* Parse fuzz bytes into geometry/amount/in_place params. Returns 1 on
 * success, 0 if input too short or workspace size is zero. */
static int parse_params(const uint8_t *data, size_t size, fuzz_params_t *p)
{
    if (size < 12) return 0;

    uint16_t u_w, u_h, u_dst_pad, u_src_pad;
    int16_t  i_amount;
    uint8_t  flag_in_place, flag_zero_amount;

    memcpy(&u_w,        data + 0, 2);
    memcpy(&u_h,        data + 2, 2);
    memcpy(&u_dst_pad,  data + 4, 2);
    memcpy(&u_src_pad,  data + 6, 2);
    memcpy(&i_amount,   data + 8, 2);
    flag_in_place    = data[10];
    flag_zero_amount = data[11];

    p->width      = (u_w % FUZZ_MAX_W) + 1;
    p->height     = (u_h % FUZZ_MAX_H) + 1;
    p->dst_stride = p->width + (u_dst_pad % 8);
    p->src_stride = p->width + (u_src_pad % 8);
    p->amount     = (flag_zero_amount & 1) ? 0 : (int)i_amount;
    p->in_place   = (flag_in_place & 1) && (p->dst_stride == p->src_stride);

    p->src_bytes = (size_t)p->src_stride * (size_t)p->height;
    p->dst_bytes = (size_t)p->dst_stride * (size_t)p->height;
    p->ws_bytes  = up_usm_workspace_size(p->width, p->height);
    return p->ws_bytes != 0;
}

/* Fill src plane from leftover fuzz bytes (or zero if none). */
static void seed_src(uint8_t *src, size_t src_bytes,
                     const uint8_t *data, size_t size)
{
    if (size > 12) {
        const uint8_t *seed = data + 12;
        size_t seed_n = size - 12;
        for (size_t i = 0; i < src_bytes; i++)
            src[i] = seed[i % seed_n];
    } else {
        memset(src, 0, src_bytes);
    }
}

/* Verify trailing sentinel bytes intact. */
static void check_sentinels(const uint8_t *src, size_t src_bytes,
                            const uint8_t *ws,  size_t ws_bytes,
                            const uint8_t *dst, size_t dst_bytes,
                            int in_place)
{
    if (src[src_bytes] != 0x5A) abort();
    if (ws[ws_bytes]   != 0x5A) abort();
    if (!in_place && dst[dst_bytes] != 0x5A) abort();
}

/* If amount==0, dst live region must equal saved src copy. */
static void check_identity(const uint8_t *dst, const uint8_t *src_copy,
                           const fuzz_params_t *p)
{
    if (p->amount != 0 || src_copy == NULL) return;
    for (int y = 0; y < p->height; y++) {
        if (memcmp(dst + (size_t)y * p->dst_stride,
                   src_copy + (size_t)y * p->src_stride,
                   (size_t)p->width) != 0) abort();
    }
}

/* Returns 1 if every byte in src live region equals v. */
static int src_is_constant(const uint8_t *src, const fuzz_params_t *p,
                           uint8_t v)
{
    for (int y = 0; y < p->height; y++) {
        for (int x = 0; x < p->width; x++) {
            if (src[(size_t)y * p->src_stride + x] != v) return 0;
        }
    }
    return 1;
}

/* High-pass of constant input must be zero, so dst==v in live region. */
static void check_constant_input(const uint8_t *src, const uint8_t *dst,
                                 const fuzz_params_t *p)
{
    uint8_t v = src[0];
    if (!src_is_constant(src, p, v)) return;
    for (int y = 0; y < p->height; y++) {
        for (int x = 0; x < p->width; x++) {
            if (dst[(size_t)y * p->dst_stride + x] != v) abort();
        }
    }
}

/* Run the kernel and verify all postconditions. */
static void run_kernel_and_check(uint8_t *dst, uint8_t *src, uint8_t *ws,
                                 const uint8_t *src_copy,
                                 const fuzz_params_t *p)
{
    int rc = up_usm_apply_plane(dst, p->dst_stride, src, p->src_stride,
                                p->width, p->height, p->amount, ws);
    if (rc != 1) abort();  /* All inputs above are valid. */

    check_sentinels(src, p->src_bytes, ws, p->ws_bytes,
                    dst, p->dst_bytes, p->in_place);
    check_identity(dst, src_copy, p);
    check_constant_input(src, dst, p);
}

/* Set up dst plane (alias src for in-place, fresh alloc otherwise) and
 * snapshot src when amount==0. Returns 1 on success, 0 on alloc failure. */
static int setup_dst_and_copy(uint8_t *src, const fuzz_params_t *p,
                              uint8_t **dst_out, uint8_t **src_copy_out)
{
    uint8_t *dst = NULL;
    uint8_t *src_copy = NULL;

    if (p->in_place) {
        dst = src;
    } else {
        dst = (uint8_t *)malloc(p->dst_bytes + 1);
        if (!dst) return 0;
        dst[p->dst_bytes] = 0x5A;
        memset(dst, 0xAB, p->dst_bytes);
    }

    if (p->amount == 0 && !p->in_place) {
        src_copy = (uint8_t *)malloc(p->src_bytes);
        if (src_copy) memcpy(src_copy, src, p->src_bytes);
    }

    *dst_out = dst;
    *src_copy_out = src_copy;
    return 1;
}

/*
 * Run one iteration with input from `data`. Reads a small struct out of
 * the first ~16 bytes; the rest seeds the source plane.
 */
static void run_one(const uint8_t *data, size_t size)
{
    fuzz_params_t p;
    if (!parse_params(data, size, &p)) return;

    uint8_t *src = (uint8_t *)malloc(p.src_bytes + 1);
    uint8_t *ws  = (uint8_t *)malloc(p.ws_bytes + 1);
    uint8_t *dst = NULL;
    uint8_t *src_copy = NULL;
    if (!src || !ws) goto out;

    /* Sentinels at the end to catch one-byte overruns. */
    src[p.src_bytes] = 0x5A;
    ws[p.ws_bytes]   = 0x5A;

    seed_src(src, p.src_bytes, data, size);

    if (!setup_dst_and_copy(src, &p, &dst, &src_copy)) goto out;

    run_kernel_and_check(dst, src, ws, src_copy, &p);

out:
    free(ws);
    free(src);
    if (!p.in_place) free(dst);
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
    long n = 100000;
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
