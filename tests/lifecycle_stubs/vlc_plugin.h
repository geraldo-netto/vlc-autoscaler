#ifndef TEST_LIFECYCLE_VLC_PLUGIN_H
#define TEST_LIFECYCLE_VLC_PLUGIN_H

#define CAT_VIDEO 0
#define SUBCAT_VIDEO_VFILTER 0

#define vlc_module_begin() \
    static const char *const lifecycle_module_descriptor[] __attribute__((unused)) = {
#define vlc_module_end() NULL };
#define set_shortname(...) "",
#define set_description(...) "",
#define set_help(...) "",
#define set_capability(...) "",
#define set_category(...) "",
#define set_subcategory(...) "",
#define set_callbacks(...) "",
#define add_shortcut(...) "",
#define add_integer_with_range(...) "",

#endif
