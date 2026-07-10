// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * cpu_level.h — runtime x86-64 micro-architecture level probes (PORT-6)
 *****************************************************************************
 * Objects compiled at -march=x86-64-v3/v4 may use ANY instruction of that
 * level (BMI/BMI2/F16C/FMA/LZCNT/MOVBE at v3; +AVX512CD/DQ/VL at v4), so a
 * runtime gate must prove the whole level, not a headline feature.
 * GCC >= 12 and Clang >= 17 accept the "x86-64-vN" strings directly; older
 * compilers fall back to a multi-feature conjunction (the strongest probe
 * their __builtin_cpu_supports vocabulary allows).
 *****************************************************************************/

#ifndef AUTOUPSCALE_CPU_LEVEL_H
#define AUTOUPSCALE_CPU_LEVEL_H

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))

#if (defined(__clang__) && __clang_major__ >= 17) || \
    (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 12)
#define UP_CPU_LEVEL_PROBE 1
#else
#define UP_CPU_LEVEL_PROBE 0
#endif

static inline int up_cpu_supports_v3(void)
{
#if UP_CPU_LEVEL_PROBE
    return __builtin_cpu_supports("x86-64-v3");
#else
    return __builtin_cpu_supports("avx2")
        && __builtin_cpu_supports("fma")
        && __builtin_cpu_supports("bmi")
        && __builtin_cpu_supports("bmi2");
#endif
}

static inline int up_cpu_supports_v4(void)
{
#if UP_CPU_LEVEL_PROBE
    return __builtin_cpu_supports("x86-64-v4");
#else
    return up_cpu_supports_v3()
        && __builtin_cpu_supports("avx512f")
        && __builtin_cpu_supports("avx512bw")
        && __builtin_cpu_supports("avx512cd")
        && __builtin_cpu_supports("avx512dq")
        && __builtin_cpu_supports("avx512vl");
#endif
}

#endif /* __x86_64__ && (GNUC || clang) */
#endif /* AUTOUPSCALE_CPU_LEVEL_H */
