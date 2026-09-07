// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_PROFILE_STAGE_H
#define AUTOUPSCALE_PROFILE_STAGE_H

#include "../src/scaler.h"
#include "../src/usm_pool.h"
#include <stdint.h>

typedef struct {
    double dispatch_us, first_start_us, last_start_us;
    double min_work_us, max_work_us, mean_work_us, handoff_us;
    double worker_cpu_us;
} up_profile_frame_t;

typedef struct {
    int workers, rows, cols;
    uint64_t runtime_ns, runqueue_ns, slices;
} up_profile_sched_t;

void up_profile_zimg_mode(int mode);
void up_profile_usm_mode(int mode);
up_profile_frame_t up_profile_zimg_frame(void);
up_profile_frame_t up_profile_usm_frame(void);
int up_profile_zimg_sched(const scaler_ctx_t *ctx, up_profile_sched_t *out);
int up_profile_usm_sched(const usm_pool_t *pool, up_profile_sched_t *out);

#endif
