// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_WORKER_TUNER_H
#define AUTOUPSCALE_WORKER_TUNER_H

#include "thread_policy.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define UP_TUNER_SAMPLES 16
#define UP_TUNER_WARMUP 2
#define UP_TUNER_RECHECK_FRAMES 16384
#define UP_TUNER_COOLDOWN_FRAMES 256

typedef enum {
    UP_TUNER_BASE,
    UP_TUNER_TRIAL,
    UP_TUNER_CONFIRM,
    UP_TUNER_SETTLED,
} up_tuner_phase_t;

typedef struct {
    int limit, best, current, candidate, cursor;
    int used, warmup, remaining, drift;
    unsigned changes;
    double samples[UP_TUNER_SAMPLES];
    double base_us, trial_us, steady_us;
    up_tuner_phase_t phase;
} up_worker_tuner_t;

static inline void up_worker_tuner_init(up_worker_tuner_t *t, int initial,
                                         int limit)
{
    memset(t, 0, sizeof *t);
    if (limit < 1) limit = 1;
    if (limit > UP_THREADS_MAX) limit = UP_THREADS_MAX;
    if (initial < 1) initial = 1;
    if (initial > limit) initial = limit;
    t->limit = limit;
    t->best = t->current = initial;
    t->warmup = UP_TUNER_WARMUP;
}

static inline int up_tuner_next_count(int previous, int limit)
{
    static const int counts[] = { 1, 2, 4, 8, 12, 16, 24, 32, 48, 64 };
    if (previous >= limit) return 0;
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++)
        if (counts[i] > previous)
            return counts[i] < limit ? counts[i] : limit;
    return 0;
}

static inline double up_tuner_median(double *values, int count)
{
    for (int i = 1; i < count; i++) {
        const double value = values[i];
        int j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
    return values[count / 2 - 1] / 2.0 + values[count / 2] / 2.0;
}

static inline void up_tuner_settle(up_worker_tuner_t *t, double elapsed)
{
    t->phase = UP_TUNER_SETTLED;
    t->current = t->best;
    t->steady_us = elapsed;
    t->remaining = UP_TUNER_RECHECK_FRAMES;
    t->drift = 0;
}

static inline void up_tuner_begin_trial(up_worker_tuner_t *t, double elapsed)
{
    t->cursor = up_tuner_next_count(t->cursor, t->limit);
    if (t->cursor == t->best)
        t->cursor = up_tuner_next_count(t->cursor, t->limit);
    if (t->cursor == 0) {
        up_tuner_settle(t, elapsed);
        return;
    }
    t->base_us = elapsed;
    t->candidate = t->current = t->cursor;
    t->phase = UP_TUNER_TRIAL;
}

static inline void up_tuner_confirm(up_worker_tuner_t *t, double elapsed)
{
    const double reference = t->base_us < elapsed ? t->base_us : elapsed;
    if (t->trial_us < reference * 0.95) {
        t->best = t->candidate;
        t->changes++;
    }
    t->current = t->best;
    t->phase = UP_TUNER_BASE;
}

static inline void up_tuner_monitor(up_worker_tuner_t *t, double elapsed)
{
    if (t->remaining > UP_TUNER_RECHECK_FRAMES - UP_TUNER_COOLDOWN_FRAMES)
        return;
    const bool shifted = elapsed > t->steady_us * 1.35
                      || elapsed < t->steady_us * 0.65;
    t->drift = shifted ? t->drift + 1 : 0;
    if (t->remaining > 0 && t->drift < 3) return;
    t->cursor = 0;
    t->phase = UP_TUNER_BASE;
    t->drift = 0;
}

static inline void up_tuner_window(up_worker_tuner_t *t, double elapsed)
{
    switch (t->phase) {
        case UP_TUNER_BASE:
            up_tuner_begin_trial(t, elapsed);
            break;
        case UP_TUNER_TRIAL:
            t->trial_us = elapsed;
            t->current = t->best;
            t->phase = UP_TUNER_CONFIRM;
            break;
        case UP_TUNER_CONFIRM:
            up_tuner_confirm(t, elapsed);
            break;
        case UP_TUNER_SETTLED:
            up_tuner_monitor(t, elapsed);
            break;
    }
}

static inline bool up_tuner_trial_too_slow(up_worker_tuner_t *t)
{
    return t->phase == UP_TUNER_TRIAL && t->used == 4
        && up_tuner_median(t->samples, 4) > t->base_us * 1.5;
}

static inline void up_worker_tuner_observe(up_worker_tuner_t *t, double elapsed)
{
    if (!isfinite(elapsed) || elapsed <= 0.0) return;
    if (t->warmup > 0) { t->warmup--; return; }
    if (t->remaining > 0) t->remaining--;
    t->samples[t->used++] = elapsed;
    if (t->used < UP_TUNER_SAMPLES && !up_tuner_trial_too_slow(t)) return;
    const int previous = t->current;
    up_tuner_window(t, up_tuner_median(t->samples, t->used));
    t->used = 0;
    if (previous != t->current) t->warmup = UP_TUNER_WARMUP;
}

#endif
