// SPDX-License-Identifier: GPL-2.0-or-later
#include "profile_internal.h"
#define up_worker_pool_dispatch up_profile_dispatch
#include "../src/scaler_zimg.c"
#undef up_worker_pool_dispatch

void up_profile_zimg_mode(int mode) { up_profile_trace.mode = mode; }
up_profile_frame_t up_profile_zimg_frame(void) { return up_profile_trace.frame; }

int up_profile_zimg_sched(const scaler_ctx_t *ctx, up_profile_sched_t *out)
{
    const zimg_priv_t *p = ctx->priv;
    *out = (up_profile_sched_t){ .rows = p->plan.n_rows, .cols = p->plan.n_cols };
    return up_profile_pool_sched(&p->pool, out);
}
