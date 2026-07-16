#ifndef TEST_LIFECYCLE_VLC_PLUGIN_H
#define TEST_LIFECYCLE_VLC_PLUGIN_H

#define CAT_VIDEO 0
#define SUBCAT_VIDEO_VFILTER 0

#define vlc_module_begin() \
    static void __attribute__((unused)) lifecycle_module_descriptor(void) {
#define vlc_module_end() }
#define set_shortname(...) do { } while (0);
#define set_description(...) do { } while (0);
#define set_help(...) do { } while (0);
#define set_capability(...) do { } while (0);
#define set_category(...) do { } while (0);
#define set_subcategory(...) do { } while (0);
#define set_callbacks(...) do { } while (0);
#define add_shortcut(...) do { } while (0);
#define add_integer_with_range(...) do { } while (0);

#endif
