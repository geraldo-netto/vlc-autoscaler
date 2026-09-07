// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_USM_ADAPTIVE_H
#define AUTOUPSCALE_USM_ADAPTIVE_H

#include "usm_pool.h"
#include "worker_tuner.h"
#include <time.h>

typedef struct {
    up_worker_tuner_t tuner;
    usm_pool_t *trial;
    int trial_count, width, height, stripe_rows;
    bool enabled, stopped, retained;
    bool frame_pending, frame_timed;
    struct timespec frame_start;
} up_usm_adaptive_t;

static inline void up_usm_adaptive_init(up_usm_adaptive_t *a, int initial,
                                        int limit, int width, int height,
                                        int stripe_rows)
{
    memset(a, 0, sizeof *a);
    const int rows = stripe_rows > 0 ? stripe_rows : 8;
    const int by_height = height / rows;
    if (limit > by_height) limit = by_height;
    up_worker_tuner_init(&a->tuner, initial, limit);
    a->width = width;
    a->height = height;
    a->stripe_rows = stripe_rows;
    a->enabled = a->tuner.limit > 1;
}

static inline void up_usm_adaptive_clear_trial(up_usm_adaptive_t *a)
{
    if (a->trial != NULL) up_usm_pool_destroy(a->trial);
    a->trial = NULL;
    a->trial_count = 0;
}

static inline void up_usm_adaptive_stop(up_usm_adaptive_t *a)
{
    up_usm_adaptive_clear_trial(a);
    a->enabled = false;
    a->stopped = true;
    a->frame_pending = false;
}

static inline void up_usm_adaptive_begin(up_usm_adaptive_t *a)
{
    if (!a->enabled) return;
    a->frame_pending = true;
    a->frame_timed = clock_gettime(CLOCK_MONOTONIC, &a->frame_start) == 0;
}

static inline bool up_usm_adaptive_take_start(up_usm_adaptive_t *a,
                                              struct timespec *start)
{
    if (!a->frame_pending) return clock_gettime(CLOCK_MONOTONIC, start) == 0;
    a->frame_pending = false;
    *start = a->frame_start;
    return a->frame_timed;
}

static inline usm_pool_t *up_usm_adaptive_select(up_usm_adaptive_t *a,
                                                usm_pool_t *best)
{
    if (a->tuner.current == a->tuner.best) return best;
    if (a->trial_count == a->tuner.current) return a->trial;
    up_usm_adaptive_clear_trial(a);
    a->trial = up_usm_pool_create(a->tuner.current, a->width, a->height,
                                  a->stripe_rows);
    if (a->trial == NULL) {
        up_usm_adaptive_stop(a);
        return best;
    }
    a->trial_count = a->tuner.current;
    return a->trial;
}

static inline bool up_usm_adaptive_elapsed(const struct timespec *start,
                                           double *elapsed)
{
    struct timespec end;
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) return false;
    *elapsed = ((double)end.tv_sec - (double)start->tv_sec) * 1.0e6
             + ((double)end.tv_nsec - (double)start->tv_nsec) / 1000.0;
    return isfinite(*elapsed) && *elapsed > 0.0;
}

static inline void up_usm_adaptive_accept(up_usm_adaptive_t *a,
                                          usm_pool_t **best, double elapsed)
{
    const int previous = a->tuner.best;
    const up_tuner_phase_t phase = a->tuner.phase;
    up_worker_tuner_observe(&a->tuner, elapsed);
    if (a->tuner.best == previous) {
        if (phase == UP_TUNER_CONFIRM && a->tuner.phase == UP_TUNER_BASE)
            up_usm_adaptive_clear_trial(a);
        return;
    }
    up_usm_pool_destroy(*best);
    *best = a->trial;
    a->trial = NULL;
    a->trial_count = 0;
}

static inline int up_usm_adaptive_recover(up_usm_adaptive_t *a,
                                          usm_pool_t *best, int status,
                                          uint8_t *pixels, int pitch, int amount)
{
    up_usm_adaptive_stop(a);
    if (status == UP_USM_APPLY_OUTPUT_UNCERTAIN) {
        a->retained = true;
        return status;
    }
    return up_usm_pool_apply(best, pixels, pitch, pixels, pitch, amount);
}

static inline int up_usm_adaptive_apply(up_usm_adaptive_t *a,
                                        usm_pool_t **best, uint8_t *pixels,
                                        int pitch, int amount)
{
    a->retained = false;
    usm_pool_t *selected = a->enabled ? up_usm_adaptive_select(a, *best) : *best;
    struct timespec start = { 0 };
    const bool timed = a->enabled && up_usm_adaptive_take_start(a, &start);
    const int status = up_usm_pool_apply(selected, pixels, pitch,
                                         pixels, pitch, amount);
    if (status != UP_USM_APPLY_OK && selected != *best)
        return up_usm_adaptive_recover(a, *best, status, pixels, pitch, amount);
    if (status != UP_USM_APPLY_OK || !a->enabled) return status;
    if (up_usm_pool_effective_threads(selected) != a->tuner.current) {
        up_usm_adaptive_stop(a);
        return status;
    }
    double elapsed;
    if (!timed || !up_usm_adaptive_elapsed(&start, &elapsed)) {
        up_usm_adaptive_stop(a);
        return status;
    }
    up_usm_adaptive_accept(a, best, elapsed);
    return status;
}

#endif
