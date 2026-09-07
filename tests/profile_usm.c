// SPDX-License-Identifier: GPL-2.0-or-later
#include "profile_internal.h"
#define up_worker_pool_dispatch up_profile_dispatch
#include "../src/usm_pool.c"
#undef up_worker_pool_dispatch

void up_profile_usm_mode(int mode) { up_profile_trace.mode = mode; }
up_profile_frame_t up_profile_usm_frame(void) { return up_profile_trace.frame; }

int up_profile_usm_sched(const usm_pool_t *pool, up_profile_sched_t *out)
{
    *out = (up_profile_sched_t){ 0 };
    return up_profile_pool_sched(&pool->pool, out);
}
