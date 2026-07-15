// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * cpu_level.h — runtime x86-64 micro-architecture level probes (PORT-6)
 *****************************************************************************
 * Objects compiled at -march=x86-64-v3/v4 may use ANY instruction of that
 * level (BMI/BMI2/F16C/FMA/LZCNT/MOVBE at v3; +AVX512CD/DQ/VL at v4), so a
 * runtime gate must prove the whole level, not a headline feature.
 * GCC >= 12 and Clang >= 17 accept the "x86-64-vN" strings directly; older
 * compilers fall back to explicit CPUID/XGETBV checks for the complete level.
 *****************************************************************************/

#ifndef AUTOUPSCALE_CPU_LEVEL_H
#define AUTOUPSCALE_CPU_LEVEL_H

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))

#include <cpuid.h>
#include <stdint.h>

#if (defined(__clang__) && __clang_major__ >= 17) || \
    (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 12)
#define UP_CPU_LEVEL_BUILTIN_AVAILABLE 1
#else
#define UP_CPU_LEVEL_BUILTIN_AVAILABLE 0
#endif

#ifndef UP_CPU_LEVEL_FORCE_FALLBACK
#define UP_CPU_LEVEL_FORCE_FALLBACK 0
#endif

#if UP_CPU_LEVEL_BUILTIN_AVAILABLE && !UP_CPU_LEVEL_FORCE_FALLBACK
#define UP_CPU_LEVEL_USE_BUILTIN 1
#else
#define UP_CPU_LEVEL_USE_BUILTIN 0
#endif

typedef struct {
    unsigned max_basic;
    unsigned max_ext;
    unsigned leaf1_ecx;
    unsigned leaf1_edx;
    unsigned leaf7_ebx;
    unsigned ext1_ecx;
    uint64_t xcr0;
} up_cpu_x86_features_t;

enum {
    UP_X86_1_ECX_SSE3      = 1u << 0,
    UP_X86_1_ECX_SSSE3     = 1u << 9,
    UP_X86_1_ECX_FMA       = 1u << 12,
    UP_X86_1_ECX_CX16      = 1u << 13,
    UP_X86_1_ECX_SSE41     = 1u << 19,
    UP_X86_1_ECX_SSE42     = 1u << 20,
    UP_X86_1_ECX_MOVBE     = 1u << 22,
    UP_X86_1_ECX_POPCNT    = 1u << 23,
    UP_X86_1_ECX_OSXSAVE   = 1u << 27,
    UP_X86_1_ECX_AVX       = 1u << 28,
    UP_X86_1_ECX_F16C      = 1u << 29,

    UP_X86_1_EDX_SSE2      = 1u << 26,

    UP_X86_7_EBX_BMI       = 1u << 3,
    UP_X86_7_EBX_AVX2      = 1u << 5,
    UP_X86_7_EBX_BMI2      = 1u << 8,
    UP_X86_7_EBX_AVX512F   = 1u << 16,
    UP_X86_7_EBX_AVX512DQ  = 1u << 17,
    UP_X86_7_EBX_AVX512CD  = 1u << 28,
    UP_X86_7_EBX_AVX512BW  = 1u << 30,
    UP_X86_7_EBX_AVX512VL  = 1u << 31,

    UP_X86_EXT1_ECX_LAHF   = 1u << 0,
    UP_X86_EXT1_ECX_LZCNT  = 1u << 5,
};

static inline int up_cpu_has_all(unsigned value, unsigned mask)
{
    return (value & mask) == mask;
}

static inline int up_cpu_xcr0_has_avx(uint64_t xcr0)
{
    return (xcr0 & UINT64_C(0x6)) == UINT64_C(0x6);
}

static inline int up_cpu_xcr0_has_avx512(uint64_t xcr0)
{
    return (xcr0 & UINT64_C(0xe6)) == UINT64_C(0xe6);
}

static inline int up_cpu_features_support_v2(const up_cpu_x86_features_t *f)
{
    const unsigned ecx = UP_X86_1_ECX_SSE3 | UP_X86_1_ECX_SSSE3
        | UP_X86_1_ECX_CX16 | UP_X86_1_ECX_SSE41 | UP_X86_1_ECX_SSE42
        | UP_X86_1_ECX_POPCNT;
    return f != 0 && f->max_basic >= 1 && f->max_ext >= 0x80000001u
        && up_cpu_has_all(f->leaf1_ecx, ecx)
        && up_cpu_has_all(f->leaf1_edx, UP_X86_1_EDX_SSE2)
        && up_cpu_has_all(f->ext1_ecx, UP_X86_EXT1_ECX_LAHF);
}

static inline int up_cpu_features_support_v3(const up_cpu_x86_features_t *f)
{
    const unsigned ecx = UP_X86_1_ECX_FMA | UP_X86_1_ECX_MOVBE
        | UP_X86_1_ECX_OSXSAVE | UP_X86_1_ECX_AVX | UP_X86_1_ECX_F16C;
    const unsigned ebx = UP_X86_7_EBX_BMI | UP_X86_7_EBX_AVX2
        | UP_X86_7_EBX_BMI2;
    return up_cpu_features_support_v2(f) && f->max_basic >= 7
        && up_cpu_has_all(f->leaf1_ecx, ecx)
        && up_cpu_has_all(f->leaf7_ebx, ebx)
        && up_cpu_has_all(f->ext1_ecx, UP_X86_EXT1_ECX_LZCNT)
        && up_cpu_xcr0_has_avx(f->xcr0);
}

static inline int up_cpu_features_support_v4(const up_cpu_x86_features_t *f)
{
    const unsigned ebx = UP_X86_7_EBX_AVX512F | UP_X86_7_EBX_AVX512DQ
        | UP_X86_7_EBX_AVX512CD | UP_X86_7_EBX_AVX512BW
        | UP_X86_7_EBX_AVX512VL;
    return up_cpu_features_support_v3(f)
        && up_cpu_has_all(f->leaf7_ebx, ebx)
        && up_cpu_xcr0_has_avx512(f->xcr0);
}

static inline uint64_t up_cpu_xgetbv0(void)
{
    unsigned eax;
    unsigned edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((uint64_t)edx << 32) | eax;
}

static inline up_cpu_x86_features_t up_cpu_collect_x86_features(void)
{
    up_cpu_x86_features_t f = { 0 };
    unsigned eax;
    unsigned ebx;
    unsigned ecx;
    unsigned edx;

    f.max_basic = __get_cpuid_max(0, 0);
    f.max_ext = __get_cpuid_max(0x80000000u, 0);
    if (f.max_basic >= 1) {
        __cpuid_count(1, 0, eax, ebx, ecx, edx);
        f.leaf1_ecx = ecx;
        f.leaf1_edx = edx;
        if (ecx & UP_X86_1_ECX_OSXSAVE)
            f.xcr0 = up_cpu_xgetbv0();
    }
    if (f.max_basic >= 7) {
        __cpuid_count(7, 0, eax, ebx, ecx, edx);
        f.leaf7_ebx = ebx;
    }
    if (f.max_ext >= 0x80000001u) {
        __cpuid_count(0x80000001u, 0, eax, ebx, ecx, edx);
        f.ext1_ecx = ecx;
    }
    return f;
}

static inline int up_cpu_supports_v3(void)
{
#if UP_CPU_LEVEL_USE_BUILTIN
    return __builtin_cpu_supports("x86-64-v3");
#else
    const up_cpu_x86_features_t f = up_cpu_collect_x86_features();
    return up_cpu_features_support_v3(&f);
#endif
}

static inline int up_cpu_supports_v4(void)
{
#if UP_CPU_LEVEL_USE_BUILTIN
    return __builtin_cpu_supports("x86-64-v4");
#else
    const up_cpu_x86_features_t f = up_cpu_collect_x86_features();
    return up_cpu_features_support_v4(&f);
#endif
}

#endif /* __x86_64__ && (GNUC || clang) */
#endif /* AUTOUPSCALE_CPU_LEVEL_H */
