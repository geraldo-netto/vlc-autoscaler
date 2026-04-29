// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * usm_pool_dispatch.c - runtime CPU dispatcher for USM pool variants
 *****************************************************************************
 * The plugin ships THREE compiled variants of usm_pool.c, each built at a
 * different x86_64 microarchitecture baseline:
 *
 *   _sse2   : -march=x86-64    (lowest common denominator: SSE2, 16-byte SIMD)
 *   _avx2   : -march=x86-64-v3 (Haswell 2013 / Zen 1 2017: AVX2, 32-byte SIMD)
 *   _avx512 : -march=x86-64-v4 (Skylake-X 2017 / Zen 4 2022: AVX-512, 64-byte SIMD)
 *
 * Each variant has its public symbols suffixed (e.g. up_usm_pool_apply_avx2),
 * so all three coexist in the .so. THIS file exposes the un-suffixed public
 * API (up_usm_pool_create / destroy / apply) and forwards every call to one
 * variant chosen ONCE at .so load time, via __attribute__((constructor)).
 *
 * Why a load-time dispatch and not per-call?
 *
 *   - __builtin_cpu_supports() always returns the same answer for the life
 *     of the process — there's no point re-checking.
 *
 *   - Per-call dispatch would add a pointer load + indirect call to every
 *     usm_pool_apply, which fires once per frame. Trivial cost (~3 ns), but
 *     a one-time check at load is even cheaper.
 *
 *   - Constructor functions run during dlopen() before any code in the .so
 *     can be called from outside, so by the time VLC's plugin loader (or
 *     vlc-cache-gen) reaches our entry points, the dispatch table is set.
 *
 * Why not __attribute__((target_clones))?
 *
 *   target_clones generates an IFUNC resolver and three function bodies per
 *   attributed function. We'd have to apply it to each of three public
 *   functions AND it requires non-static linkage (defeating the static-inline
 *   inlining of the hot kernels into the worker_main loop). Compiling the
 *   whole TU at three -march levels into separate .o files lets each variant
 *   inline its kernels at its own SIMD width — strictly better codegen.
 *
 * Safety: the AVX-512 variant's code section contains AVX-512 instructions.
 * We never EXECUTE that code on a CPU that doesn't support AVX-512 (the
 * dispatcher's selection logic prevents it). Holding a function pointer to
 * code we never run is fine — the dynamic linker only does relocations.
 *****************************************************************************/

#include "usm_pool.h"

#include <stdint.h>

/* Forward declarations: each lives in its own variant .o */
extern usm_pool_t *up_usm_pool_create_sse2(int n_threads, int width, int height);
extern usm_pool_t *up_usm_pool_create_avx2(int n_threads, int width, int height);
extern usm_pool_t *up_usm_pool_create_avx512(int n_threads, int width, int height);

extern void up_usm_pool_destroy_sse2(usm_pool_t *pool);
extern void up_usm_pool_destroy_avx2(usm_pool_t *pool);
extern void up_usm_pool_destroy_avx512(usm_pool_t *pool);

extern int up_usm_pool_apply_sse2(usm_pool_t *pool,
                                  uint8_t *dst, int dst_stride,
                                  const uint8_t *src, int src_stride,
                                  int amount_q8);
extern int up_usm_pool_apply_avx2(usm_pool_t *pool,
                                  uint8_t *dst, int dst_stride,
                                  const uint8_t *src, int src_stride,
                                  int amount_q8);
extern int up_usm_pool_apply_avx512(usm_pool_t *pool,
                                    uint8_t *dst, int dst_stride,
                                    const uint8_t *src, int src_stride,
                                    int amount_q8);

/* Picked variant name, exported for diagnostic logging from autoupscale.c. */
const char *up_usm_pool_variant_name = "uninitialized";

/* Function pointers populated at .so load. */
static usm_pool_t *(*p_create) (int, int, int);
static void        (*p_destroy)(usm_pool_t *);
static int         (*p_apply)  (usm_pool_t *, uint8_t *, int,
                                const uint8_t *, int, int);

/*
 * Constructor: runs at .so load via the GCC/Clang constructor attribute.
 * Selection priority: AVX-512 > AVX2 > SSE2. The SSE2 baseline is always a
 * valid fallback (every x86_64 CPU has SSE2; it's part of the architecture).
 *
 * AVX-512 needs both the foundation (avx512f) AND the byte/word ops
 * (avx512bw) since our kernels operate on bytes. avx512f alone wouldn't
 * give us the SIMD instructions the vectorizer needs for uint8_t loops.
 */
static void __attribute__((constructor))
up_usm_pool_dispatch_init(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
        p_create  = up_usm_pool_create_avx512;
        p_destroy = up_usm_pool_destroy_avx512;
        p_apply   = up_usm_pool_apply_avx512;
        up_usm_pool_variant_name = "avx512";
        return;
    }
    if (__builtin_cpu_supports("avx2")) {
        p_create  = up_usm_pool_create_avx2;
        p_destroy = up_usm_pool_destroy_avx2;
        p_apply   = up_usm_pool_apply_avx2;
        up_usm_pool_variant_name = "avx2";
        return;
    }
#endif
    p_create  = up_usm_pool_create_sse2;
    p_destroy = up_usm_pool_destroy_sse2;
    p_apply   = up_usm_pool_apply_sse2;
    up_usm_pool_variant_name = "sse2";
}

/* Public API — thin forwarding shims. The function-pointer indirection
 * is one extra load per call; ~1 ns on modern x86. Negligible against
 * the millisecond-scale work it dispatches. */
usm_pool_t *up_usm_pool_create(int n_threads, int width, int height)
{
    return p_create(n_threads, width, height);
}

void up_usm_pool_destroy(usm_pool_t *pool)
{
    p_destroy(pool);
}

int up_usm_pool_apply(usm_pool_t *pool,
                      uint8_t *dst, int dst_stride,
                      const uint8_t *src, int src_stride,
                      int amount_q8)
{
    return p_apply(pool, dst, dst_stride, src, src_stride, amount_q8);
}
