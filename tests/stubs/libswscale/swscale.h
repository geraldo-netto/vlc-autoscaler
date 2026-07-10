#ifndef TEST_STUB_LIBSWSCALE_SWSCALE_H
#define TEST_STUB_LIBSWSCALE_SWSCALE_H

#include <libavutil/pixfmt.h>
#include <stdint.h>

struct SwsContext;
struct SwsFilter;

#define SWS_FAST_BILINEAR  0x0001
#define SWS_BICUBIC        0x0004
#define SWS_LANCZOS        0x0200
#define SWS_ACCURATE_RND   0x40000
#define SWS_FULL_CHR_H_INT 0x02000
#define SWS_FULL_CHR_H_INP 0x04000

struct SwsContext *sws_getContext(
    int src_w, int src_h, enum AVPixelFormat src_format,
    int dst_w, int dst_h, enum AVPixelFormat dst_format, int flags,
    struct SwsFilter *src_filter, struct SwsFilter *dst_filter,
    const double *param);
int sws_scale(struct SwsContext *ctx, const uint8_t *const src[],
              const int src_stride[], int src_y, int src_h,
              uint8_t *const dst[], const int dst_stride[]);
void sws_freeContext(struct SwsContext *ctx);

#endif
