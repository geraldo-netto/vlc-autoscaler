#ifndef TEST_LIFECYCLE_VLC_FILTER_H
#define TEST_LIFECYCLE_VLC_FILTER_H

#include <vlc_common.h>
#include <vlc_picture.h>

#include <stdbool.h>

typedef struct
{
    video_format_t video;
} es_format_t;

typedef struct filter_t filter_t;
typedef struct filter_sys_t filter_sys_t;

typedef struct
{
    struct {
        picture_t *(*buffer_new)(filter_t *);
    } video;
} filter_owner_t;

struct filter_t
{
    vlc_object_t obj;
    filter_sys_t *p_sys;
    es_format_t fmt_in;
    es_format_t fmt_out;
    bool b_allow_fmt_out_change;
    picture_t *(*pf_video_filter)(filter_t *, picture_t *);
    filter_owner_t owner;
};

static inline picture_t *filter_NewPicture(filter_t *filter)
{
    return filter->owner.video.buffer_new(filter);
}

#endif
