// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_copy_plane.c - libFuzzer/smoke target for up_copy_plane
 *****************************************************************************
 * Memory-safety fuzzer for the stride-aware plane copy. For each trial:
 *   1. Pick (rows, row_bytes, src_stride, dst_stride) from the fuzz bytes.
 *   2. Allocate src buffer of exactly rows*src_stride bytes (no padding).
 *   3. Allocate dst buffer of exactly rows*dst_stride bytes, framed with
 *      sentinel pages so any write past the buffer end is caught by ASan.
 *   4. Fill src with a deterministic pattern.
 *   5. Run up_copy_plane.
 *   6. Verify each row's first row_bytes bytes in dst match src's, and any
 *      stride-padding bytes in dst (between row_bytes and dst_stride per
 *      row) are unchanged from sentinel.
 *
 * This is the most safety-critical of the three new fuzzers because
 * copy_plane runs on every frame and gets non-trivial inputs from VLC's
 * picture pool (varying strides per chroma format).
 *
 * Build:
 *   cc -O2 -g -fsanitize=address,undefined -DFUZZ_MAIN \
 *      -I src tests/fuzz_copy_plane.c -o build/fuzz_copy_plane_smoke
 *****************************************************************************/

#include "../src/zimg_helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS    256
#define MAX_BYTES   1024
#define MAX_STRIDE  2048

static int run_one(const uint8_t *data, size_t size)
{
    if (size < 4 * sizeof(uint16_t)) return 0;

    uint16_t r0, r1, r2, r3;
    memcpy(&r0, data + 0,  sizeof r0);
    memcpy(&r1, data + 2,  sizeof r1);
    memcpy(&r2, data + 4,  sizeof r2);
    memcpy(&r3, data + 6,  sizeof r3);

    int rows       = (int)(r0 % (MAX_ROWS + 1));
    int row_bytes  = (int)(r1 % (MAX_BYTES + 1));
    int src_stride = (int)(r2 % (MAX_STRIDE + 1));
    int dst_stride = (int)(r3 % (MAX_STRIDE + 1));

    /* Strides must be at least row_bytes; if they're not, copy_plane
     * would read/write past the row width into the padding region of the
     * next row, which is fine for memory safety BUT means our "padding
     * preserved" check below isn't meaningful. So clamp src/dst_stride
     * up to row_bytes for a clean test. */
    if (src_stride < row_bytes) src_stride = row_bytes;
    if (dst_stride < row_bytes) dst_stride = row_bytes;

    /* Allocate exactly rows*stride bytes — no slack — so an out-of-bounds
     * write would land outside the allocation and trip ASan/UBSan. */
    size_t src_size = (size_t)rows * (size_t)src_stride;
    size_t dst_size = (size_t)rows * (size_t)dst_stride;
    if (src_size == 0 || dst_size == 0) return 0;
    if (src_size > 16 * 1024 * 1024 || dst_size > 16 * 1024 * 1024) return 0;

    uint8_t *src = malloc(src_size);
    uint8_t *dst = malloc(dst_size);
    if (!src || !dst) { free(src); free(dst); return 0; }

    /* Fill src deterministically. */
    for (size_t i = 0; i < src_size; i++)
        src[i] = (uint8_t)((i * 31 + 7) & 0xff);
    /* Sentinel-fill dst so we can detect unintended writes. */
    memset(dst, 0xC3, dst_size);

    up_copy_plane(dst, dst_stride, src, src_stride, row_bytes, rows);

    /* For each row: first row_bytes must match src; the remaining
     * stride - row_bytes bytes must still be 0xC3. */
    for (int r = 0; r < rows; r++) {
        const uint8_t *src_row = src + (size_t)r * src_stride;
        const uint8_t *dst_row = dst + (size_t)r * dst_stride;
        for (int c = 0; c < row_bytes; c++) {
            if (dst_row[c] != src_row[c]) {
                fprintf(stderr,
                    "FAIL: dst[%d][%d]=0x%02x src[%d][%d]=0x%02x "
                    "(rows=%d row_bytes=%d src_stride=%d dst_stride=%d)\n",
                    r, c, dst_row[c], r, c, src_row[c],
                    rows, row_bytes, src_stride, dst_stride);
                free(src); free(dst); return 1;
            }
        }
        for (int c = row_bytes; c < dst_stride; c++) {
            if (dst_row[c] != 0xC3) {
                fprintf(stderr,
                    "FAIL: dst row %d padding byte %d = 0x%02x "
                    "(should be 0xC3 sentinel) "
                    "rows=%d row_bytes=%d dst_stride=%d\n",
                    r, c, dst_row[c], rows, row_bytes, dst_stride);
                free(src); free(dst); return 1;
            }
        }
    }

    free(src); free(dst);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return run_one(data, size) ? 1 : 0;
}

#ifdef FUZZ_MAIN
static uint64_t xs_state = 0xfeedfacef00dULL;
static uint64_t xs(void)
{
    uint64_t x = xs_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return xs_state = x;
}

int main(int argc, char **argv)
{
    long iters = (argc > 1) ? atol(argv[1]) : 50000;
    uint8_t buf[16];
    long n_fail = 0;
    for (long i = 0; i < iters; i++) {
        for (size_t j = 0; j < sizeof buf; j++) buf[j] = (uint8_t)xs();
        if (run_one(buf, sizeof buf)) n_fail++;
    }
    if (n_fail) {
        fprintf(stderr, "copy_plane smoke FAIL: %ld/%ld trials\n",
                n_fail, iters);
        return 1;
    }
    fprintf(stderr, "copy_plane smoke OK: %ld iterations\n", iters);
    return 0;
}
#endif
