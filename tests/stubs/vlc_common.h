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

/* VLC's real headers expand every msg_* to a printf-like msg_Generic that
 * reads all of its arguments. The stub forwards them to a variadic sink so the
 * object AND the format arguments are "used" — otherwise cppcheck (which parses
 * these stubs) reports false unreadVariable warnings for values passed only to
 * a log call. No output is produced. */
static inline void vlc_stub_msg_sink(void *obj, ...) { (void)obj; }
#define msg_Dbg(obj, ...)  vlc_stub_msg_sink((obj), __VA_ARGS__)
#define msg_Warn(obj, ...) vlc_stub_msg_sink((obj), __VA_ARGS__)
#define msg_Err(obj, ...)  vlc_stub_msg_sink((obj), __VA_ARGS__)
#define msg_Info(obj, ...) vlc_stub_msg_sink((obj), __VA_ARGS__)

#endif
