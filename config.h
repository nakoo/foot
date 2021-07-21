#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <tllist.h>

#include "terminal.h"
#include "user-notification.h"
#include "wayland.h"

#ifdef HAVE_TERMINFO
    #define DEFAULT_TERM "foot"
#else
    #define DEFAULT_TERM "xterm-256color"
#endif

#define DEFINE_LIST(type) \
    type##_list {         \
        size_t count;     \
        type *arr;        \
    }

enum conf_size_type {CONF_SIZE_PX, CONF_SIZE_CELLS};

struct config_font {
    char *pattern;
    double pt_size;
    int px_size;
};
DEFINE_LIST(struct config_font);

struct config_key_modifiers {
    bool shift;
    bool alt;
    bool ctrl;
    bool meta;
};

struct argv {
    char **args;
};

struct config_binding_pipe {
    struct argv argv;
    bool master_copy;
};

struct config_key_binding {
    int action;  /* One of the varios bind_action_* enums from wayland.h */
    struct config_key_modifiers modifiers;
    xkb_keysym_t sym;
    struct config_binding_pipe pipe;
};
DEFINE_LIST(struct config_key_binding);

struct config_mouse_binding {
    enum bind_action_normal action;
    struct config_key_modifiers modifiers;
    int button;
    int count;
    struct config_binding_pipe pipe;
};
DEFINE_LIST(struct config_mouse_binding);

typedef tll(char *) config_override_t;

struct config_spawn_template {
    struct argv argv;
};

struct config {
    char *term;
    char *shell;
    char *title;
    char *app_id;
    wchar_t *word_delimiters;
    bool login_shell;
    bool no_wait;
    bool locked_title;

    struct {
        enum conf_size_type type;
        unsigned width;
        unsigned height;
    } size;

    unsigned pad_x;
    unsigned pad_y;
    bool center;
    uint16_t resize_delay_ms;

    struct {
        bool enabled;
        bool palette_based;
    } bold_in_bright;

    enum { STARTUP_WINDOWED, STARTUP_MAXIMIZED, STARTUP_FULLSCREEN } startup_mode;

    enum {DPI_AWARE_AUTO, DPI_AWARE_YES, DPI_AWARE_NO} dpi_aware;
    struct config_font_list fonts[4];

    /* Custom font metrics (-1 = use real font metrics) */
    struct pt_or_px line_height;
    struct pt_or_px letter_spacing;

    /* Adjusted letter x/y offsets */
    struct pt_or_px horizontal_letter_offset;
    struct pt_or_px vertical_letter_offset;

    bool use_custom_underline_offset;
    struct pt_or_px underline_offset;

    bool box_drawings_uses_font_glyphs;
    bool can_shape_grapheme;

    bool subpixel_with_alpha;

    struct {
        bool urgent;
        bool notify;
        struct config_spawn_template command;
        bool command_focused;
    } bell;

    struct {
        int lines;

        struct {
            enum {
                SCROLLBACK_INDICATOR_POSITION_NONE,
                SCROLLBACK_INDICATOR_POSITION_FIXED,
                SCROLLBACK_INDICATOR_POSITION_RELATIVE
            } position;

            enum {
                SCROLLBACK_INDICATOR_FORMAT_PERCENTAGE,
                SCROLLBACK_INDICATOR_FORMAT_LINENO,
                SCROLLBACK_INDICATOR_FORMAT_TEXT,
            } format;

            wchar_t *text;
        } indicator;
        double multiplier;
    } scrollback;

    struct {
        wchar_t *label_letters;
        struct config_spawn_template launch;
        enum {
            OSC8_UNDERLINE_URL_MODE,
            OSC8_UNDERLINE_ALWAYS,
        } osc8_underline;

        wchar_t **protocols;
        size_t prot_count;
        size_t max_prot_len;
    } url;

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t table[256];
        uint16_t alpha;
        uint32_t selection_fg;
        uint32_t selection_bg;
        uint32_t url;

        struct {
            uint32_t fg;
            uint32_t bg;
        } jump_label;

        struct {
            bool selection:1;
            bool jump_label:1;
            bool url:1;
        } use_custom;
    } colors;

    struct {
        enum cursor_style style;
        bool blink;
        struct {
            uint32_t text;
            uint32_t cursor;
        } color;
        struct pt_or_px beam_thickness;
        struct pt_or_px underline_thickness;
    } cursor;

    struct {
        bool hide_when_typing;
        bool alternate_scroll_mode;
    } mouse;

    struct {
        /* Bindings for "normal" mode */
        struct config_key_binding_list key;
        struct config_mouse_binding_list mouse;

        /*
         * Special modes
         */

        /* While searching (not - action to *start* a search is in the
         * 'key' bindings above */
        struct config_key_binding_list search;

        /* While showing URL jump labels */
        struct config_key_binding_list url;
    } bindings;

    struct {
        enum { CONF_CSD_PREFER_NONE, CONF_CSD_PREFER_SERVER, CONF_CSD_PREFER_CLIENT } preferred;

        int title_height;
        int border_width;
        int button_width;

        struct {
            bool title_set:1;
            bool buttons_set:1;
            bool minimize_set:1;
            bool maximize_set:1;
            bool close_set:1;
            uint32_t title;
            uint32_t buttons;
            uint32_t minimize;
            uint32_t maximize;
            uint32_t close;
        } color;
    } csd;

    size_t render_worker_count;
    char *server_socket_path;
    bool presentation_timings;
    bool hold_at_exit;
    enum {
        SELECTION_TARGET_NONE,
        SELECTION_TARGET_PRIMARY,
        SELECTION_TARGET_CLIPBOARD,
        SELECTION_TARGET_BOTH
    } selection_target;

    struct config_spawn_template notify;

    struct {
        enum fcft_scaling_filter fcft_filter;
        bool overflowing_glyphs;
        bool grapheme_shaping;
        enum {GRAPHEME_WIDTH_WCSWIDTH, GRAPHEME_WIDTH_DOUBLE} grapheme_width_method;
        bool render_timer_osd;
        bool render_timer_log;
        bool damage_whole_window;
        uint64_t delayed_render_lower_ns;
        uint64_t delayed_render_upper_ns;
        off_t max_shm_pool_size;
        float box_drawing_base_thickness;
        bool box_drawing_solid_shades;
    } tweak;

    user_notifications_t notifications;
};

bool config_override_apply(struct config *conf, config_override_t *overrides,
    bool errors_are_fatal);
bool config_load(
    struct config *conf, const char *path,
    user_notifications_t *initial_user_notifications,
    config_override_t *overrides, bool errors_are_fatal);
void config_free(struct config conf);
struct config *config_clone(const struct config *old);

bool config_font_parse(const char *pattern, struct config_font *font);
void config_font_list_destroy(struct config_font_list *font_list);
