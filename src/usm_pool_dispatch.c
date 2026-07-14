// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * usm_pool_dispatch.c - runtime CPU dispatcher for USM pool variants
 *****************************************************************************
 * The plugin ships THREE compiled variants of usm_pool.c, each built at a
 * different x86_64 microarchitecture baseline:
 *
 *   _sse2   : -march=x86-64    (lowest common denominator: SSE2, 16-byte SIMD)
 *   _avx2   : -march=x86-64-v3 (full v3 level, including 32-byte AVX2 SIMD)
 *   _avx512 : -march=x86-64-v4 (full v4 level, including 64-byte AVX-512 SIMD)
 *
 * Each variant has its public symbols suffixed (e.g. up_usm_pool_apply_avx2),
 * so all three coexist in the .so. THIS file exposes the un-suffixed public
 * API (up_usm_pool_create / destroy / apply / effective_threads) and forwards
 * every call to one variant chosen ONCE at .so load time, via
 * __attribute__((constructor)).
 *
 * Why a load-time dispatch and not per-call?
 *
 *   - __builtin_cpu_supports() always returns the same answer for the life
 *     of the process — there's no point re-checking.
 *
 *   - Per-call feature selection would repeat invariant work on every frame;
 *     load-time selection performs it once.
 *
 *   - Constructor functions run during dlopen() before any code in the .so
 *     can be called from outside, so by the time VLC's plugin loader (or
 *     vlc-cache-gen) reaches our entry points, the dispatch table is set.
 *
 * Why not __attribute__((target_clones))?
 *
 *   target_clones generates an IFUNC resolver and three function bodies per
 *   attributed function. We'd have to apply it to each of four public
 *   functions AND it requires non-static linkage (defeating the static-inline
 *   inlining of the hot kernels into the worker_main loop). Compiling the
 *   whole TU at three -march levels into separate .o files lets each variant
 *   inline its kernels at its own SIMD width — strictly better codegen.
 *
 * The selector uses the shared v3/v4 level probes. Current compilers query the
 * complete levels directly; the compatibility fallback uses the strongest
 * feature conjunction available to older compilers.
 *****************************************************************************/

#include "cpu_level.h"
#include "usm_pool.h"

#include <stdint.h>

/* This dispatcher only makes sense on x86-64: it selects between SSE2 / AVX2 /
 * AVX-512 variant objects (built at x86 -march levels) via __builtin_cpu_*,
 * which exists only on x86 gcc/clang. The Makefile only compiles this TU for
 * MULTIVERSION=1, and only with x86 marches; fail loudly if it is ever fed to
 * another architecture instead of silently mis-wiring the SSE2 fallback
 * (PORT-2). Non-x86 targets build MULTIVERSION=0 (single usm_pool.o). */
#if !defined(__x86_64__)
#  error "usm_pool_dispatch.c is x86-64 only; build with MULTIVERSION=0 elsewhere"
#endif

/* Variant entry points: one shared set of prototypes (ABI-2); each
 * definition lives in its own variant .o. */
#include "usm_pool_variants.h"

/* Picked variant name, exported for diagnostic logging from autoupscale.c. */
const char *up_usm_pool_variant_name = "uninitialized";

/*
 * One strategy vtable, selected once at load. Grouping the four function
 * pointers (rather than four loose statics) parallels scaler_backend_t
 * and makes "the active variant" a single object — add a new entry point
 * and it is one struct member to wire, not another loose global.
 *
 * ABI-2: derive each member type from the matching variant declaration in
 * usm_pool_variants.h (the single source of truth) rather than restating the
 * signature here. A prototype change in that header now propagates to the
 * vtable automatically; a truly incompatible change fails to type-check at the
 * assignments below instead of drifting silently. __typeof__ is the
 * strict-ISO-safe spelling and this TU is already GCC/Clang-x86 only. */
typedef struct {
    __typeof__(&up_usm_pool_create_sse2)             create;
    __typeof__(&up_usm_pool_destroy_sse2)            destroy;
    __typeof__(&up_usm_pool_apply_sse2)              apply;
    __typeof__(&up_usm_pool_effective_threads_sse2)  effective_threads;
} usm_pool_ops_t;

static usm_pool_ops_t g_ops;

/*
 * Selection priority: AVX-512 > AVX2 > SSE2. The SSE2 baseline is always a
 * valid fallback (every x86_64 CPU has SSE2; it's part of the architecture).
 * Split out from the constructor and parameterised on the two capability bits
 * so the whole selection table is unit-testable without the host's actual CPU
 * (tests/test_usm_pool_dispatch.c drives all three arms).
 *
 * PORT-6: the variant objects are compiled at -march=x86-64-v4/v3, so
 * selection proves the full level (the v4 object contains EVEX.256
 * instructions requiring AVX512VL, not just F+BW).
 */
static void usm_pool_select_ops(int have_v4, int have_v3,
                                usm_pool_ops_t *ops, const char **name)
{
    if (have_v4) {
        *ops = (usm_pool_ops_t){ up_usm_pool_create_avx512,
                                 up_usm_pool_destroy_avx512,
                                 up_usm_pool_apply_avx512,
                                 up_usm_pool_effective_threads_avx512 };
        *name = "avx512";
        return;
    }
    if (have_v3) {
        *ops = (usm_pool_ops_t){ up_usm_pool_create_avx2,
                                 up_usm_pool_destroy_avx2,
                                 up_usm_pool_apply_avx2,
                                 up_usm_pool_effective_threads_avx2 };
        *name = "avx2";
        return;
    }
    *ops = (usm_pool_ops_t){ up_usm_pool_create_sse2,
                             up_usm_pool_destroy_sse2,
                             up_usm_pool_apply_sse2,
                             up_usm_pool_effective_threads_sse2 };
    *name = "sse2";
}

/* Constructor: runs at .so load via the GCC/Clang constructor attribute and
 * binds g_ops to the best variant the running CPU supports. */
static void __attribute__((constructor))
up_usm_pool_dispatch_init(void)
{
    int have_v4 = 0;
    int have_v3 = 0;
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    have_v4 = up_cpu_supports_v4();
    have_v3 = up_cpu_supports_v3();
#endif
    usm_pool_select_ops(have_v4, have_v3, &g_ops, &up_usm_pool_variant_name);
}

/* Public API — thin forwarding shims through the load-time-selected table. */
usm_pool_t *up_usm_pool_create(int n_threads, int width, int height,
                               int stripe_min_rows)
{
    return g_ops.create(n_threads, width, height, stripe_min_rows);
}

void up_usm_pool_destroy(usm_pool_t *pool)
{
    g_ops.destroy(pool);
}

int up_usm_pool_apply(usm_pool_t *pool,
                      uint8_t *dst, int dst_stride,
                      const uint8_t *src, int src_stride,
                      int amount_q8)
{
    return g_ops.apply(pool, dst, dst_stride, src, src_stride, amount_q8);
}

int up_usm_pool_effective_threads(const usm_pool_t *pool)
{
    return g_ops.effective_threads(pool);
}
