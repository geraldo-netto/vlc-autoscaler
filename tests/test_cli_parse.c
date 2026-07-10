// SPDX-License-Identifier: GPL-2.0-or-later
#include "cli_parse.h"

#include <limits.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) do {                                                      \
    if (!(expr)) {                                                            \
        fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);   \
        failures++;                                                           \
    }                                                                         \
} while (0)

static void test_valid_values(void)
{
    long value = 99;
    CHECK(up_cli_parse_long("-7", -7, 12, &value));
    CHECK(value == -7);
    CHECK(up_cli_parse_long("12", -7, 12, &value));
    CHECK(value == 12);
    CHECK(up_cli_parse_long("+3", -7, 12, &value));
    CHECK(value == 3);
}

static void test_invalid_text(void)
{
    long value = 41;
    CHECK(!up_cli_parse_long(NULL, 0, 10, &value));
    CHECK(!up_cli_parse_long("1", 0, 10, NULL));
    CHECK(!up_cli_parse_long("1", 10, 0, &value));
    CHECK(!up_cli_parse_long("", 0, 10, &value));
    CHECK(!up_cli_parse_long("x", 0, 10, &value));
    CHECK(!up_cli_parse_long("1x", 0, 10, &value));
    CHECK(!up_cli_parse_long("999999999999999999999999", LONG_MIN,
                             LONG_MAX, &value));
    CHECK(!up_cli_parse_long("-999999999999999999999999", LONG_MIN,
                             LONG_MAX, &value));
    CHECK(value == 41);
}

static void test_range_rejection(void)
{
    long value = 17;
    CHECK(!up_cli_parse_long("4", 5, 10, &value));
    CHECK(!up_cli_parse_long("11", 5, 10, &value));
    CHECK(value == 17);
}

int main(void)
{
    test_valid_values();
    test_invalid_text();
    test_range_rejection();
    if (failures != 0) return 1;
    puts("cli_parse: all tests passed");
    return 0;
}
