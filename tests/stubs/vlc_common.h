#ifndef TEST_STUB_VLC_COMMON_H
#define TEST_STUB_VLC_COMMON_H

#include <stdint.h>

typedef uint32_t vlc_fourcc_t;
typedef struct vlc_object_t { int unused; } vlc_object_t;

#define VLC_FOURCC(a, b, c, d) \
    ((vlc_fourcc_t)(a) | ((vlc_fourcc_t)(b) << 8) \
     | ((vlc_fourcc_t)(c) << 16) | ((vlc_fourcc_t)(d) << 24))

#define VLC_CODEC_I420  VLC_FOURCC('I', '4', '2', '0')
#define VLC_CODEC_YV12  VLC_FOURCC('Y', 'V', '1', '2')
#define VLC_CODEC_NV12  VLC_FOURCC('N', 'V', '1', '2')
#define VLC_CODEC_NV21  VLC_FOURCC('N', 'V', '2', '1')
#define VLC_CODEC_I422  VLC_FOURCC('I', '4', '2', '2')
#define VLC_CODEC_I444  VLC_FOURCC('I', '4', '4', '4')
#define VLC_CODEC_RGB24 VLC_FOURCC('R', 'V', '2', '4')
#define VLC_CODEC_RGBA  VLC_FOURCC('R', 'G', 'B', 'A')
#define VLC_CODEC_BGRA  VLC_FOURCC('B', 'G', 'R', 'A')

#define msg_Dbg(obj, ...) ((void)(obj))

#endif
