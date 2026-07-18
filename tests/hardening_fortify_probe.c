// SPDX-License-Identifier: GPL-2.0-or-later
#include <stddef.h>
#include <string.h>

#if !defined(_FORTIFY_SOURCE) || _FORTIFY_SOURCE < 2
#error "_FORTIFY_SOURCE level 2 or greater is required"
#endif

/*
 * Build-only probe inspected for __memcpy_chk. It is never run or installed.
 */
__attribute__((noinline, visibility("default")))
int up_hardening_fortify_probe(const char *src, size_t bytes)
{
    char dst[8] = {0};
    memcpy(dst, src, bytes);
    return (unsigned char)dst[0];
}
