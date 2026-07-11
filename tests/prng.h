// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * prng.h — one deterministic PRNG for test/fuzzer buffer fills (DUP-2)
 *****************************************************************************
 * Several tests carried their own byte-fill helper with divergent xorshift
 * constants, so "deterministic random" meant a different stream per file.
 * This is the single canonical xorshift32 (shift triple 13/17/5); callers that
 * only need a filled buffer use up_fill_random, callers that draw scalars
 * (e.g. per-plane geometry loops) use up_xs32 directly.
 *****************************************************************************/
#ifndef TEST_PRNG_H
#define TEST_PRNG_H

#include <stddef.h>
#include <stdint.h>

/* Advance and return the state. Seed 0 is remapped so the state, which
 * xorshift never drives back to zero from a nonzero value, cannot stick. */
static inline uint32_t up_xs32(uint32_t *s)
{
    uint32_t v = *s ? *s : 0x9E3779B9u;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    *s = v;
    return v;
}

/* Fill n bytes deterministically from seed. */
static inline void up_fill_random(uint8_t *buf, size_t n, uint32_t seed)
{
    uint32_t s = seed ? seed : 0x9E3779B9u;
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)up_xs32(&s);
}

#endif /* TEST_PRNG_H */
