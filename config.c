#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <fontconfig/fontconfig.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "input.h"
#include "macros.h"
#include "tokenize.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

static const uint32_t default_foreground = 0xdcdccc;
static const uint32_t default_background = 0x111111;

static const uint32_t default_regular[] = {
    0x222222,
    0xcc9393,
    0x7f9f7f,
    0xd0bf8f,
    0x6ca0a3,
    0xdc8cc3,
    0x93e0e3,
    0xdcdccc,
};

static const uint32_t default_bright[] = {
    0x666666,
    0xdca3a3,
    0xbfebbf,
    0xf0dfaf,
    0x8cd0d3,
    0xfcace3,
    0xb3ffff,
    0xffffff,
};

static const char *const binding_action_map[] = {
    [BIND_ACTION_NONE] = NULL,
    [BIND_ACTION_SCROLLBACK_UP_PAGE] = "scrollback-up-page",
    [BIND_ACTION_SCROLLBACK_UP_HALF_PAGE] = "scrollback-up-half-page",
    [BIND_ACTION_SCROLLBACK_UP_LINE] = "scrollback-up-line",
    [BIND_ACTION_SCROLLBACK_DOWN_PAGE] = "scrollback-down-page",
    [BIND_ACTION_SCROLLBACK_DOWN_HALF_PAGE] = "scrollback-down-half-page",
    [BIND_ACTION_SCROLLBACK_DOWN_LINE] = "scrollback-down-line",
    [BIND_ACTION_CLIPBOARD_COPY] = "clipboard-copy",
    [BIND_ACTION_CLIPBOARD_PASTE] = "clipboard-paste",
    [BIND_ACTION_PRIMARY_PASTE] = "primary-paste",
    [BIND_ACTION_SEARCH_START] = "search-start",
    [BIND_ACTION_FONT_SIZE_UP] = "font-increase",
    [BIND_ACTION_FONT_SIZE_DOWN] = "font-decrease",
    [BIND_ACTION_FONT_SIZE_RESET] = "font-reset",
    [BIND_ACTION_SPAWN_TERMINAL] = "spawn-terminal",
    [BIND_ACTION_MINIMIZE] = "minimize",
    [BIND_ACTION_MAXIMIZE] = "maximize",
    [BIND_ACTION_FULLSCREEN] = "fullscreen",
    [BIND_ACTION_PIPE_SCROLLBACK] = "pipe-scrollback",
    [BIND_ACTION_PIPE_VIEW] = "pipe-visible",
    [BIND_ACTION_PIPE_SELECTED] = "pipe-selected",
    [BIND_ACTION_SHOW_URLS_COPY] = "show-urls-copy",
    [BIND_ACTION_SHOW_URLS_LAUNCH] = "show-urls-launch",

    /* Mouse-specific actions */
    [BIND_ACTION_SELECT_BEGIN] = "select-begin",
    [BIND_ACTION_SELECT_BEGIN_BLOCK] = "select-begin-block",
    [BIND_ACTION_SELECT_EXTEND] = "select-extend",
    [BIND_ACTION_SELECT_EXTEND_CHAR_WISE] = "select-extend-character-wise",
    [BIND_ACTION_SELECT_WORD] = "select-word",
    [BIND_ACTION_SELECT_WORD_WS] = "select-word-whitespace",
    [BIND_ACTION_SELECT_ROW] = "select-row",
};

static_assert(ALEN(binding_action_map) == BIND_ACTION_COUNT,
              "binding action map size mismatch");

static void NOINLINE PRINTF(5)
log_and_notify(struct config *conf, enum log_class log_class,
               const char *file, int lineno, const char *fmt, ...)
{
    enum user_notification_kind kind;

    switch (log_class) {
    case LOG_CLASS_WARNING: kind = USER_NOTIFICATION_WARNING; break;
    case LOG_CLASS_ERROR:   kind = USER_NOTIFICATION_ERROR; break;

    case LOG_CLASS_INFO:
    case LOG_CLASS_DEBUG:
    case LOG_CLASS_NONE:
    default:
        BUG("unsupported log class: %d", (int)log_class);
        return;
    }

    va_list va1, va2;
    va_start(va1, fmt);
    va_copy(va2, va1);

    log_msg_va(log_class, LOG_MODULE, file, lineno, fmt, va1);

    char *text = xvasprintf(fmt, va2);
    tll_push_back(
        conf->notifications,
        ((struct user_notification){.kind = kind, .text = text}));

    va_end(va2);
    va_end(va1);
}

static void NOINLINE PRINTF(5)
log_errno_and_notify(struct config *conf, enum log_class log_class,
                     const char *file, int lineno, const char *fmt, ...)
{
    int errno_copy = errno;

    va_list va1, va2, va3;
    va_start(va1, fmt);
    va_copy(va2, va1);
    va_copy(va3, va2);

    log_errno_provided_va(
        log_class, LOG_MODULE, file, lineno, errno_copy, fmt, va1);

    int len = vsnprintf(NULL, 0, fmt, va2);
    int errno_len = snprintf(NULL, 0, ": %s", strerror(errno_copy));

    char *text = xmalloc(len + errno_len + 1);
    vsnprintf(text, len + errno_len + 1, fmt, va3);
    snprintf(&text[len], errno_len + 1, ": %s", strerror(errno_copy));

    tll_push_back(
        conf->notifications,
        ((struct user_notification){
            .kind = USER_NOTIFICATION_ERROR, .text = text}));

    va_end(va3);
    va_end(va2);
    va_end(va1);
}

#define LOG_AND_NOTIFY_ERR(...) \
    log_and_notify(conf, LOG_CLASS_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_AND_NOTIFY_WARN(...) \
    log_and_notify(conf, LOG_CLASS_WARNING, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_AND_NOTIFY_ERRNO(...) \
    log_errno_and_notify(conf, LOG_CLASS_ERROR, __FILE__, __LINE__, __VA_ARGS__)

static char *
get_shell(void)
{
    const char *shell = getenv("SHELL");

    if (shell == NULL) {
        struct passwd *passwd = getpwuid(getuid());
        if (passwd == NULL) {
            LOG_ERRNO("failed to lookup user: falling back to 'sh'");
            shell = "sh";
        } else
            shell = passwd->pw_shell;
    }

    LOG_DBG("user's shell: %s", shell);
    return xstrdup(shell);
}

struct config_file {
    char *path;       /* Full, absolute, path */
    int fd;           /* FD of file, O_RDONLY */
};

struct path_component {
    const char *component;
    int fd;
};
typedef tll(struct path_component) path_components_t;

static void NOINLINE
path_component_add(path_components_t *components, const char *comp, int fd)
{
    xassert(comp != NULL);
    xassert(fd >= 0);

    struct path_component pc = {.component = comp, .fd = fd};
    tll_push_back(*components, pc);
}

static void NOINLINE
path_component_destroy(struct path_component *component)
{
    xassert(component->fd >= 0);
    close(component->fd);
}

static void NOINLINE
path_components_destroy(path_components_t *components)
{
    tll_foreach(*components, it) {
        path_component_destroy(&it->item);
        tll_remove(*components, it);
    }
}

static struct config_file
path_components_to_config_file(const path_components_t *components)
{
    if (tll_length(*components) == 0)
        goto err;

    size_t len = 0;
    tll_foreach(*components, it)
        len += strlen(it->item.component) + 1;

    char *path = malloc(len);
    if (path == NULL)
        goto err;

    size_t idx = 0;
    tll_foreach(*components, it) {
        strcpy(&path[idx], it->item.component);
        idx += strlen(it->item.component);
        path[idx++] = '/';
    }
    path[idx - 1] = '\0';  /* Strip last ’/’ */

    int fd_copy = dup(tll_back(*components).fd);
    if (fd_copy < 0) {
        free(path);
        goto err;
    }

    return (struct config_file){.path = path, .fd = fd_copy};

err:
    return (struct config_file){.path = NULL, .fd = -1};
}

static const char *
get_user_home_dir(void)
{
    const struct passwd *passwd = getpwuid(getuid());
    if (passwd == NULL)
        return NULL;
    return passwd->pw_dir;
}

static bool
try_open_file(path_components_t *components, const char *name)
{
    int parent_fd = tll_back(*components).fd;

    struct stat st;
    if (fstatat(parent_fd, name, &st, 0) == 0 && S_ISREG(st.st_mode)) {
        int fd = openat(parent_fd, name, O_RDONLY);
        if (fd >= 0) {
            path_component_add(components, name, fd);
            return true;
        }
    }

    return false;
}

static struct config_file
open_config(void)
{
    struct config_file ret = {.path = NULL, .fd = -1};

    path_components_t components = tll_init();

    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char *user_home_dir = get_user_home_dir();
    char *xdg_config_dirs_copy = NULL;

    /* Use XDG_CONFIG_HOME, or ~/.config */
    if (xdg_config_home != NULL) {
        int fd = open(xdg_config_home, O_RDONLY);
        if (fd >= 0)
            path_component_add(&components, xdg_config_home, fd);
    } else if (user_home_dir != NULL) {
        int home_fd = open(user_home_dir, O_RDONLY);
        if (home_fd >= 0) {
            int config_fd = openat(home_fd, ".config", O_RDONLY);
            if (config_fd >= 0) {
                path_component_add(&components, user_home_dir, home_fd);
                path_component_add(&components, ".config", config_fd);
            } else
                close(home_fd);
        }
    }

    /* First look for foot/foot.ini */
    if (tll_length(components) > 0) {
        int foot_fd = openat(tll_back(components).fd, "foot", O_RDONLY);
        if (foot_fd >= 0) {
            path_component_add(&components, "foot", foot_fd);

            if (try_open_file(&components, "foot.ini"))
                goto done;

            struct path_component pc = tll_pop_back(components);
            path_component_destroy(&pc);
        }
    }

    /* Finally, try foot/foot.ini in all XDG_CONFIG_DIRS */
    const char *xdg_config_dirs = getenv("XDG_CONFIG_DIRS");
    xdg_config_dirs_copy = xdg_config_dirs != NULL
        ? strdup(xdg_config_dirs) : NULL;

    if (xdg_config_dirs_copy != NULL) {
        for (char *save = NULL,
                 *xdg_dir = strtok_r(xdg_config_dirs_copy, ":", &save);
             xdg_dir != NULL;
             xdg_dir = strtok_r(NULL, ":", &save))
        {
            path_components_destroy(&components);

            int xdg_fd = open(xdg_dir, O_RDONLY);
            if (xdg_fd < 0)
                continue;

            int foot_fd = openat(xdg_fd, "foot", O_RDONLY);
            if (foot_fd < 0) {
                close(xdg_fd);
                continue;
            }

            xassert(tll_length(components) == 0);
            path_component_add(&components, xdg_dir, xdg_fd);
            path_component_add(&components, "foot", foot_fd);

            if (try_open_file(&components, "foot.ini"))
                goto done;
        }
    }

out:
    path_components_destroy(&components);
    free(xdg_config_dirs_copy);
    return ret;

done:
    xassert(tll_length(components) > 0);
    ret = path_components_to_config_file(&components);
    goto out;
}

static bool
str_has_prefix(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static bool NOINLINE
str_to_bool(const char *s)
{
    return strcasecmp(s, "on") == 0 ||
        strcasecmp(s, "true") == 0 ||
        strcasecmp(s, "yes") == 0 ||
        strtoul(s, NULL, 0) > 0;
}

static bool NOINLINE
str_to_ulong(const char *s, int base, unsigned long *res)
{
    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtoul(s, &end, base);
    return errno == 0 && *end == '\0';
}

static bool NOINLINE
str_to_double(const char *s, double *res)
{
    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtod(s, &end);
    return errno == 0 && *end == '\0';
}

static bool NOINLINE
str_to_wchars(const char *s, wchar_t **res, struct config *conf,
              const char  *path, int lineno,
              const char *section, const char *key)
{
    *res = NULL;

    size_t chars = mbstowcs(NULL, s, 0);
    if (chars == (size_t)-1) {
        LOG_AND_NOTIFY_ERR("%s:%d: [%s]: %s: invalid string: %s",
                           path, lineno, section, key, s);
        return false;
    }

    *res = xmalloc((chars + 1) * sizeof(wchar_t));
    mbstowcs(*res, s, chars + 1);
    return true;
}

static bool NOINLINE
str_to_color(const char *s, uint32_t *color, bool allow_alpha,
             struct config *conf, const char *path, int lineno,
             const char *section, const char *key)
{
    unsigned long value;
    if (!str_to_ulong(s, 16, &value)) {
        LOG_AND_NOTIFY_ERR(
            "%s:%d: [%s]: %s: invalid color: %s", path, lineno, section, key, s);
        return false;
    }

    if (!allow_alpha && (value & 0xff000000) != 0) {
        LOG_AND_NOTIFY_ERR(
            "%s:%d: [%s]: %s: color value must not have an alpha component: %s",
            path, lineno, section, key, s);
        return false;
    }

    *color = value;
    return true;
}

static bool NOINLINE
str_to_two_colors(const char *s, uint32_t *first, uint32_t *second,
                  bool allow_alpha, struct config *conf, const char *path,
                  int lineno, const char *section, const char *key)
{
    /* TODO: do this without strdup() */
    char *value_copy = xstrdup(s);
    const char *first_as_str = strtok(value_copy, " ");
    const char *second_as_str = strtok(NULL, " ");

    if (first_as_str == NULL || second_as_str == NULL ||
        !str_to_color(first_as_str, first, allow_alpha, conf, path, lineno, section, key) ||
        !str_to_color(second_as_str, second, allow_alpha, conf, path, lineno, section, key))
    {
        free(value_copy);
        return false;
    }

    free(value_copy);
    return true;
}

static bool NOINLINE
str_to_pt_or_px(const char *s, struct pt_or_px *res, struct config *conf,
                const char *path, int lineno, const char *section, const char *key)
{
    size_t len = s != NULL ? strlen(s) : 0;
    if (len >= 2 && s[len - 2] == 'p' && s[len - 1] == 'x') {
        errno = 0;
        char *end = NULL;

        long value = strtol(s, &end, 10);
        if (!(errno == 0 && end == s + len - 2)) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [%s]: %s: "
                "expected an integer directly followed by 'px', got '%s'",
                path, lineno, section, key, s);
            return false;
        }
        res->pt = 0;
        res->px = value;
    } else {
        double value;
        if (!str_to_double(s, &value)) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [%s]: %s: expected a decimal value, got '%s'",
                path, lineno, section, key, s);
            return false;
        }
        res->pt = value;
        res->px = 0;
    }

    return true;
}

static void NOINLINE
free_argv(struct argv *argv)
{
    if (argv->args == NULL)
        return;
    for (char **a = argv->args; *a != NULL; a++)
        free(*a);
    free(argv->args);
    argv->args = NULL;
}

static void NOINLINE
clone_argv(struct argv *dst, const struct argv *src)
{
    if (src->args == NULL) {
        dst->args = NULL;
        return;
    }

    size_t count = 0;
    for (char **args = src->args; *args != NULL; args++)
        count++;

    dst->args = xmalloc((count + 1) * sizeof(dst->args[0]));
    for (char **args_src = src->args, **args_dst = dst->args;
         *args_src != NULL; args_src++,
             args_dst++)
    {
        *args_dst = xstrdup(*args_src);
    }
    dst->args[count] = NULL;
}

static void
spawn_template_free(struct config_spawn_template *template)
{
    free_argv(&template->argv);
}

static void
spawn_template_clone(struct config_spawn_template *dst,
                     const struct config_spawn_template *src)
{
    clone_argv(&dst->argv, &src->argv);
}

static bool NOINLINE
str_to_spawn_template(struct config *conf,
                      const char *s, struct config_spawn_template *template,
                      const char *path, int lineno, const char *section,
                      const char *key)
{
    spawn_template_free(template);

    char **argv = NULL;

    if (!tokenize_cmdline(s, &argv)) {
        LOG_AND_NOTIFY_ERR(
            "%s:%d: [%s]: %s: syntax error in command line",
            path, lineno, section, key);
        return false;
    }

    template->argv.args = argv;
    return true;
}

static void
deprecated_url_option(struct config *conf,
                      const char *old_name, const char *new_name,
                      const char *path, unsigned lineno)
{
    LOG_WARN(
        "deprecated: %s:%d: [default]: %s: use '%s' in section '[url]' instead",
        path, lineno, old_name, new_name);

    const char fmt[] =
        "%s:%d: \033[1m%s\033[22m, use \033[1m%s\033[22m in the \033[1m[url]\033[22m section instead";
    char *text = xasprintf(fmt, path, lineno, old_name, new_name);

    struct user_notification deprecation = {
        .kind = USER_NOTIFICATION_DEPRECATED,
        .text = text,
    };
    tll_push_back(conf->notifications, deprecation);
}

static bool parse_config_file(
    FILE *f, struct config *conf, const char *path, bool errors_are_fatal);

static bool
parse_section_main(const char *key, const char *value, struct config *conf,
                   const char *path, unsigned lineno, bool errors_are_fatal)
{
    if (strcmp(key, "include") == 0) {
        const char *include_path = value;

        if (include_path[0] != '/') {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: %s: not an absolute path",
                path, lineno, include_path);
            return false;
        }

        FILE *include = fopen(include_path, "r");

        if (include == NULL) {
            LOG_AND_NOTIFY_ERRNO(
                "%s:%d: [default]: %s: failed to open",
                path, lineno, include_path);
            return false;
        }

        bool ret = parse_config_file(
            include, conf, include_path, errors_are_fatal);
        fclose(include);

        LOG_INFO("imported sub-configuration from %s", include_path);
        return ret;
    }

    else if (strcmp(key, "term") == 0) {
        free(conf->term);
        conf->term = xstrdup(value);
    }

    else if (strcmp(key, "shell") == 0) {
        free(conf->shell);
        conf->shell = xstrdup(value);
    }

    else if (strcmp(key, "login-shell") == 0) {
        conf->login_shell = str_to_bool(value);
    }

    else if (strcmp(key, "title") == 0) {
        free(conf->title);
        conf->title = xstrdup(value);
    }

    else if (strcmp(key, "locked-title") == 0)
        conf->locked_title = str_to_bool(value);

    else if (strcmp(key, "app-id") == 0) {
        free(conf->app_id);
        conf->app_id = xstrdup(value);
    }

    else if (strcmp(key, "initial-window-size-pixels") == 0) {
        unsigned width, height;
        if (sscanf(value, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: initial-window-size-pixels: "
                "expected WIDTHxHEIGHT, where both are positive integers, "
                "got '%s'", path, lineno, value);
            return false;
        }

        conf->size.type = CONF_SIZE_PX;
        conf->size.width = width;
        conf->size.height = height;
    }

    else if (strcmp(key, "initial-window-size-chars") == 0) {
        unsigned width, height;
        if (sscanf(value, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: initial-window-size-chars: "
                "expected WIDTHxHEIGHT, where both are positive integers, "
                "got '%s'", path, lineno, value);
            return false;
        }

        conf->size.type = CONF_SIZE_CELLS;
        conf->size.width = width;
        conf->size.height = height;
    }

    else if (strcmp(key, "pad") == 0) {
        unsigned x, y;
        char mode[16] = {0};

        int ret = sscanf(value, "%ux%u %15s", &x, &y, mode);
        bool center = strcasecmp(mode, "center") == 0;
        bool invalid_mode = !center && mode[0] != '\0';

        if ((ret != 2 && ret != 3) || invalid_mode) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: pad: expected PAD_XxPAD_Y [center], "
                "where both are positive integers, got '%s'",
                path, lineno, value);
            return false;
        }

        conf->pad_x = x;
        conf->pad_y = y;
        conf->center = center;
    }

    else if (strcmp(key, "resize-delay-ms") == 0) {
        unsigned long ms;
        if (!str_to_ulong(value, 10, &ms)) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: resize-delay-ms: "
                "expected an integer, got '%s'",
                path, lineno, value);
            return false;
        }

        conf->resize_delay_ms = ms;
    }

    else if (strcmp(key, "bold-text-in-bright") == 0) {
        if (strcmp(value, "palette-based") == 0) {
            conf->bold_in_bright.enabled = true;
            conf->bold_in_bright.palette_based = true;
        } else {
            conf->bold_in_bright.enabled = str_to_bool(value);
            conf->bold_in_bright.palette_based = false;
        }
    }

    else if (strcmp(key, "bell") == 0) {
        LOG_WARN(
            "deprecated: %s:%d: [default]: bell: "
            "set actions in section '[bell]' instead", path, lineno);

        const char fmt[] =
            "%s:%d: \033[1mbell\033[22m, use \033[1murgent\033[22m in "
            "the \033[1m[bell]\033[22m section instead";

        struct user_notification deprecation = {
            .kind = USER_NOTIFICATION_DEPRECATED,
            .text = xasprintf(fmt, path, lineno),
        };
        tll_push_back(conf->notifications, deprecation);

        if (strcmp(value, "set-urgency") == 0) {
            memset(&conf->bell, 0, sizeof(conf->bell));
            conf->bell.urgent = true;
        }
        else if (strcmp(value, "notify") == 0) {
            memset(&conf->bell, 0, sizeof(conf->bell));
            conf->bell.notify = true;
        }
        else if (strcmp(value, "none") == 0) {
            memset(&conf->bell, 0, sizeof(conf->bell));
        }
        else {
            LOG_AND_NOTIFY_ERR(
                "%s%d: [default]: bell: "
                "expected either 'set-urgency', 'notify' or 'none'",
                path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "initial-window-mode") == 0) {
        if (strcmp(value, "windowed") == 0)
            conf->startup_mode = STARTUP_WINDOWED;
        else if (strcmp(value, "maximized") == 0)
            conf->startup_mode = STARTUP_MAXIMIZED;
        else if (strcmp(value, "fullscreen") == 0)
            conf->startup_mode = STARTUP_FULLSCREEN;
        else {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: initial-window-mode: expected either "
                "'windowed', 'maximized' or 'fullscreen'",
                path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "font") == 0 ||
             strcmp(key, "font-bold") == 0 ||
             strcmp(key, "font-italic") == 0 ||
             strcmp(key, "font-bold-italic") == 0)

    {
        size_t idx =
            strcmp(key, "font") == 0 ? 0 :
            strcmp(key, "font-bold") == 0 ? 1 :
            strcmp(key, "font-italic") == 0 ? 2 : 3;

        config_font_list_destroy(&conf->fonts[idx]);

        size_t count = 0;
        size_t size = 0;
        struct config_font *fonts = NULL;

        char *copy = xstrdup(value);
        for (const char *font = strtok(copy, ","); font != NULL; font = strtok(NULL, ",")) {
            /* Trim spaces, strictly speaking not necessary, but looks nice :) */
            while (*font != '\0' && isspace(*font))
                font++;

            if (font[0] == '\0')
                continue;

            struct config_font font_data;
            if (!config_font_parse(font, &font_data)) {
                LOG_ERR("%s:%d: [default]: %s: invalid font specification",
                        path, lineno, key);
                free(copy);
                return false;
            }

            if (count + 1 > size) {
                size += 4;
                fonts = xrealloc(fonts, size * sizeof(fonts[0]));
            }

            xassert(count + 1 <= size);
            fonts[count++] = font_data;
        }

        conf->fonts[idx].count = count;
        conf->fonts[idx].arr = fonts;
        free(copy);
    }

    else if (strcmp(key, "line-height") == 0) {
        if (!str_to_pt_or_px(value, &conf->line_height,
                             conf, path, lineno, "default", "line-height"))
            return false;
    }

    else if (strcmp(key, "letter-spacing") == 0) {
        if (!str_to_pt_or_px(value, &conf->letter_spacing,
                             conf, path, lineno, "default", "letter-spacing"))
            return false;
    }

    else if (strcmp(key, "horizontal-letter-offset") == 0) {
        if (!str_to_pt_or_px(
                value, &conf->horizontal_letter_offset,
                conf, path, lineno, "default", "horizontal-letter-offset"))
            return false;
    }

    else if (strcmp(key, "vertical-letter-offset") == 0) {
        if (!str_to_pt_or_px(
                value, &conf->vertical_letter_offset,
                conf, path, lineno, "default", "vertical-letter-offset"))
            return false;
    }

    else if (strcmp(key, "underline-offset") == 0) {
        if (!str_to_pt_or_px(
                value, &conf->underline_offset,
                conf, path, lineno, "default", "underline-offset"))
            return false;
        conf->use_custom_underline_offset = true;
    }

    else if (strcmp(key, "dpi-aware") == 0) {
        if (strcmp(value, "auto") == 0)
            conf->dpi_aware = DPI_AWARE_AUTO;
        else
            conf->dpi_aware = str_to_bool(value) ? DPI_AWARE_YES : DPI_AWARE_NO;
    }

    else if (strcmp(key, "workers") == 0) {
        unsigned long count;
        if (!str_to_ulong(value, 10, &count)) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: workers: expected an integer, got '%s'",
                path, lineno, value);
            return false;
        }
        conf->render_worker_count = count;
    }

    else if (strcmp(key, "word-delimiters") == 0) {
        wchar_t *word_delimiters;
        if (!str_to_wchars(value, &word_delimiters, conf, path, lineno,
                           "default", "word-delimiters"))
        {
            return false;
        }
        free(conf->word_delimiters);
        conf->word_delimiters = word_delimiters;
    }

    else if (strcmp(key, "jump-label-letters") == 0) {
        deprecated_url_option(
            conf, "jump-label-letters", "label-letters", path, lineno);

        wchar_t *letters;
        if (!str_to_wchars(value, &letters, conf, path, lineno,
                           "default", "label-letters"))
        {
            return false;
        }
        free(conf->url.label_letters);
        conf->url.label_letters = letters;
    }

    else if (strcmp(key, "notify") == 0) {
        if (!str_to_spawn_template(conf, value, &conf->notify, path, lineno,
                                   "default", "notify"))
        {
            return false;
        }
    }

    else if (strcmp(key, "url-launch") == 0) {
        deprecated_url_option(
            conf, "url-launch", "launch", path, lineno);

        if (!str_to_spawn_template(conf, value, &conf->url.launch, path, lineno,
                                   "default", "url-launch"))
        {
            return false;
        }
    }

    else if (strcmp(key, "selection-target") == 0) {
        static const char values[][12] = {
            [SELECTION_TARGET_NONE] = "none",
            [SELECTION_TARGET_PRIMARY] = "primary",
            [SELECTION_TARGET_CLIPBOARD] = "clipboard",
            [SELECTION_TARGET_BOTH] = "both",
        };

        for (size_t i = 0; i < ALEN(values); i++) {
            if (strcasecmp(value, values[i]) == 0) {
                conf->selection_target = i;
                return true;
            }
        }

        LOG_AND_NOTIFY_ERR(
            "%s:%d: [default]: %s: invalid 'selection-target'; "
            "must be one of 'none', 'primary', 'clipboard' or 'both",
            path, lineno, value);
        return false;
    }

    else if (strcmp(key, "osc8-underline") == 0) {
        deprecated_url_option(
            conf, "osc8-underline", "osc8-underline", path, lineno);

        if (strcmp(value, "url-mode") == 0)
            conf->url.osc8_underline = OSC8_UNDERLINE_URL_MODE;
        else if (strcmp(value, "always") == 0)
            conf->url.osc8_underline = OSC8_UNDERLINE_ALWAYS;
        else {
            LOG_AND_NOTIFY_ERR(
                "%s:%u: [default]: %s: invalid 'osc8-underline'; "
                "must be one of 'url-mode', or 'always'", path, lineno, value);
            return false;
        }
    }

    else if (strcmp(key, "box-drawings-uses-font-glyphs") == 0)
        conf->box_drawings_uses_font_glyphs = str_to_bool(value);

    else if (strcmp(key, "subpixel-with-alpha") == 0)
        conf->subpixel_with_alpha = str_to_bool(value);

    else {
        LOG_AND_NOTIFY_ERR("%s:%u: [default]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_bell(const char *key, const char *value, struct config *conf,
                   const char *path, unsigned lineno, bool errors_are_fatal)
{
    if (strcmp(key, "urgent") == 0)
        conf->bell.urgent = str_to_bool(value);
    else if (strcmp(key, "notify") == 0)
        conf->bell.notify = str_to_bool(value);
    else if (strcmp(key, "command") == 0) {
        if (!str_to_spawn_template(conf, value, &conf->bell.command, path, lineno, "bell", key))
            return false;
    }
    else if (strcmp(key, "command-focused") == 0)
        conf->bell.command_focused = str_to_bool(value);
    else {
        LOG_AND_NOTIFY_ERR("%s:%u: [bell]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_scrollback(const char *key, const char *value, struct config *conf,
                         const char *path, unsigned lineno, bool errors_are_fatal)
{
    if (strcmp(key, "lines") == 0) {
        unsigned long lines;
        if (!str_to_ulong(value, 10, &lines)) {
            LOG_AND_NOTIFY_ERR("%s:%d: [scrollback]: lines: expected an integer, got '%s'", path, lineno, value);
            return false;
        }
        conf->scrollback.lines = lines;
    }

    else if (strcmp(key, "indicator-position") == 0) {
        if (strcmp(value, "none") == 0)
            conf->scrollback.indicator.position = SCROLLBACK_INDICATOR_POSITION_NONE;
        else if (strcmp(value, "fixed") == 0)
            conf->scrollback.indicator.position = SCROLLBACK_INDICATOR_POSITION_FIXED;
        else if (strcmp(value, "relative") == 0)
            conf->scrollback.indicator.position = SCROLLBACK_INDICATOR_POSITION_RELATIVE;
        else {
            LOG_AND_NOTIFY_ERR("%s:%d: [scrollback]: indicator-position must be one of "
                    "'none', 'fixed' or 'relative'",
                    path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "indicator-format") == 0) {
        if (strcmp(value, "percentage") == 0) {
            conf->scrollback.indicator.format
                = SCROLLBACK_INDICATOR_FORMAT_PERCENTAGE;
        } else if (strcmp(value, "line") == 0) {
            conf->scrollback.indicator.format
                = SCROLLBACK_INDICATOR_FORMAT_LINENO;
        } else {
            free(conf->scrollback.indicator.text);
            conf->scrollback.indicator.text = NULL;

            size_t len = mbstowcs(NULL, value, 0);
            if (len == (size_t)-1) {
                LOG_AND_NOTIFY_ERRNO(
                    "%s:%d: [scrollback]: indicator-format: "
                    "invalid value: %s", path, lineno, value);
                return false;
            }

            conf->scrollback.indicator.text = xcalloc(len + 1, sizeof(wchar_t));
            mbstowcs(conf->scrollback.indicator.text, value, len + 1);
        }
    }

    else if (strcmp(key, "multiplier") == 0) {
        double multiplier;
        if (!str_to_double(value, &multiplier)) {
            LOG_AND_NOTIFY_ERR("%s:%d: [scrollback]: multiplier: "
                               "invalid value: %s", path, lineno, value);
            return false;
        }

        conf->scrollback.multiplier = multiplier;
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%u: [scrollback]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_url(const char *key, const char *value, struct config *conf,
                  const char *path, unsigned lineno, bool errors_are_fatal)
{
    if (strcmp(key, "launch") == 0) {
        if (!str_to_spawn_template(conf, value, &conf->url.launch, path, lineno,
                                   "url", "launch"))
        {
            return false;
        }
    }

    else if (strcmp(key, "label-letters") == 0) {
        wchar_t *letters;
        if (!str_to_wchars(value, &letters, conf, path, lineno, "url", "letters"))
            return false;

        free(conf->url.label_letters);
        conf->url.label_letters = letters;
    }

    else if (strcmp(key, "osc8-underline") == 0) {
        if (strcmp(value, "url-mode") == 0)
            conf->url.osc8_underline = OSC8_UNDERLINE_URL_MODE;
        else if (strcmp(value, "always") == 0)
            conf->url.osc8_underline = OSC8_UNDERLINE_ALWAYS;
        else {
            LOG_AND_NOTIFY_ERR(
                "%s:%u: [url]: %s: invalid 'osc8-underline'; "
                "must be one of 'url-mode', or 'always'", path, lineno, value);
            return false;
        }
    }

    else if (strcmp(key, "protocols") == 0) {
        for (size_t i = 0; i < conf->url.prot_count; i++)
            free(conf->url.protocols[i]);
        free(conf->url.protocols);

        conf->url.max_prot_len = 0;
        conf->url.prot_count = 0;
        conf->url.protocols = NULL;

        char *copy = xstrdup(value);

        for (char *prot = strtok(copy, ",");
             prot != NULL;
             prot = strtok(NULL, ","))
        {

            /* Strip leading whitespace */
            while (isspace(*prot))
                prot++;

            /* Strip trailing whitespace */
            size_t len = strlen(prot);
            while (len > 0 && isspace(prot[len - 1]))
                prot[--len] = '\0';

            size_t chars = mbstowcs(NULL, prot, 0);
            if (chars == (size_t)-1) {
                LOG_AND_NOTIFY_ERRNO(
                    "%s:%u: [url]: protocols: invalid protocol name: %s",
                    path, lineno, prot);
                return false;
            }

            conf->url.prot_count++;
            conf->url.protocols = xrealloc(
                conf->url.protocols,
                conf->url.prot_count * sizeof(conf->url.protocols[0]));

            size_t idx = conf->url.prot_count - 1;
            conf->url.protocols[idx] = xmalloc((chars + 1 + 3) * sizeof(wchar_t));
            mbstowcs(conf->url.protocols[idx], prot, chars + 1);
            wcscpy(&conf->url.protocols[idx][chars], L"://");

            chars += 3;  /* Include the "://" */
            if (chars > conf->url.max_prot_len)
                conf->url.max_prot_len = chars;
        }

        free(copy);
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%d: [url]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_colors(const char *key, const char *value, struct config *conf,
                     const char *path, unsigned lineno, bool errors_are_fatal)
{
    size_t key_len = strlen(key);
    uint8_t last_digit = (unsigned char)key[key_len - 1] - '0';
    uint32_t *color = NULL;

    if (isdigit(key[0])) {
        unsigned long index;
        if (!str_to_ulong(key, 0, &index)) {
            LOG_AND_NOTIFY_ERR("%s:%d: [colors]: invalid numeric key", path, lineno);
            return false;
        }
        if (index >= ALEN(conf->colors.table)) {
            LOG_AND_NOTIFY_ERR("%s:%d: [colors]: numeric key out of range", path, lineno);
            return false;
        }
        color = &conf->colors.table[index];
    }

    else if (key_len == 8 && str_has_prefix(key, "regular") && last_digit < 8)
        color = &conf->colors.table[last_digit];

    else if (key_len == 7 && str_has_prefix(key, "bright") && last_digit < 8)
        color = &conf->colors.table[8 + last_digit];

    else if (strcmp(key, "foreground") == 0) color = &conf->colors.fg;
    else if (strcmp(key, "background") == 0) color = &conf->colors.bg;
    else if (strcmp(key, "selection-foreground") == 0) color = &conf->colors.selection_fg;
    else if (strcmp(key, "selection-background") == 0) color = &conf->colors.selection_bg;

    else if (strcmp(key, "jump-labels") == 0) {
        if (!str_to_two_colors(
                value, &conf->colors.jump_label.fg, &conf->colors.jump_label.bg,
                false, conf, path, lineno, "colors", "jump-labels"))
        {
            return false;
        }

        conf->colors.use_custom.jump_label = true;
        return true;
    }

    else if (strcmp(key, "urls") == 0) {
        if (!str_to_color(value, &conf->colors.url, false,
                          conf, path, lineno, "colors", "urls"))
        {
            return false;
        }

        conf->colors.use_custom.url = true;
        return true;
    }

    else if (strcmp(key, "alpha") == 0) {
        double alpha;
        if (!str_to_double(value, &alpha) || alpha < 0. || alpha > 1.) {
            LOG_AND_NOTIFY_ERR("%s:%d: [colors]: alpha: expected a value in the range 0.0-1.0",
                    path, lineno);
            return false;
        }

        conf->colors.alpha = alpha * 65535.;
        return true;
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%d: [colors]: %s: invalid key", path, lineno, key);
        return false;
    }

    uint32_t color_value;
    if (!str_to_color(value, &color_value, false, conf, path, lineno, "colors", key))
        return false;

    *color = color_value;
    return true;
}

static bool
parse_section_cursor(const char *key, const char *value, struct config *conf,
                     const char *path, unsigned lineno, bool errors_are_fatal)
{
    if (strcmp(key, "style") == 0) {
        if (strcmp(value, "block") == 0)
            conf->cursor.style = CURSOR_BLOCK;
        else if (strcmp(value, "beam") == 0 || strcmp(value, "bar") == 0)
            conf->cursor.style = CURSOR_BEAM;
        else if (strcmp(value, "underline") == 0)
            conf->cursor.style = CURSOR_UNDERLINE;

        else {
            LOG_AND_NOTIFY_ERR("%s:%d: style: one of block, beam or underline",
                               path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "blink") == 0)
        conf->cursor.blink = str_to_bool(value);

    else if (strcmp(key, "color") == 0) {
        if (!str_to_two_colors(
                value, &conf->cursor.color.text, &conf->cursor.color.cursor,
                false, conf, path, lineno, "cursor", "color"))
        {
            return false;
        }

        conf->cursor.color.text |= 1u << 31;
        conf->cursor.color.cursor |= 1u << 31;
    }

    else if (strcmp(key, "beam-thickness") == 0) {
        if (!str_to_pt_or_px(
                value, &conf->cursor.beam_thickness,
                conf, path, lineno, "cursor", "beam-thickness"))
            return false;
    }

    else if (strcmp(key, "underline-thickness") == 0) {
        if (!str_to_pt_or_px(
                value, &conf->cursor.underline_thickness,
                conf, path, lineno, "cursor", "underline-thickness"))
            return false;
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%d: [cursor]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_mouse(const char *key, const char *value, struct config *conf,
                    const char *path, unsigned lineno, bool errors_are_fatal)
{
    if (strcmp(key, "hide-when-typing") == 0)
        conf->mouse.hide_when_typing = str_to_bool(value);

    else if (strcmp(key, "alternate-scroll-mode") == 0)
        conf->mouse.alternate_scroll_mode = str_to_bool(value);

    else {
        LOG_AND_NOTIFY_ERR("%s:%d: [mouse]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_csd(const char *key, const char *value, struct config *conf,
                     const char *path, unsigned lineno, bool errors_are_fatal)
{
    if (strcmp(key, "preferred") == 0) {
        if (strcmp(value, "server") == 0)
            conf->csd.preferred = CONF_CSD_PREFER_SERVER;
        else if (strcmp(value, "client") == 0)
            conf->csd.preferred = CONF_CSD_PREFER_CLIENT;
        else if (strcmp(value, "none") == 0)
            conf->csd.preferred = CONF_CSD_PREFER_NONE;
        else {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: csd.preferred: expected either "
                "'server', 'client' or 'none'", path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, conf, path, lineno, "csd", "color"))
            return false;

        conf->csd.color.title_set = true;
        conf->csd.color.title = color;
    }

    else if (strcmp(key, "size") == 0) {
        unsigned long pixels;
        if (!str_to_ulong(value, 10, &pixels)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        conf->csd.title_height = pixels;
    }

    else if (strcmp(key, "button-width") == 0) {
        unsigned long pixels;
        if (!str_to_ulong(value, 10, &pixels)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        conf->csd.button_width = pixels;
    }

    else if (strcmp(key, "button-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, conf, path, lineno, "csd", "button-color"))
            return false;

        conf->csd.color.buttons_set = true;
        conf->csd.color.buttons = color;
    }

    else if (strcmp(key, "button-minimize-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, conf, path, lineno, "csd", "button-minimize-color"))
            return false;

        conf->csd.color.minimize_set = true;
        conf->csd.color.minimize = color;
    }

    else if (strcmp(key, "button-maximize-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, conf, path, lineno, "csd", "button-maximize-color"))
            return false;

        conf->csd.color.maximize_set = true;
        conf->csd.color.maximize = color;
    }

    else if (strcmp(key, "button-close-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, conf, path, lineno, "csd", "button-close-color"))
            return false;

        conf->csd.color.close_set = true;
        conf->csd.color.close = color;
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%u: [csd]: %s: invalid action",
                           path, lineno, key);
        return false;
    }

    return true;
}

/* Struct that holds temporary key/mouse binding parsed data */
struct key_combo {
    char *text;          /* Raw text, e.g. "Control+Shift+V" */
    struct config_key_modifiers modifiers;
    union {
        xkb_keysym_t sym;    /* Key converted to an XKB symbol, e.g. XKB_KEY_V */
        struct {
            int button;
            int count;
        } m;
    };
};

struct key_combo_list {
    size_t count;
    struct key_combo *combos;
};

static void NOINLINE
free_key_combo_list(struct key_combo_list *key_combos)
{
    for (size_t i = 0; i < key_combos->count; i++)
        free(key_combos->combos[i].text);
    free(key_combos->combos);
    key_combos->count = 0;
    key_combos->combos = NULL;
}

static bool
parse_modifiers(struct config *conf, const char *text, size_t len,
                struct config_key_modifiers *modifiers, const char *path, unsigned lineno)
{
    bool ret = false;

    *modifiers = (struct config_key_modifiers){0};
    char *copy = xstrndup(text, len);

    for (char *tok_ctx = NULL, *key = strtok_r(copy, "+", &tok_ctx);
         key != NULL;
         key = strtok_r(NULL, "+", &tok_ctx))
    {
        if (strcmp(key, XKB_MOD_NAME_SHIFT) == 0)
            modifiers->shift = true;
        else if (strcmp(key, XKB_MOD_NAME_CTRL) == 0)
            modifiers->ctrl = true;
        else if (strcmp(key, XKB_MOD_NAME_ALT) == 0)
            modifiers->alt = true;
        else if (strcmp(key, XKB_MOD_NAME_LOGO) == 0)
            modifiers->meta = true;
        else {
            LOG_AND_NOTIFY_ERR("%s:%d: %s: not a valid modifier name",
                               path, lineno, key);
            goto out;
        }
    }

    ret = true;

out:
    free(copy);
    return ret;
}

static bool
parse_key_combos(struct config *conf, const char *combos,
                 struct key_combo_list *key_combos,
                 const char *section, const char *option,
                 const char *path, unsigned lineno)
{
    xassert(key_combos != NULL);
    xassert(key_combos->count == 0 && key_combos->combos == NULL);

    size_t size = 0;  /* Size of ‘combos’ array in the key-combo list */

    char *copy = xstrdup(combos);

    for (char *tok_ctx = NULL, *combo = strtok_r(copy, " ", &tok_ctx);
         combo != NULL;
         combo = strtok_r(NULL, " ", &tok_ctx))
    {
        struct config_key_modifiers modifiers = {0};
        char *key = strrchr(combo, '+');

        if (key == NULL) {
            /* No modifiers */
            key = combo;
        } else {
            if (!parse_modifiers(conf, combo, key - combo, &modifiers, path, lineno))
                goto err;
            key++;  /* Skip past the '+' */
        }

#if 0
        if (modifiers.shift && strlen(key) == 1 && (*key >= 'A' && *key <= 'Z')) {
            LOG_WARN(
                "%s:%d: [%s]: %s: %s: "
                "upper case keys not supported with explicit 'Shift' modifier",
                path, lineno, section, option, combo);
            user_notification_add(
                &conf->notifications, USER_NOTIFICATION_DEPRECATED,
                "%s:%d: [%s]: %s: \033[1m%s\033[m: "
                "shifted keys not supported with explicit \033[1mShift\033[m "
                "modifier",
                path, lineno, section, option, combo);
            *key = *key - 'A' + 'a';
        }
#endif
        /* Translate key name to symbol */
        xkb_keysym_t sym = xkb_keysym_from_name(key, 0);
        if (sym == XKB_KEY_NoSymbol) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [%s]: %s: ]%s: key is not a valid XKB key name",
                path, lineno, section, option, key);
            goto err;
        }

        if (key_combos->count + 1 > size) {
            size += 4;
            key_combos->combos = xrealloc(
                key_combos->combos, size * sizeof(key_combos->combos[0]));
        }

        xassert(key_combos->count + 1 <= size);
        key_combos->combos[key_combos->count++] = (struct key_combo){
            .text = xstrdup(combo),
            .modifiers = modifiers,
            .sym = sym,
        };
    }

    free(copy);
    return true;

err:
    free_key_combo_list(key_combos);
    free(copy);
    return false;
}

static bool
has_key_binding_collisions(struct config *conf,
                           int action, const char *const action_map[],
                           const struct config_key_binding_list *bindings,
                           const struct key_combo_list *key_combos,
                           const char *path, unsigned lineno)
{
    for (size_t j = 0; j < bindings->count; j++) {
        const struct config_key_binding *combo1 = &bindings->arr[j];

        if (combo1->action == BIND_ACTION_NONE)
            continue;

        if (combo1->action == action)
            continue;

        for (size_t i = 0; i < key_combos->count; i++) {
            const struct key_combo *combo2 = &key_combos->combos[i];

            const struct config_key_modifiers *mods1 = &combo1->modifiers;
            const struct config_key_modifiers *mods2 = &combo2->modifiers;

            bool shift = mods1->shift == mods2->shift;
            bool alt = mods1->alt == mods2->alt;
            bool ctrl = mods1->ctrl == mods2->ctrl;
            bool meta = mods1->meta == mods2->meta;
            bool sym = combo1->sym == combo2->sym;

            if (shift && alt && ctrl && meta && sym) {
                bool has_pipe = combo1->pipe.argv.args != NULL;
                LOG_AND_NOTIFY_ERR("%s:%d: %s already mapped to '%s%s%s%s'",
                                   path, lineno, combo2->text,
                                   action_map[combo1->action],
                                   has_pipe ? " [" : "",
                                   has_pipe ? combo1->pipe.argv.args[0] : "",
                                   has_pipe ? "]" : "");
                return true;
            }
        }
    }

    return false;
}

static int
argv_compare(char *const *argv1, char *const *argv2)
{
    xassert(argv1 != NULL);
    xassert(argv2 != NULL);

    for (size_t i = 0; ; i++) {
        if (argv1[i] == NULL && argv2[i] == NULL)
            return 0;
        if (argv1[i] == NULL)
            return -1;
        if (argv2[i] == NULL)
            return 1;

        int ret = strcmp(argv1[i], argv2[i]);
        if (ret != 0)
            return ret;
    }

    BUG("unexpected loop break");
    return 1;
}

/*
 * Parses a key binding value on the form
 *  "[cmd-to-exec arg1 arg2] Mods+Key"
 *
 * and extracts 'cmd-to-exec' and its arguments.
 *
 * Input:
 *  - value: raw string, on the form mention above
 *  - cmd: pointer to string to will be allocated and filled with
 *        'cmd-to-exec arg1 arg2'
 *  - argv: point to array of string. Array will be allocated. Will be
 *          filled with {'cmd-to-exec', 'arg1', 'arg2', NULL}
 *
 * Returns:
 *  - ssize_t, number of bytes to strip from 'value' to remove the '[]'
 *    enclosed cmd and its arguments, including any subsequent
 *    whitespace characters. I.e. if 'value' is "[cmd] BTN_RIGHT", the
 *    return value is 6 (strlen("[cmd] ")).
 *  - cmd: allocated string containing "cmd arg1 arg2...". Caller frees.
 *  - argv: allocatd array containing {"cmd", "arg1", "arg2", NULL}. Caller frees.
 */
static ssize_t
pipe_argv_from_string(const char *value, char ***argv,
                      struct config *conf,
                      const char *path, unsigned lineno)
{
    *argv = NULL;

    if (value[0] != '[')
        return 0;

    const char *pipe_cmd_end = strrchr(value, ']');
    if (pipe_cmd_end == NULL) {
        LOG_AND_NOTIFY_ERR("%s:%d: unclosed '['", path, lineno);
        return -1;
    }

    size_t pipe_len = pipe_cmd_end - value - 1;
    char *cmd = xstrndup(&value[1], pipe_len);

    if (!tokenize_cmdline(cmd, argv)) {
        LOG_AND_NOTIFY_ERR("%s:%d: syntax error in command line", path, lineno);
        free(cmd);
        return -1;
    }

    ssize_t remove_len = pipe_cmd_end + 1 - value;
    value = pipe_cmd_end + 1;
    while (isspace(*value)) {
        value++;
        remove_len++;
    }

    free(cmd);
    return remove_len;
}

static void NOINLINE
remove_action_from_key_bindings_list(struct config_key_binding_list *bindings,
                                     int action, char **pipe_argv)
{
    size_t remove_first_idx = 0;
    size_t remove_count = 0;

    for (size_t i = 0; i < bindings->count; i++) {
        struct config_key_binding *binding = &bindings->arr[i];

        if (binding->action == action &&
            ((binding->pipe.argv.args == NULL && pipe_argv == NULL) ||
             (binding->pipe.argv.args != NULL && pipe_argv != NULL &&
              argv_compare(binding->pipe.argv.args, pipe_argv) == 0)))
        {
            if (remove_count++ == 0)
                remove_first_idx = i;

            xassert(remove_first_idx + remove_count - 1 == i);

            if (binding->pipe.master_copy)
                free_argv(&binding->pipe.argv);
        }
    }

    if (remove_count == 0)
        return;

    size_t move_count = bindings->count - (remove_first_idx + remove_count);

    memmove(
        &bindings->arr[remove_first_idx],
        &bindings->arr[remove_first_idx + remove_count],
        move_count * sizeof(bindings->arr[0]));
    bindings->count -= remove_count;
}

static bool NOINLINE
parse_key_binding_section(
    const char *section, const char *key, const char *value,
    int action_count, const char *const action_map[static action_count],
    struct config_key_binding_list *bindings,
    struct config *conf, const char *path, unsigned lineno)
{
    char **pipe_argv;

    ssize_t pipe_remove_len = pipe_argv_from_string(
        value, &pipe_argv, conf, path, lineno);

    if (pipe_remove_len < 0)
        return false;

    value += pipe_remove_len;

    for (int action = 0; action < action_count; action++) {
        if (action_map[action] == NULL)
            continue;

        if (strcmp(key, action_map[action]) != 0)
            continue;

        /* Unset binding */
        if (strcasecmp(value, "none") == 0) {
            remove_action_from_key_bindings_list(bindings, action, pipe_argv);
            free(pipe_argv);
            return true;
        }

        struct key_combo_list key_combos = {0};
        if (!parse_key_combos(
                conf, value, &key_combos, section, key, path, lineno) ||
            has_key_binding_collisions(
                conf, action, action_map, bindings, &key_combos,
                path, lineno))
        {
            free(pipe_argv);
            free_key_combo_list(&key_combos);
            return false;
        }

        remove_action_from_key_bindings_list(bindings, action, pipe_argv);

        /* Emit key bindings */
        size_t ofs = bindings->count;
        bindings->count += key_combos.count;
        bindings->arr = xrealloc(
            bindings->arr, bindings->count * sizeof(bindings->arr[0]));

        bool first = true;
        for (size_t i = 0; i < key_combos.count; i++) {
            const struct key_combo *combo = &key_combos.combos[i];
            struct config_key_binding binding = {
                .action = action,
                .modifiers = combo->modifiers,
                .sym = combo->sym,
                .pipe = {
                    .argv = {
                        .args = pipe_argv,
                    },
                    .master_copy = first,
                },
            };

            /* TODO: we could re-use free:d slots */
            bindings->arr[ofs + i] = binding;
            first = false;
        }

        free_key_combo_list(&key_combos);
        return true;
    }

    LOG_AND_NOTIFY_ERR("%s:%u: [%s]: %s: invalid action",
                       path, lineno, section, key);
    free(pipe_argv);
    return false;
}

UNITTEST
{
    enum test_actions {
        TEST_ACTION_NONE,
        TEST_ACTION_FOO,
        TEST_ACTION_BAR,
        TEST_ACTION_COUNT,
    };

    const char *const map[] = {
        [TEST_ACTION_NONE] = NULL,
        [TEST_ACTION_FOO] = "foo",
        [TEST_ACTION_BAR] = "bar",
    };

    struct config conf = {0};
    struct config_key_binding_list bindings = {0};

    /*
     * ADD foo=Escape
     *
     * This verifies we can bind a single key combo to an action.
     */
    xassert(parse_key_binding_section(
                "", "foo", "Escape", ALEN(map), map, &bindings, &conf, "", 0));
    xassert(bindings.count == 1);
    xassert(bindings.arr[0].action == TEST_ACTION_FOO);
    xassert(bindings.arr[0].sym == XKB_KEY_Escape);

    /*
     * ADD bar=Control+g Control+Shift+x
     *
     * This verifies we can bind multiple key combos to an action.
     */
    xassert(parse_key_binding_section(
                "", "bar", "Control+g Control+Shift+x", ALEN(map), map,
                &bindings, &conf, "", 0));
    xassert(bindings.count == 3);
    xassert(bindings.arr[0].action == TEST_ACTION_FOO);
    xassert(bindings.arr[1].action == TEST_ACTION_BAR);
    xassert(bindings.arr[1].sym == XKB_KEY_g);
    xassert(bindings.arr[1].modifiers.ctrl);
    xassert(bindings.arr[2].action == TEST_ACTION_BAR);
    xassert(bindings.arr[2].sym == XKB_KEY_x);
    xassert(bindings.arr[2].modifiers.ctrl && bindings.arr[2].modifiers.shift);

    /*
     * REPLACE foo with foo=Mod+v Shift+q
     *
     * This verifies we can update a single-combo action with multiple
     * key combos.
     */
    xassert(parse_key_binding_section(
                "", "foo", "Mod1+v Shift+q", ALEN(map), map,
                &bindings, &conf, "", 0));
    xassert(bindings.count == 4);
    xassert(bindings.arr[0].action == TEST_ACTION_BAR);
    xassert(bindings.arr[1].action == TEST_ACTION_BAR);
    xassert(bindings.arr[2].action == TEST_ACTION_FOO);
    xassert(bindings.arr[2].sym == XKB_KEY_v);
    xassert(bindings.arr[2].modifiers.alt);
    xassert(bindings.arr[3].action == TEST_ACTION_FOO);
    xassert(bindings.arr[3].sym == XKB_KEY_q);
    xassert(bindings.arr[3].modifiers.shift);

    /*
     * REMOVE bar
     */
    xassert(parse_key_binding_section(
                "", "bar", "none", ALEN(map), map, &bindings, &conf, "", 0));
    xassert(bindings.count == 2);
    xassert(bindings.arr[0].action == TEST_ACTION_FOO);
    xassert(bindings.arr[1].action == TEST_ACTION_FOO);

    /*
     * REMOVE foo
     */
    xassert(parse_key_binding_section(
                "", "foo", "none", ALEN(map), map, &bindings, &conf, "", 0));
    xassert(bindings.count == 0);

    free(bindings.arr);
}

static bool
parse_section_key_bindings(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno, bool errors_are_fatal)
{
    return parse_key_binding_section(
        "key-bindings", key, value, BIND_ACTION_KEY_COUNT, binding_action_map,
        &conf->bindings.key, conf, path, lineno);
}

static bool
parse_section_search_bindings(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno, bool errors_are_fatal)
{
    static const char *const search_binding_action_map[] = {
        [BIND_ACTION_SEARCH_NONE] = NULL,
        [BIND_ACTION_SEARCH_CANCEL] = "cancel",
        [BIND_ACTION_SEARCH_COMMIT] = "commit",
        [BIND_ACTION_SEARCH_FIND_PREV] = "find-prev",
        [BIND_ACTION_SEARCH_FIND_NEXT] = "find-next",
        [BIND_ACTION_SEARCH_EDIT_LEFT] = "cursor-left",
        [BIND_ACTION_SEARCH_EDIT_LEFT_WORD] = "cursor-left-word",
        [BIND_ACTION_SEARCH_EDIT_RIGHT] = "cursor-right",
        [BIND_ACTION_SEARCH_EDIT_RIGHT_WORD] = "cursor-right-word",
        [BIND_ACTION_SEARCH_EDIT_HOME] = "cursor-home",
        [BIND_ACTION_SEARCH_EDIT_END] = "cursor-end",
        [BIND_ACTION_SEARCH_DELETE_PREV] = "delete-prev",
        [BIND_ACTION_SEARCH_DELETE_PREV_WORD] = "delete-prev-word",
        [BIND_ACTION_SEARCH_DELETE_NEXT] = "delete-next",
        [BIND_ACTION_SEARCH_DELETE_NEXT_WORD] = "delete-next-word",
        [BIND_ACTION_SEARCH_EXTEND_WORD] = "extend-to-word-boundary",
        [BIND_ACTION_SEARCH_EXTEND_WORD_WS] = "extend-to-next-whitespace",
        [BIND_ACTION_SEARCH_CLIPBOARD_PASTE] = "clipboard-paste",
        [BIND_ACTION_SEARCH_PRIMARY_PASTE] = "primary-paste",
    };

    static_assert(ALEN(search_binding_action_map) == BIND_ACTION_SEARCH_COUNT,
                  "search binding action map size mismatch");

    return parse_key_binding_section(
        "search-bindings", key, value, BIND_ACTION_SEARCH_COUNT,
        search_binding_action_map, &conf->bindings.search, conf, path, lineno);
}

static bool
parse_section_url_bindings(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno, bool errors_are_fatal)
{
    static const char *const url_binding_action_map[] = {
        [BIND_ACTION_URL_NONE] = NULL,
        [BIND_ACTION_URL_CANCEL] = "cancel",
        [BIND_ACTION_URL_TOGGLE_URL_ON_JUMP_LABEL] = "toggle-url-visible",
    };

    static_assert(ALEN(url_binding_action_map) == BIND_ACTION_URL_COUNT,
                  "URL binding action map size mismatch");

    return parse_key_binding_section(
        "url-bindings", key, value, BIND_ACTION_URL_COUNT,
        url_binding_action_map, &conf->bindings.url, conf, path, lineno);
}

static bool
parse_mouse_combos(struct config *conf, const char *combos,
                   struct key_combo_list *key_combos,
                   const char *path, unsigned lineno)
{
    xassert(key_combos != NULL);
    xassert(key_combos->count == 0 && key_combos->combos == NULL);

    size_t size = 0;  /* Size of the ‘combos’ array in key_combos */

    char *copy = xstrdup(combos);

    for (char *tok_ctx = NULL, *combo = strtok_r(copy, " ", &tok_ctx);
         combo != NULL;
         combo = strtok_r(NULL, " ", &tok_ctx))
    {
        struct config_key_modifiers modifiers = {0};
        char *key = strrchr(combo, '+');

        if (key == NULL) {
            /* No modifiers */
            key = combo;
        } else {
            *key = '\0';
            if (!parse_modifiers(conf, combo, key - combo, &modifiers, path, lineno))
                goto err;
            if (modifiers.shift) {
                LOG_AND_NOTIFY_ERR(
                    "%s:%d: Shift cannot be used in mouse bindings",
                    path, lineno);
                goto err;
            }
            key++;  /* Skip past the '+' */
        }

        size_t count = 1;
        {
            char *_count = strrchr(key, '-');
            if (_count != NULL) {
                *_count = '\0';
                _count++;

                errno = 0;
                char *end;
                unsigned long value = strtoul(_count, &end, 10);
                if (_count[0] == '\0' || *end != '\0' || errno != 0) {
                    if (errno != 0)
                        LOG_AND_NOTIFY_ERRNO(
                            "%s:%d: %s: invalid click count", path, lineno, _count);
                    else
                        LOG_AND_NOTIFY_ERR(
                            "%s:%d: %s: invalid click count", path, lineno, _count);
                    goto err;
                }
                count = value;
            }
        }

        static const struct {
            const char *name;
            int code;
        } map[] = {
            {"BTN_LEFT", BTN_LEFT},
            {"BTN_RIGHT", BTN_RIGHT},
            {"BTN_MIDDLE", BTN_MIDDLE},
            {"BTN_SIDE", BTN_SIDE},
            {"BTN_EXTRA", BTN_EXTRA},
            {"BTN_FORWARD", BTN_FORWARD},
            {"BTN_BACK", BTN_BACK},
            {"BTN_TASK", BTN_TASK},
        };

        int button = 0;
        for (size_t i = 0; i < ALEN(map); i++) {
            if (strcmp(key, map[i].name) == 0) {
                button = map[i].code;
                break;
            }
        }

        if (button == 0) {
            LOG_AND_NOTIFY_ERR("%s:%d: %s: invalid mouse button name", path, lineno, key);
            goto err;
        }

        struct key_combo new = {
            .text = xstrdup(combo),
            .modifiers = modifiers,
            .m = {
                .button = button,
                .count = count,
            },
        };

        if (key_combos->count + 1 > size) {
            size += 4;
            key_combos->combos = xrealloc(
                key_combos->combos, size * sizeof(key_combos->combos[0]));
        }

        xassert(key_combos->count + 1 <= size);
        key_combos->combos[key_combos->count++] = new;
    }

    free(copy);
    return true;

err:
    free_key_combo_list(key_combos);
    free(copy);
    return false;
}

static bool
has_mouse_binding_collisions(struct config *conf, const struct key_combo_list *key_combos,
                             const char *path, unsigned lineno)
{
    for (size_t j = 0; j < conf->bindings.mouse.count; j++) {
        const struct config_mouse_binding *combo1 = &conf->bindings.mouse.arr[j];
        if (combo1->action == BIND_ACTION_NONE)
            continue;

        for (size_t i = 0; i < key_combos->count; i++) {
            const struct key_combo *combo2 = &key_combos->combos[i];

            const struct config_key_modifiers *mods1 = &combo1->modifiers;
            const struct config_key_modifiers *mods2 = &combo2->modifiers;

            bool shift = mods1->shift == mods2->shift;
            bool alt = mods1->alt == mods2->alt;
            bool ctrl = mods1->ctrl == mods2->ctrl;
            bool meta = mods1->meta == mods2->meta;
            bool button = combo1->button == combo2->m.button;
            bool count = combo1->count == combo2->m.count;

            if (shift && alt && ctrl && meta && button && count) {
                bool has_pipe = combo1->pipe.argv.args != NULL;
                LOG_AND_NOTIFY_ERR("%s:%d: %s already mapped to '%s%s%s%s'",
                                   path, lineno, combo2->text,
                                   binding_action_map[combo1->action],
                                   has_pipe ? " [" : "",
                                   has_pipe ? combo1->pipe.argv.args[0] : "",
                                   has_pipe ? "]" : "");
                return true;
            }
        }
    }

    return false;
}


static bool
parse_section_mouse_bindings(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno, bool errors_are_fatal)
{
    char **pipe_argv;

    ssize_t pipe_remove_len = pipe_argv_from_string(
        value, &pipe_argv, conf, path, lineno);

    if (pipe_remove_len < 0)
        return false;

    value += pipe_remove_len;

    for (enum bind_action_normal action = 0;
         action < BIND_ACTION_COUNT;
         action++)
    {
        if (binding_action_map[action] == NULL)
            continue;

        if (strcmp(key, binding_action_map[action]) != 0)
            continue;

        /* Unset binding */
        if (strcasecmp(value, "none") == 0) {
            for (size_t i = 0; i < conf->bindings.mouse.count; i++) {
                struct config_mouse_binding *binding =
                    &conf->bindings.mouse.arr[i];

                if (binding->action == action) {
                    if (binding->pipe.master_copy)
                        free_argv(&binding->pipe.argv);
                    binding->action = BIND_ACTION_NONE;
                }
            }
            free(pipe_argv);
            return true;
        }

        struct key_combo_list key_combos = {0};
        if (!parse_mouse_combos(conf, value, &key_combos, path, lineno) ||
            has_mouse_binding_collisions(conf, &key_combos, path, lineno))
        {
            free(pipe_argv);
            free_key_combo_list(&key_combos);
            return false;
        }

        /* Remove existing bindings for this action */
        for (size_t i = 0; i < conf->bindings.mouse.count; i++) {
            struct config_mouse_binding *binding = &conf->bindings.mouse.arr[i];

            if (binding->action == action &&
                ((binding->pipe.argv.args == NULL && pipe_argv == NULL) ||
                 (binding->pipe.argv.args != NULL && pipe_argv != NULL &&
                  argv_compare(binding->pipe.argv.args, pipe_argv) == 0)))
            {
                if (binding->pipe.master_copy)
                    free_argv(&binding->pipe.argv);
                binding->action = BIND_ACTION_NONE;
            }
        }

        /* Emit mouse bindings */
        size_t ofs = conf->bindings.mouse.count;
        conf->bindings.mouse.count += key_combos.count;
        conf->bindings.mouse.arr = xrealloc(
            conf->bindings.mouse.arr,
            conf->bindings.mouse.count * sizeof(conf->bindings.mouse.arr[0]));

        bool first = true;
        for (size_t i = 0; i < key_combos.count; i++) {
            const struct key_combo *combo = &key_combos.combos[i];
            struct config_mouse_binding binding = {
                .action = action,
                .modifiers = combo->modifiers,
                .button = combo->m.button,
                .count = combo->m.count,
                .pipe = {
                    .argv = {
                        .args = pipe_argv,
                    },
                    .master_copy = first,
                },
            };

            conf->bindings.mouse.arr[ofs + i] = binding;
            first = false;
        }

        free_key_combo_list(&key_combos);
        return true;
    }

    LOG_AND_NOTIFY_ERR("%s:%u: [mouse-bindings]: %s: invalid key", path, lineno, key);
    free(pipe_argv);
    return false;
}

static bool
parse_section_tweak(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno, bool errors_are_fatal)
{
    if (strcmp(key, "scaling-filter") == 0) {
        static const char filters[][12] = {
            [FCFT_SCALING_FILTER_NONE] = "none",
            [FCFT_SCALING_FILTER_NEAREST] = "nearest",
            [FCFT_SCALING_FILTER_BILINEAR] = "bilinear",
            [FCFT_SCALING_FILTER_CUBIC] = "cubic",
            [FCFT_SCALING_FILTER_LANCZOS3] = "lanczos3",
        };

        for (size_t i = 0; i < ALEN(filters); i++) {
            if (strcmp(value, filters[i]) == 0) {
                conf->tweak.fcft_filter = i;
                LOG_WARN("tweak: scaling-filter=%s", filters[i]);
                return true;
            }
        }

        LOG_AND_NOTIFY_ERR(
            "%s:%d: [tweak]: %s: invalid 'scaling-filter' value, "
            "expected one of 'none', 'nearest', 'bilinear', 'cubic' or "
            "'lanczos3'", path, lineno, value);
        return false;
    }

    else if (strcmp(key, "overflowing-glyphs") == 0) {
        conf->tweak.overflowing_glyphs = str_to_bool(value);
        if (!conf->tweak.overflowing_glyphs)
            LOG_WARN("tweak: disabled overflowing glyphs");
    }

    else if (strcmp(key, "damage-whole-window") == 0) {
        conf->tweak.damage_whole_window = str_to_bool(value);
        if (conf->tweak.damage_whole_window)
            LOG_WARN("tweak: damage whole window");
    }

    else if (strcmp(key, "grapheme-shaping") == 0) {
        conf->tweak.grapheme_shaping = str_to_bool(value);

#if !defined(FOOT_GRAPHEME_CLUSTERING)
        if (conf->tweak.grapheme_shaping) {
            LOG_AND_NOTIFY_WARN(
                "%s:%d: [tweak]: "
                "grapheme-shaping enabled but foot was not compiled with "
                "support for it", path, lineno);
            conf->tweak.grapheme_shaping = false;
        }
#endif

        if (conf->tweak.grapheme_shaping && !conf->can_shape_grapheme) {
            LOG_WARN(
                "%s:%d [tweak]: "
                "grapheme-shaping enabled but fcft was not compiled with "
                "support for it", path, lineno);

            /* Keep it enabled though - this will cause us to do
             * grapheme-clustering at least */
        }

        if (conf->tweak.grapheme_shaping)
            LOG_WARN("tweak: grapheme shaping");
    }

    else if (strcmp(key, "grapheme-width-method") == 0) {
        if (strcmp(value, "double-width") == 0)
            conf->tweak.grapheme_width_method = GRAPHEME_WIDTH_DOUBLE;
        else if (strcmp(value, "wcswidth") == 0)
            conf->tweak.grapheme_width_method = GRAPHEME_WIDTH_WCSWIDTH;

        LOG_WARN("%s:%d [tweak]: grapheme-width-method=%s", path, lineno, value);
    }

    else if (strcmp(key, "render-timer") == 0) {
        if (strcmp(value, "none") == 0) {
            conf->tweak.render_timer_osd = false;
            conf->tweak.render_timer_log = false;
        } else if (strcmp(value, "osd") == 0) {
            conf->tweak.render_timer_osd = true;
            conf->tweak.render_timer_log = false;
        } else if (strcmp(value, "log") == 0) {
            conf->tweak.render_timer_osd = false;
            conf->tweak.render_timer_log = true;
        } else if (strcmp(value, "both") == 0) {
            conf->tweak.render_timer_osd = true;
            conf->tweak.render_timer_log = true;
        } else {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [tweak]: %s: invalid 'render-timer' value, "
                "expected one of 'none', 'osd', 'log' or 'both'",
                path, lineno, value);
            return false;
        }
    }

    else if (strcmp(key, "delayed-render-lower") == 0) {
        unsigned long ns;
        if (!str_to_ulong(value, 10, &ns)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        if (ns > 16666666) {
            LOG_AND_NOTIFY_ERR("%s:%d: timeout must not exceed 16ms", path, lineno);
            return false;
        }

        conf->tweak.delayed_render_lower_ns = ns;
        LOG_WARN("tweak: delayed-render-lower=%lu", ns);
    }

    else if (strcmp(key, "delayed-render-upper") == 0) {
        unsigned long ns;
        if (!str_to_ulong(value, 10, &ns)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        if (ns > 16666666) {
            LOG_AND_NOTIFY_ERR("%s:%d: timeout must not exceed 16ms", path, lineno);
            return false;
        }

        conf->tweak.delayed_render_upper_ns = ns;
        LOG_WARN("tweak: delayed-render-upper=%lu", ns);
    }

    else if (strcmp(key, "max-shm-pool-size-mb") == 0) {
        unsigned long mb;
        if (!str_to_ulong(value, 10, &mb)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        conf->tweak.max_shm_pool_size = min(mb * 1024 * 1024, INT32_MAX);
        LOG_WARN("tweak: max-shm-pool-size=%lld bytes",
                 (long long)conf->tweak.max_shm_pool_size);
    }

    else if (strcmp(key, "box-drawing-base-thickness") == 0) {
        double base_thickness;
        if (!str_to_double(value, &base_thickness)) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [tweak]: box-drawing-base-thickness: "
                "expected a decimal value, got '%s'", path, lineno, value);
            return false;
        }

        conf->tweak.box_drawing_base_thickness = base_thickness;
        LOG_WARN("tweak: box-drawing-base-thickness=%f",
                 conf->tweak.box_drawing_base_thickness);
    }

    else if (strcmp(key, "box-drawing-solid-shades") == 0) {
        conf->tweak.box_drawing_solid_shades = str_to_bool(value);

        if (!conf->tweak.box_drawing_solid_shades)
            LOG_WARN("tweak: box-drawing-solid-shades=%s",
                     conf->tweak.box_drawing_solid_shades ? "yes" : "no");
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%u: [tweak]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_key_value(char *kv, char **section, char **key, char **value)
{

    /*strip leading whitespace*/
    while (*kv && isspace(*kv))
        ++kv;

    if (section != NULL)
        *section = NULL;
    *key = kv;
    *value = NULL;

    size_t kvlen = strlen(kv);
    for (size_t i = 0; i < kvlen; ++i) {
        if (kv[i] == '.') {
            if (section != NULL && *section == NULL) {
                *section = kv;
                kv[i] = '\0';
                *key = &kv[i + 1];
            }
        } else if (kv[i] == '=') {
            if (section != NULL && *section == NULL)
                *section = "main";
            kv[i] = '\0';
            *value = &kv[i + 1];
            break;
        }
    }
    if (*value == NULL)
        return false;

    /* Strip trailing whitespace from key (leading stripped earlier) */
    {
        xassert(!isspace(**key));

        char *end = *key + strlen(*key) - 1;
        while (isspace(*end))
            end--;
        *(end + 1) = '\0';
    }

    /* Strip leading+trailing whitespace from valueue */
    {
        while (isspace(**value))
            ++*value;

        if (*value[0] != '\0') {
            char *end = *value + strlen(*value) - 1;
            while (isspace(*end))
                end--;
            *(end + 1) = '\0';
        }
    }
    return true;
}

enum section {
    SECTION_MAIN,
    SECTION_BELL,
    SECTION_SCROLLBACK,
    SECTION_URL,
    SECTION_COLORS,
    SECTION_CURSOR,
    SECTION_MOUSE,
    SECTION_CSD,
    SECTION_KEY_BINDINGS,
    SECTION_SEARCH_BINDINGS,
    SECTION_URL_BINDINGS,
    SECTION_MOUSE_BINDINGS,
    SECTION_TWEAK,
    SECTION_COUNT,
};

/* Function pointer, called for each key/value line */
typedef bool (*parser_fun_t)(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno, bool errors_are_fatal);

static const struct {
    parser_fun_t fun;
    const char *name;
} section_info[] = {
    [SECTION_MAIN] =            {&parse_section_main, "main"},
    [SECTION_BELL] =            {&parse_section_bell, "bell"},
    [SECTION_SCROLLBACK] =      {&parse_section_scrollback, "scrollback"},
    [SECTION_URL] =             {&parse_section_url, "url"},
    [SECTION_COLORS] =          {&parse_section_colors, "colors"},
    [SECTION_CURSOR] =          {&parse_section_cursor, "cursor"},
    [SECTION_MOUSE] =           {&parse_section_mouse, "mouse"},
    [SECTION_CSD] =             {&parse_section_csd, "csd"},
    [SECTION_KEY_BINDINGS] =    {&parse_section_key_bindings, "key-bindings"},
    [SECTION_SEARCH_BINDINGS] = {&parse_section_search_bindings, "search-bindings"},
    [SECTION_URL_BINDINGS] =    {&parse_section_url_bindings, "url-bindings"},
    [SECTION_MOUSE_BINDINGS] =  {&parse_section_mouse_bindings, "mouse-bindings"},
    [SECTION_TWEAK] =           {&parse_section_tweak, "tweak"},
};

static_assert(ALEN(section_info) == SECTION_COUNT, "section info array size mismatch");

static enum section
str_to_section(const char *str)
{
    for (enum section section = SECTION_MAIN; section < SECTION_COUNT; ++section) {
        if (strcmp(str, section_info[section].name) == 0)
            return section;
    }
    return SECTION_COUNT;
}

static bool
parse_config_file(FILE *f, struct config *conf, const char *path, bool errors_are_fatal)
{
    enum section section = SECTION_MAIN;

    unsigned lineno = 0;

    char *_line = NULL;
    size_t count = 0;

#define error_or_continue()                     \
    {                                           \
        if (errors_are_fatal)                   \
            goto err;                           \
        else                                    \
            continue;                           \
    }

    while (true) {
        errno = 0;
        lineno++;

        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            if (errno != 0) {
                LOG_AND_NOTIFY_ERRNO("failed to read from configuration");
                if (errors_are_fatal)
                    goto err;
            }
            break;
        }

        /* Strip leading whitespace */
        char *line = _line;
        {
            while (isspace(*line))
                line++;
            if (line[0] != '\0') {
                char *end = line + strlen(line) - 1;
                while (isspace(*end))
                    end--;
                *(end + 1) = '\0';
            }
        }

        /* Empty line, or comment */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* Split up into key/value pair + trailing comment separated by blank */
        char *key_value = line;
        char *comment = line;
        while (comment[0] != '\0') {
            const char c = comment[0];
            comment++;
            if (isblank(c) && comment[0] == '#') {
                comment[0] = '\0'; /* Terminate key/value pair */
                comment++;
                break;
            }
        }

        /* Check for new section */
        if (key_value[0] == '[') {
            char *end = strchr(key_value, ']');
            if (end == NULL) {
                LOG_AND_NOTIFY_ERR("%s:%d: syntax error: %s", path, lineno, key_value);
                error_or_continue();
            }

            *end = '\0';

            section = str_to_section(&key_value[1]);
            if (section == SECTION_COUNT) {
                LOG_AND_NOTIFY_ERR("%s:%d: invalid section name: %s", path, lineno, &key_value[1]);
                error_or_continue();
            }

            /* Process next line */
            continue;
        }

        if (section >= SECTION_COUNT) {
            /* Last section name was invalid; ignore all keys in it */
            continue;
        }

        char *key, *value;
        if (!parse_key_value(key_value, NULL, &key, &value)) {
            LOG_AND_NOTIFY_ERR("%s:%d: syntax error: %s", path, lineno, key_value);
            if (errors_are_fatal)
                goto err;
            break;
        }

        LOG_DBG("section=%s, key='%s', value='%s', comment='%s'",
                section_info[section].name, key, value, comment);

        xassert(section >= 0 && section < SECTION_COUNT);

        parser_fun_t section_parser = section_info[section].fun;
        xassert(section_parser != NULL);

        if (!section_parser(key, value, conf, path, lineno, errors_are_fatal))
            error_or_continue();
    }

    free(_line);
    return true;

err:
    free(_line);
    return false;
}

static char *
get_server_socket_path(void)
{
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime == NULL)
        return xstrdup("/tmp/foot.sock");

    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display == NULL) {
        return xasprintf("%s/foot.sock", xdg_runtime);
    }

    return xasprintf("%s/foot-%s.sock", xdg_runtime, wayland_display);
}

#define m_none       {0}
#define m_alt        {.alt = true}
#define m_ctrl       {.ctrl = true}
#define m_shift      {.shift = true}
#define m_ctrl_shift {.ctrl = true, .shift = true}

static void
add_default_key_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_SCROLLBACK_UP_PAGE, m_shift, XKB_KEY_Page_Up},
        {BIND_ACTION_SCROLLBACK_DOWN_PAGE, m_shift, XKB_KEY_Page_Down},
        {BIND_ACTION_CLIPBOARD_COPY, m_ctrl_shift, XKB_KEY_c},
        {BIND_ACTION_CLIPBOARD_PASTE, m_ctrl_shift, XKB_KEY_v},
        {BIND_ACTION_PRIMARY_PASTE, m_shift, XKB_KEY_Insert},
        {BIND_ACTION_SEARCH_START, m_ctrl_shift, XKB_KEY_r},
        {BIND_ACTION_FONT_SIZE_UP, m_ctrl, XKB_KEY_plus},
        {BIND_ACTION_FONT_SIZE_UP, m_ctrl, XKB_KEY_equal},
        {BIND_ACTION_FONT_SIZE_UP, m_ctrl, XKB_KEY_KP_Add},
        {BIND_ACTION_FONT_SIZE_DOWN, m_ctrl, XKB_KEY_minus},
        {BIND_ACTION_FONT_SIZE_DOWN, m_ctrl, XKB_KEY_KP_Subtract},
        {BIND_ACTION_FONT_SIZE_RESET, m_ctrl, XKB_KEY_0},
        {BIND_ACTION_FONT_SIZE_RESET, m_ctrl, XKB_KEY_KP_0},
        {BIND_ACTION_SPAWN_TERMINAL, m_ctrl_shift, XKB_KEY_n},
        {BIND_ACTION_SHOW_URLS_LAUNCH, m_ctrl_shift, XKB_KEY_u},
    };

    conf->bindings.key.count = ALEN(bindings);
    conf->bindings.key.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.key.arr, bindings, sizeof(bindings));
}


static void
add_default_search_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_SEARCH_CANCEL, m_ctrl, XKB_KEY_c},
        {BIND_ACTION_SEARCH_CANCEL, m_ctrl, XKB_KEY_g},
        {BIND_ACTION_SEARCH_CANCEL, m_none, XKB_KEY_Escape},
        {BIND_ACTION_SEARCH_COMMIT, m_none, XKB_KEY_Return},
        {BIND_ACTION_SEARCH_FIND_PREV, m_ctrl, XKB_KEY_r},
        {BIND_ACTION_SEARCH_FIND_NEXT, m_ctrl, XKB_KEY_s},
        {BIND_ACTION_SEARCH_EDIT_LEFT, m_none, XKB_KEY_Left},
        {BIND_ACTION_SEARCH_EDIT_LEFT, m_ctrl, XKB_KEY_b},
        {BIND_ACTION_SEARCH_EDIT_LEFT_WORD, m_ctrl, XKB_KEY_Left},
        {BIND_ACTION_SEARCH_EDIT_LEFT_WORD, m_alt, XKB_KEY_b},
        {BIND_ACTION_SEARCH_EDIT_RIGHT, m_none, XKB_KEY_Right},
        {BIND_ACTION_SEARCH_EDIT_RIGHT, m_ctrl, XKB_KEY_f},
        {BIND_ACTION_SEARCH_EDIT_RIGHT_WORD, m_ctrl, XKB_KEY_Right},
        {BIND_ACTION_SEARCH_EDIT_RIGHT_WORD, m_alt, XKB_KEY_f},
        {BIND_ACTION_SEARCH_EDIT_HOME, m_none, XKB_KEY_Home},
        {BIND_ACTION_SEARCH_EDIT_HOME, m_ctrl, XKB_KEY_a},
        {BIND_ACTION_SEARCH_EDIT_END, m_none, XKB_KEY_End},
        {BIND_ACTION_SEARCH_EDIT_END, m_ctrl, XKB_KEY_e},
        {BIND_ACTION_SEARCH_DELETE_PREV, m_none, XKB_KEY_BackSpace},
        {BIND_ACTION_SEARCH_DELETE_PREV_WORD, m_ctrl, XKB_KEY_BackSpace},
        {BIND_ACTION_SEARCH_DELETE_PREV_WORD, m_alt, XKB_KEY_BackSpace},
        {BIND_ACTION_SEARCH_DELETE_NEXT, m_none, XKB_KEY_Delete},
        {BIND_ACTION_SEARCH_DELETE_NEXT_WORD, m_ctrl, XKB_KEY_Delete},
        {BIND_ACTION_SEARCH_DELETE_NEXT_WORD, m_alt, XKB_KEY_d},
        {BIND_ACTION_SEARCH_EXTEND_WORD, m_ctrl, XKB_KEY_w},
        {BIND_ACTION_SEARCH_EXTEND_WORD_WS, m_ctrl_shift, XKB_KEY_w},
        {BIND_ACTION_SEARCH_CLIPBOARD_PASTE, m_ctrl, XKB_KEY_v},
        {BIND_ACTION_SEARCH_CLIPBOARD_PASTE, m_ctrl, XKB_KEY_y},
        {BIND_ACTION_SEARCH_PRIMARY_PASTE, m_shift, XKB_KEY_Insert},
    };

    conf->bindings.search.count = ALEN(bindings);
    conf->bindings.search.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.search.arr, bindings, sizeof(bindings));
}

static void
add_default_url_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_URL_CANCEL, m_ctrl, XKB_KEY_c},
        {BIND_ACTION_URL_CANCEL, m_ctrl, XKB_KEY_g},
        {BIND_ACTION_URL_CANCEL, m_ctrl, XKB_KEY_d},
        {BIND_ACTION_URL_CANCEL, m_none, XKB_KEY_Escape},
        {BIND_ACTION_URL_TOGGLE_URL_ON_JUMP_LABEL, m_none, XKB_KEY_t},
    };

    conf->bindings.url.count = ALEN(bindings);
    conf->bindings.url.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.url.arr, bindings, sizeof(bindings));
}

static void
add_default_mouse_bindings(struct config *conf)
{
    static const struct config_mouse_binding bindings[] = {
        {BIND_ACTION_PRIMARY_PASTE, m_none, BTN_MIDDLE, 1},
        {BIND_ACTION_SELECT_BEGIN, m_none, BTN_LEFT, 1},
        {BIND_ACTION_SELECT_BEGIN_BLOCK, m_ctrl, BTN_LEFT, 1},
        {BIND_ACTION_SELECT_EXTEND, m_none, BTN_RIGHT, 1},
        {BIND_ACTION_SELECT_EXTEND_CHAR_WISE, m_ctrl, BTN_RIGHT, 1},
        {BIND_ACTION_SELECT_WORD, m_none, BTN_LEFT, 2},
        {BIND_ACTION_SELECT_WORD_WS, m_ctrl, BTN_LEFT, 2},
        {BIND_ACTION_SELECT_ROW, m_none, BTN_LEFT, 3},
    };

    conf->bindings.mouse.count = ALEN(bindings);
    conf->bindings.mouse.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.mouse.arr, bindings, sizeof(bindings));
}

bool
config_load(struct config *conf, const char *conf_path,
            user_notifications_t *initial_user_notifications,
            config_override_t *overrides, bool errors_are_fatal)
{
    bool ret = false;
    enum fcft_capabilities fcft_caps = fcft_capabilities();

    *conf = (struct config) {
        .term = xstrdup(DEFAULT_TERM),
        .shell = get_shell(),
        .title = xstrdup("foot"),
        .app_id = xstrdup("foot"),
        .word_delimiters = xwcsdup(L",│`|:\"'()[]{}<>"),
        .size = {
            .type = CONF_SIZE_PX,
            .width = 700,
            .height = 500,
        },
        .pad_x = 2,
        .pad_y = 2,
        .resize_delay_ms = 100,
        .bold_in_bright = {
            .enabled = false,
            .palette_based = false,
        },
        .startup_mode = STARTUP_WINDOWED,
        .fonts = {{0}},
        .line_height = {.pt = 0, .px = -1},
        .letter_spacing = {.pt = 0, .px = 0},
        .horizontal_letter_offset = {.pt = 0, .px = 0},
        .vertical_letter_offset = {.pt = 0, .px = 0},
        .use_custom_underline_offset = false,
        .box_drawings_uses_font_glyphs = false,
        .subpixel_with_alpha = false,
        .dpi_aware = DPI_AWARE_AUTO, /* DPI-aware when scaling-factor == 1 */
        .bell = {
            .urgent = false,
            .notify = false,
            .command = {
                .argv = {.args = NULL},
            },
            .command_focused = false,
        },
        .url = {
            .label_letters = xwcsdup(L"sadfjklewcmpgh"),
            .osc8_underline = OSC8_UNDERLINE_URL_MODE,
        },
        .can_shape_grapheme = fcft_caps & FCFT_CAPABILITY_GRAPHEME_SHAPING,
        .scrollback = {
            .lines = 1000,
            .indicator = {
                .position = SCROLLBACK_INDICATOR_POSITION_RELATIVE,
                .format = SCROLLBACK_INDICATOR_FORMAT_TEXT,
                .text = wcsdup(L""),
            },
            .multiplier = 3.,
        },
        .colors = {
            .fg = default_foreground,
            .bg = default_background,
            .table = {
                default_regular[0],
                default_regular[1],
                default_regular[2],
                default_regular[3],
                default_regular[4],
                default_regular[5],
                default_regular[6],
                default_regular[7],

                default_bright[0],
                default_bright[1],
                default_bright[2],
                default_bright[3],
                default_bright[4],
                default_bright[5],
                default_bright[6],
                default_bright[7],
            },
            .alpha = 0xffff,
            .selection_fg = 0x80000000,  /* Use default bg */
            .selection_bg = 0x80000000,  /* Use default fg */
            .use_custom = {
                .selection = false,
                .jump_label = false,
                .url = false,
            },
        },

        .cursor = {
            .style = CURSOR_BLOCK,
            .blink = false,
            .color = {
                .text = 0,
                .cursor = 0,
            },
            .beam_thickness = {.pt = 1.5},
            .underline_thickness = {.pt = 0., .px = -1},
        },
        .mouse = {
            .hide_when_typing = false,
            .alternate_scroll_mode = true,
        },
        .csd = {
            .preferred = CONF_CSD_PREFER_SERVER,
            .title_height = 26,
            .border_width = 5,
            .button_width = 26,
        },

        .render_worker_count = sysconf(_SC_NPROCESSORS_ONLN),
        .server_socket_path = get_server_socket_path(),
        .presentation_timings = false,
        .selection_target = SELECTION_TARGET_PRIMARY,
        .hold_at_exit = false,
        .notify = {
            .argv = {.args = NULL},
        },

        .tweak = {
            .fcft_filter = FCFT_SCALING_FILTER_LANCZOS3,
            .overflowing_glyphs = true,
            .grapheme_shaping = false,
            .grapheme_width_method = GRAPHEME_WIDTH_DOUBLE,
            .delayed_render_lower_ns = 500000,         /* 0.5ms */
            .delayed_render_upper_ns = 16666666 / 2,   /* half a frame period (60Hz) */
            .max_shm_pool_size = 512 * 1024 * 1024,
            .render_timer_osd = false,
            .render_timer_log = false,
            .damage_whole_window = false,
            .box_drawing_base_thickness = 0.04,
            .box_drawing_solid_shades = true,
        },

        .notifications = tll_init(),
    };

    /* Initialize the color cube */
    {
        /* First 16 entries correspond to the "regular" and "bright"
         * colors, and have already been initialized */

        /* Then follows 216 RGB shades */
        for (size_t r = 0; r < 6; r++) {
            for (size_t g = 0; g < 6; g++) {
                for (size_t b = 0; b < 6; b++) {
                    uint8_t red = r ? r * 40 + 55 : 0;
                    uint8_t green = g ? g * 40 + 55 : 0;
                    uint8_t blue = b ? b * 40 + 55 : 0;
                    conf->colors.table[16 + r * 6 * 6 + g * 6 + b]
                        = red << 16 | green << 8 | blue << 0;
                }
            }
        }

        /* And finally 24 shades of gray */
        for (size_t i = 0; i < 24; i++) {
            uint8_t level = i * 10 + 8;
            conf->colors.table[232 + i] = level << 16 | level << 8 | level << 0;
        }
    }

    tokenize_cmdline("notify-send -a ${app-id} -i ${app-id} ${title} ${body}",
                     &conf->notify.argv.args);
    tokenize_cmdline("xdg-open ${url}", &conf->url.launch.argv.args);

    static const wchar_t *url_protocols[] = {
        L"http://",
        L"https://",
        L"ftp://",
        L"ftps://",
        L"file://",
        L"gemini://",
        L"gopher://",
    };
    conf->url.protocols = xmalloc(
        ALEN(url_protocols) * sizeof(conf->url.protocols[0]));
    conf->url.prot_count = ALEN(url_protocols);
    conf->url.max_prot_len = 0;

    for (size_t i = 0; i < ALEN(url_protocols); i++) {
        size_t len = wcslen(url_protocols[i]);
        if (len > conf->url.max_prot_len)
            conf->url.max_prot_len = len;
        conf->url.protocols[i] = xwcsdup(url_protocols[i]);
    }

    tll_foreach(*initial_user_notifications, it) {
        tll_push_back(conf->notifications, it->item);
        tll_remove(*initial_user_notifications, it);
    }

    add_default_key_bindings(conf);
    add_default_search_bindings(conf);
    add_default_url_bindings(conf);
    add_default_mouse_bindings(conf);

    struct config_file conf_file = {.path = NULL, .fd = -1};
    if (conf_path != NULL) {
        int fd = open(conf_path, O_RDONLY);
        if (fd < 0) {
            LOG_AND_NOTIFY_ERRNO("%s: failed to open", conf_path);
            ret = !errors_are_fatal;
            goto out;
        }

        conf_file.path = xstrdup(conf_path);
        conf_file.fd = fd;
    } else {
        conf_file = open_config();
        if (conf_file.fd < 0) {
            LOG_WARN("no configuration found, using defaults");
            ret = !errors_are_fatal;
            goto out;
        }
    }

    xassert(conf_file.path != NULL);
    xassert(conf_file.fd >= 0);
    LOG_INFO("loading configuration from %s", conf_file.path);

    FILE *f = fdopen(conf_file.fd, "r");
    if (f == NULL) {
        LOG_AND_NOTIFY_ERRNO("%s: failed to open", conf_file.path);
        ret = !errors_are_fatal;
        goto out;
    }

    ret = parse_config_file(f, conf, conf_file.path, errors_are_fatal) &&
          config_override_apply(conf, overrides, errors_are_fatal);
    fclose(f);

    conf->colors.use_custom.selection =
        conf->colors.selection_fg >> 24 == 0 &&
        conf->colors.selection_bg >> 24 == 0;

out:
    if (ret && conf->fonts[0].count == 0) {
        struct config_font font;
        if (!config_font_parse("monospace", &font)) {
            LOG_ERR("failed to load font 'monospace' - no fonts installed?");
            ret = false;
        } else {
            conf->fonts[0].count = 1;
            conf->fonts[0].arr = malloc(sizeof(font));
            conf->fonts[0].arr[0] = font;
        }
    }

#if defined(_DEBUG)
    for (size_t i = 0; i < conf->bindings.key.count; i++)
        xassert(conf->bindings.key.arr[i].action != BIND_ACTION_NONE);
    for (size_t i = 0; i < conf->bindings.search.count; i++)
        xassert(conf->bindings.search.arr[i].action != BIND_ACTION_SEARCH_NONE);
    for (size_t i = 0; i < conf->bindings.url.count; i++)
        xassert(conf->bindings.url.arr[i].action != BIND_ACTION_URL_NONE);
#endif

    free(conf_file.path);
    if (conf_file.fd >= 0)
        close(conf_file.fd);

    return ret;
}

bool
config_override_apply(struct config *conf, config_override_t *overrides, bool errors_are_fatal)
{
    int i = -1;
    tll_foreach(*overrides, it) {
        ++i;
        char *section_str, *key, *value;
        if (!parse_key_value(it->item, &section_str, &key, &value)) {
            LOG_AND_NOTIFY_ERR("syntax error: %s", it->item);
            if (errors_are_fatal)
                return false;
            continue;
        }

        enum section section = str_to_section(section_str);
        if (section == SECTION_COUNT) {
            LOG_AND_NOTIFY_ERR("override: invalid section name: %s", section_str);
            if (errors_are_fatal)
                return false;
            continue;
        }
        parser_fun_t section_parser = section_info[section].fun;
        xassert(section_parser != NULL);

        if (!section_parser(key, value, conf, "override", i, errors_are_fatal)) {
            if (errors_are_fatal)
                return false;
            continue;
        }
    }
    return true;
}

static void
binding_pipe_free(struct config_binding_pipe *pipe)
{
    if (pipe->master_copy)
        free_argv(&pipe->argv);
}

static void
binding_pipe_clone(struct config_binding_pipe *dst,
                   const struct config_binding_pipe *src)
{
    xassert(src->master_copy);
    clone_argv(&dst->argv, &src->argv);
}

static void NOINLINE
key_binding_list_free(struct config_key_binding_list *bindings)
{
    for (size_t i = 0; i < bindings->count; i++)
        binding_pipe_free(&bindings->arr[i].pipe);
    free(bindings->arr);
}

static void NOINLINE
key_binding_list_clone(struct config_key_binding_list *dst,
                       const struct config_key_binding_list *src)
{
    struct argv *last_master_argv = NULL;

    dst->count = src->count;
    dst->arr = xmalloc(src->count * sizeof(dst->arr[0]));

    for (size_t i = 0; i < src->count; i++) {
        const struct config_key_binding *old = &src->arr[i];
        struct config_key_binding *new = &dst->arr[i];

        *new = *old;

        if (old->pipe.argv.args == NULL)
            continue;

        if (old->pipe.master_copy) {
            binding_pipe_clone(&new->pipe, &old->pipe);
            last_master_argv = &new->pipe.argv;
        } else {
            xassert(last_master_argv != NULL);
            new->pipe.argv = *last_master_argv;
        }
    }
}

static void
mouse_binding_list_free(struct config_mouse_binding_list *bindings)
{
    for (size_t i = 0; i < bindings->count; i++)
        binding_pipe_free(&bindings->arr[i].pipe);
    free(bindings->arr);
}

static void NOINLINE
mouse_binding_list_clone(struct config_mouse_binding_list *dst,
                         const struct config_mouse_binding_list *src)
{
    struct argv *last_master_argv = NULL;

    dst->count = src->count;
    dst->arr = xmalloc(src->count * sizeof(dst->arr[0]));

    for (size_t i = 0; i < src->count; i++) {
        const struct config_mouse_binding *old = &src->arr[i];
        struct config_mouse_binding *new = &dst->arr[i];

        *new = *old;

        if (old->pipe.argv.args == NULL)
            continue;

        if (old->pipe.master_copy) {
            binding_pipe_clone(&new->pipe, &old->pipe);
            last_master_argv = &new->pipe.argv;
        } else {
            xassert(last_master_argv != NULL);
            new->pipe.argv = *last_master_argv;
        }
    }
}

struct config *
config_clone(const struct config *old)
{
    struct config *conf = xmalloc(sizeof(*conf));
    *conf = *old;

    conf->term = xstrdup(old->term);
    conf->shell = xstrdup(old->shell);
    conf->title = xstrdup(old->title);
    conf->app_id = xstrdup(old->app_id);
    conf->word_delimiters = xwcsdup(old->word_delimiters);
    conf->scrollback.indicator.text = xwcsdup(old->scrollback.indicator.text);
    conf->server_socket_path = xstrdup(old->server_socket_path);
    spawn_template_clone(&conf->bell.command, &old->bell.command);
    spawn_template_clone(&conf->notify, &old->notify);

    for (size_t i = 0; i < ALEN(conf->fonts); i++) {
        struct config_font_list *dst = &conf->fonts[i];
        const struct config_font_list *src = &old->fonts[i];

        dst->count = src->count;
        dst->arr = xmalloc(dst->count * sizeof(dst->arr[0]));

        for (size_t j = 0; j < dst->count; j++) {
            dst->arr[j].pt_size = src->arr[j].pt_size;
            dst->arr[j].px_size = src->arr[j].px_size;
            dst->arr[j].pattern = xstrdup(src->arr[j].pattern);
        }
    }

    conf->url.label_letters = xwcsdup(old->url.label_letters);
    spawn_template_clone(&conf->url.launch, &old->url.launch);
    conf->url.protocols = xmalloc(
        old->url.prot_count * sizeof(conf->url.protocols[0]));
    for (size_t i = 0; i < old->url.prot_count; i++)
        conf->url.protocols[i] = xwcsdup(old->url.protocols[i]);

    key_binding_list_clone(&conf->bindings.key, &old->bindings.key);
    key_binding_list_clone(&conf->bindings.search, &old->bindings.search);
    key_binding_list_clone(&conf->bindings.url, &old->bindings.url);
    mouse_binding_list_clone(&conf->bindings.mouse, &old->bindings.mouse);

    conf->notifications.length = 0;
    conf->notifications.head = conf->notifications.tail = 0;
    tll_foreach(old->notifications, it) {
        struct user_notification notif = {
            .kind = it->item.kind,
            .text = xstrdup(it->item.text),
        };
        tll_push_back(conf->notifications, notif);
    }

    return conf;
}

UNITTEST
{
    struct config original;
    user_notifications_t nots = tll_init();
    config_override_t overrides = tll_init();

    bool ret = config_load(&original, "/dev/null", &nots, &overrides, false);
    xassert(ret);

    struct config *clone = config_clone(&original);
    xassert(clone != NULL);
    xassert(clone != &original);

    config_free(original);
    config_free(*clone);
    free(clone);

    tll_free(overrides);
    tll_free(nots);
}

void
config_free(struct config conf)
{
    free(conf.term);
    free(conf.shell);
    free(conf.title);
    free(conf.app_id);
    free(conf.word_delimiters);
    spawn_template_free(&conf.bell.command);
    free(conf.scrollback.indicator.text);
    spawn_template_free(&conf.notify);
    for (size_t i = 0; i < ALEN(conf.fonts); i++)
        config_font_list_destroy(&conf.fonts[i]);
    free(conf.server_socket_path);

    free(conf.url.label_letters);
    spawn_template_free(&conf.url.launch);
    for (size_t i = 0; i < conf.url.prot_count; i++)
        free(conf.url.protocols[i]);
    free(conf.url.protocols);

    key_binding_list_free(&conf.bindings.key);
    key_binding_list_free(&conf.bindings.search);
    key_binding_list_free(&conf.bindings.url);
    mouse_binding_list_free(&conf.bindings.mouse);

    user_notifications_free(&conf.notifications);
}

bool
config_font_parse(const char *pattern, struct config_font *font)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    if (pat == NULL)
        return false;

    double pt_size = -1.0;
    FcPatternGetDouble(pat, FC_SIZE, 0, &pt_size);
    FcPatternRemove(pat, FC_SIZE, 0);

    int px_size = -1;
    FcPatternGetInteger(pat, FC_PIXEL_SIZE, 0, &px_size);
    FcPatternRemove(pat, FC_PIXEL_SIZE, 0);

    if (pt_size == -1. && px_size == -1)
        pt_size = 8.0;

    char *stripped_pattern = (char *)FcNameUnparse(pat);
    FcPatternDestroy(pat);

    *font = (struct config_font){
        .pattern = stripped_pattern,
        .pt_size = pt_size,
        .px_size = px_size
    };
    return true;
}

void
config_font_list_destroy(struct config_font_list *font_list)
{
    for (size_t i = 0; i < font_list->count; i++)
        free(font_list->arr[i].pattern);
    free(font_list->arr);
    font_list->count = 0;
    font_list->arr = NULL;
}
