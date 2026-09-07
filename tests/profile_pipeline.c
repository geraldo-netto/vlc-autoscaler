// SPDX-License-Identifier: GPL-2.0-or-later
#define ZIMG_TEST_DEFINE_MODULE_NAME
#include "zimg_test_util.h"
#include "profile_stage.h"
#include "cli_parse.h"
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

#define PROFILE_INPUTS 8
#define PROFILE_WARMUP 128

typedef struct {
    long zimg, usm, width, height, frames, pin, detail, content, period;
} args_t;

typedef struct {
    double total, zimg, usm;
    up_profile_frame_t ztrace, utrace;
} sample_t;

typedef struct {
    scaler_ctx_t ctx;
    zt_pic_t input[PROFILE_INPUTS], output;
    usm_pool_t *usm;
    sample_t *samples;
    up_profile_sched_t zbefore, ubefore, zafter, uafter;
    struct rusage before, after;
    double elapsed_us, cpu_us;
} pipeline_t;

static double clock_us(clockid_t id)
{
    struct timespec ts;
    if (clock_gettime(id, &ts) != 0) { perror("clock_gettime"); abort(); }
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int parse(int argc, char **argv, args_t *a)
{
    if (argc != 10 && argc != 11) return -1;
    const long low[] = { 1, 0, 64, 64, 1, 0, 0, 0, 0 };
    const long high[] = { 64, 64, 4096, 2160, 1000000, 1, 2, 1, 100000 };
    long *fields[] = { &a->zimg, &a->usm, &a->width, &a->height, &a->frames,
                       &a->pin, &a->detail, &a->content, &a->period };
    for (int i = 0; i < 9; i++)
        if (!up_cli_parse_long(argv[i + 1], low[i], high[i], fields[i])) return -1;
    return a->width % 4 || a->height % 4 ? -1 : 0;
}

static void fill_smooth(zt_pic_t *pic, int frame)
{
    zt_pic_memset(pic, 128);
    plane_t *p = &pic->pic.p[0];
    for (int y = 0; y < p->i_visible_lines; y++)
        for (int x = 0; x < p->i_visible_pitch; x++)
            p->p_pixels[(size_t)y * (size_t)p->i_pitch + (size_t)x] =
                (uint8_t)(32 + ((x / 8 + y / 8 + frame * 3) % 192));
}

static int initialize(pipeline_t *p, const args_t *a)
{
    const int w = (int)a->width, h = (int)a->height;
    for (int i = 0; i < PROFILE_INPUTS; i++) {
        if (zt_pic_alloc(&p->input[i], VLC_CODEC_I420, w / 2, h / 2)) return -1;
        if (a->content) fill_smooth(&p->input[i], i);
        else zt_pic_fill(&p->input[i], 0x12345678u + (uint32_t)i);
    }
    if (zt_pic_alloc(&p->output, VLC_CODEC_I420, w, h)) return -1;
    zt_ctx_init(&p->ctx, VLC_CODEC_I420, w / 2, h / 2, w, h, (int)a->zimg, 1);
    p->ctx.algo = UP_ALGO_SPLINE36;
    p->ctx.pin_cpus = (int)a->pin;
    if (p->ctx.backend->open(&p->ctx)) return -1;
    if (a->usm) {
        p->usm = up_usm_pool_create((int)a->usm, w, h, 0);
        if (!p->usm) return -1;
    }
    p->samples = calloc((size_t)a->frames, sizeof *p->samples);
    return p->samples ? 0 : -1;
}

static void destroy(pipeline_t *p)
{
    up_usm_pool_destroy(p->usm);
    if (p->ctx.priv) p->ctx.backend->close(&p->ctx);
    for (int i = 0; i < PROFILE_INPUTS; i++) zt_pic_free(&p->input[i]);
    zt_pic_free(&p->output);
    free(p->samples);
}

static int frame(pipeline_t *p, int index, sample_t *sample)
{
    const double start = clock_us(CLOCK_MONOTONIC);
    if (p->ctx.backend->process(&p->ctx, &p->input[index % PROFILE_INPUTS].pic,
                                &p->output.pic) != SCALER_PROCESS_OK) return -1;
    const double scaled = clock_us(CLOCK_MONOTONIC);
    plane_t *luma = &p->output.pic.p[0];
    if (p->usm && up_usm_pool_apply(p->usm, luma->p_pixels, luma->i_pitch,
                                   luma->p_pixels, luma->i_pitch, 51)) return -1;
    const double end = clock_us(CLOCK_MONOTONIC);
    *sample = (sample_t){ end - start, scaled - start, end - scaled,
                          up_profile_zimg_frame(), up_profile_usm_frame() };
    return 0;
}

static int sched_snapshot(pipeline_t *p, up_profile_sched_t *z,
                           up_profile_sched_t *u)
{
    if (up_profile_zimg_sched(&p->ctx, z)) return -1;
    return p->usm ? up_profile_usm_sched(p->usm, u) : 0;
}

static int wait_period(double deadline)
{
    struct timespec ts = { .tv_sec = (time_t)(deadline / 1e6) };
    ts.tv_nsec = (long)((deadline - (double)ts.tv_sec * 1e6) * 1e3);
    int rc;
    do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL); }
    while (rc == EINTR);
    return rc;
}

static int run(pipeline_t *p, const args_t *a)
{
    sample_t warmup;
    for (int i = 0; i < PROFILE_WARMUP; i++)
        if (frame(p, i, &warmup)) return -1;
    if (sched_snapshot(p, &p->zbefore, &p->ubefore)) return -1;
    if (getrusage(RUSAGE_SELF, &p->before)) return -1;
    const double cpu = clock_us(CLOCK_PROCESS_CPUTIME_ID);
    const double start = clock_us(CLOCK_MONOTONIC);
    for (int i = 0; i < a->frames; i++) {
        if (a->period && wait_period(start + (double)i * (double)a->period)) return -1;
        if (frame(p, i, &p->samples[i])) return -1;
    }
    p->elapsed_us = clock_us(CLOCK_MONOTONIC) - start;
    p->cpu_us = clock_us(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    if (getrusage(RUSAGE_SELF, &p->after)) return -1;
    return sched_snapshot(p, &p->zafter, &p->uafter);
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void report_times(const sample_t *samples, int n, int field,
                          const char *name, double *sorted)
{
    double total = 0.0;
    for (int i = 0; i < n; i++) {
        const double values[] = { samples[i].total, samples[i].zimg, samples[i].usm };
        sorted[i] = values[field];
        total += sorted[i];
    }
    qsort(sorted, (size_t)n, sizeof *sorted, cmp_double);
    printf("\"%s_mean\":%.6f,\"%s_p50\":%.6f,\"%s_p95\":%.6f,\"%s_p99\":%.6f,",
           name, total / n, name, sorted[(n - 1) / 2],
           name, sorted[(size_t)(n - 1) * 95 / 100],
           name, sorted[(size_t)(n - 1) * 99 / 100]);
}

static uint64_t picture_hash(const zt_pic_t *pic)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int k = 0; k < pic->pic.i_planes; k++) {
        const plane_t *p = &pic->pic.p[k];
        for (int y = 0; y < p->i_visible_lines; y++)
            for (int x = 0; x < p->i_visible_pitch; x++) {
                hash ^= p->p_pixels[(size_t)y * (size_t)p->i_pitch + (size_t)x];
                hash *= UINT64_C(1099511628211);
            }
    }
    return hash;
}

static void report_trace(const sample_t *samples, int n, int zimg)
{
    up_profile_frame_t sum = {0};
    for (int i = 0; i < n; i++) {
        const up_profile_frame_t f = zimg ? samples[i].ztrace : samples[i].utrace;
        sum.dispatch_us += f.dispatch_us;
        sum.first_start_us += f.first_start_us;
        sum.last_start_us += f.last_start_us;
        sum.min_work_us += f.min_work_us;
        sum.max_work_us += f.max_work_us;
        sum.mean_work_us += f.mean_work_us;
        sum.handoff_us += f.handoff_us;
        sum.worker_cpu_us += f.worker_cpu_us;
    }
    printf("\"%strace\":[%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f],",
           zimg ? "z" : "u", sum.dispatch_us / n, sum.first_start_us / n,
           sum.last_start_us / n, sum.min_work_us / n, sum.max_work_us / n,
           sum.mean_work_us / n, sum.handoff_us / n, sum.worker_cpu_us / n);
}

static void report_sched(const up_profile_sched_t *before,
                          const up_profile_sched_t *after, const char *name)
{
    printf("\"%s_runtime_ns\":%llu,\"%s_runqueue_ns\":%llu,\"%s_slices\":%llu,",
           name, (unsigned long long)(after->runtime_ns - before->runtime_ns),
           name, (unsigned long long)(after->runqueue_ns - before->runqueue_ns),
           name, (unsigned long long)(after->slices - before->slices));
}

static int report(const pipeline_t *p, const args_t *a)
{
    double *sorted = malloc((size_t)a->frames * sizeof *sorted);
    if (!sorted) return -1;
    printf("{\"zimg_effective\":%d,\"usm_effective\":%d,\"rows\":%d,\"cols\":%d,",
           p->zafter.workers, p->uafter.workers, p->zafter.rows, p->zafter.cols);
    const char *names[] = { "frame", "zimg", "usm" };
    for (int i = 0; i < 3; i++) report_times(p->samples, (int)a->frames, i, names[i], sorted);
    free(sorted);
    report_trace(p->samples, (int)a->frames, 1);
    report_trace(p->samples, (int)a->frames, 0);
    report_sched(&p->zbefore, &p->zafter, "z");
    report_sched(&p->ubefore, &p->uafter, "u");
    printf("\"cpu_us\":%.6f,\"elapsed_us\":%.6f,\"nvcsw\":%ld,\"nivcsw\":%ld,"
           "\"minflt\":%ld,\"maxrss_kb\":%ld,\"hash\":\"%016llx\"}\n",
           p->cpu_us, p->elapsed_us, p->after.ru_nvcsw - p->before.ru_nvcsw,
           p->after.ru_nivcsw - p->before.ru_nivcsw,
           p->after.ru_minflt - p->before.ru_minflt, p->after.ru_maxrss,
           (unsigned long long)picture_hash(&p->output));
    return ferror(stdout) ? -1 : 0;
}

static void trace_row(FILE *f, const up_profile_frame_t *t)
{
    fprintf(f, ",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
            t->dispatch_us, t->first_start_us, t->last_start_us,
            t->min_work_us, t->max_work_us, t->mean_work_us,
            t->handoff_us, t->worker_cpu_us);
}

static int write_samples(const pipeline_t *p, int n, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "frame,total,zimg,usm,zdispatch,zfirst,zlast,zmin,zmax,zmean,zhandoff,zcpu,"
               "udispatch,ufirst,ulast,umin,umax,umean,uhandoff,ucpu\n");
    for (int i = 0; i < n; i++) {
        const sample_t *s = &p->samples[i];
        fprintf(f, "%d,%.3f,%.3f,%.3f", i, s->total, s->zimg, s->usm);
        trace_row(f, &s->ztrace);
        trace_row(f, &s->utrace);
        fputc('\n', f);
    }
    const int failed = ferror(f);
    return fclose(f) || failed ? -1 : 0;
}

int main(int argc, char **argv)
{
    args_t a;
    if (parse(argc, argv, &a)) {
        fprintf(stderr, "usage: %s zworkers uworkers width height frames pin detail content period-us [samples.csv]\n", argv[0]);
        return 2;
    }
    pipeline_t p = {0};
    up_profile_zimg_mode((int)a.detail);
    up_profile_usm_mode((int)a.detail);
    int rc = initialize(&p, &a);
    if (!rc) rc = run(&p, &a);
    if (!rc) rc = report(&p, &a);
    if (!rc && argc == 11) rc = write_samples(&p, (int)a.frames, argv[10]);
    destroy(&p);
    return rc ? 1 : 0;
}
