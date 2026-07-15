// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_CONTENT_PROBE_REFERENCE_H
#define AUTOUPSCALE_CONTENT_PROBE_REFERENCE_H

#include "../src/content_probe.h"

static inline uint64_t up_laplacian_variance(const uint8_t *plane,
                                             int stride, int w, int h,
                                             uint64_t *n_samples_out)
{
    if (n_samples_out) *n_samples_out = 0;
    if (plane == NULL || stride <= 0 || stride < w || w <= 2 || h <= 2)
        return 0;
    uint64_t sum_sq = 0;
    uint64_t n = 0;
    for (int y = UP_PROBE_GRID_STEP; y < h - 1; y += UP_PROBE_GRID_STEP) {
        const uint8_t *row = plane + (size_t)y * (size_t)stride;
        const uint8_t *row_up = row - stride;
        const uint8_t *row_dn = row + stride;
        for (int x = UP_PROBE_GRID_STEP; x < w - 1;
             x += UP_PROBE_GRID_STEP) {
            int lap = 4 * row[x]
                    - (row_up[x] + row_dn[x] + row[x - 1] + row[x + 1]);
            sum_sq += (uint64_t)(lap * lap);
            n++;
        }
    }
    if (n_samples_out) *n_samples_out = n;
    return sum_sq;
}

typedef struct {
    uint64_t sum;
    uint64_t n;
} up_edge_acc_t;

static inline void up_block_edge_vertical(const uint8_t *plane,
                                          int stride, int w, int h,
                                          int b, int step,
                                          up_edge_acc_t *acc)
{
    for (int y = 0; y < h; y += step) {
        const uint8_t *row = plane + (size_t)y * (size_t)stride;
        for (int x = b; x < w; x += b) {
            int diff = (int)row[x] - (int)row[x - 1];
            if (diff < 0) diff = -diff;
            acc->sum += (uint64_t)diff;
            acc->n++;
        }
    }
}

static inline void up_block_edge_horizontal(const uint8_t *plane,
                                            int stride, int w, int h,
                                            int b, int step,
                                            up_edge_acc_t *acc)
{
    for (int y = b; y < h; y += b) {
        const uint8_t *row = plane + (size_t)y * (size_t)stride;
        const uint8_t *row_up = row - stride;
        for (int x = 0; x < w; x += step) {
            int diff = (int)row[x] - (int)row_up[x];
            if (diff < 0) diff = -diff;
            acc->sum += (uint64_t)diff;
            acc->n++;
        }
    }
}

static inline uint64_t up_block_edge_strength(const uint8_t *plane,
                                              int stride, int w, int h,
                                              uint64_t *n_samples_out)
{
    if (n_samples_out) *n_samples_out = 0;
    if (plane == NULL || stride <= 0 || stride < w
        || w <= UP_PROBE_BLOCK_SIZE || h <= UP_PROBE_BLOCK_SIZE)
        return 0;
    up_edge_acc_t acc = { 0, 0 };
    up_block_edge_vertical(plane, stride, w, h, UP_PROBE_BLOCK_SIZE,
                           UP_PROBE_GRID_STEP, &acc);
    up_block_edge_horizontal(plane, stride, w, h, UP_PROBE_BLOCK_SIZE,
                             UP_PROBE_GRID_STEP, &acc);
    if (n_samples_out) *n_samples_out = acc.n;
    return acc.sum;
}

#endif
