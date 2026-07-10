// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_CLI_PARSE_H
#define AUTOUPSCALE_CLI_PARSE_H

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

static inline int up_cli_parse_long(const char *text, long minimum,
                                    long maximum, long *value_out)
{
    if (text == NULL || value_out == NULL || minimum > maximum) return 0;
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return 0;
    if (value < minimum || value > maximum) return 0;
    *value_out = value;
    return 1;
}

#endif
