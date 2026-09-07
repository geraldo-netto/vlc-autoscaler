// SPDX-License-Identifier: GPL-2.0-or-later
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usm_test_util.h"
#include "prng.h"

static int adapter_clock(clockid_t id, struct timespec *time);
#define clock_gettime adapter_clock
#include "../src/usm_adaptive.h"
#undef clock_gettime

static up_usm_adaptive_t adaptive;
static long ticks;
static int optimum = 16;

static int adapter_clock(clockid_t id, struct timespec *time)
{
    (void)id;
    ticks += adaptive.tuner.current == optimum ? 50 : 200;
    time->tv_sec = ticks / 1000000L;
    time->tv_nsec = ticks % 1000000L * 1000L;
    return 0;
}

enum { WIDTH = 65, HEIGHT = 513, PITCH = 71, BYTES = PITCH * HEIGHT };
typedef struct { uint8_t pixels[BYTES], reference[BYTES], scratch[BYTES]; } frames_t;

static int one_frame(frames_t *f, usm_pool_t **pool, int frame)
{
    up_fill_random(f->pixels, BYTES, (uint32_t)frame * 7919u + 17u);
    memcpy(f->reference, f->pixels, BYTES);
    UP_TEST_USM_APPLY_PLANE(f->reference, PITCH, f->pixels, PITCH,
                            WIDTH, HEIGHT, 51, f->scratch);
    if (up_usm_adaptive_apply(&adaptive, pool, f->pixels, PITCH, 51) != 0)
        return 1;
    return memcmp(f->reference, f->pixels, BYTES) != 0;
}

int main(void)
{
    frames_t *frames = calloc(1, sizeof *frames);
    usm_pool_t *pool = up_usm_pool_create(12, WIDTH, HEIGHT, 8);
    up_usm_adaptive_init(&adaptive, 12, 64, WIDTH, HEIGHT, 8);
    int rc = frames == NULL || pool == NULL;
    for (int i = 0; !rc && i < 1600; i++) {
        if (i == 800) optimum = 4;
        rc = one_frame(frames, &pool, i);
        if (i == 799 && adaptive.tuner.best != 16) rc = 1;
    }
    if (adaptive.tuner.best != 4) rc = 1;
    up_usm_adaptive_stop(&adaptive);
    up_usm_pool_destroy(pool);
    free(frames);
    printf("adaptive pools: 1600 changing frames, 1..64 workers, %s\n",
           rc ? "FAILED" : "byte-identical");
    return rc;
}
