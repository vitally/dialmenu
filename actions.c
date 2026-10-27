#include "dialmenu.h"

#include <errno.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static struct libevdev_uinput *uinput;
static gboolean uinput_warned;

static gboolean ensure_uinput(void) {
    if (uinput) {
        return TRUE;
    }
    static gint64 next_try;
    gint64 now = g_get_monotonic_time();
    if (now < next_try) {
        return FALSE;
    }
    next_try = now + 2 * G_TIME_SPAN_SECOND;
    GError *error = NULL;
    if (!actions_init(&error)) {
        if (!uinput_warned) {
            g_warning("%s", error->message);
            uinput_warned = TRUE;
        }
        g_error_free(error);
        return FALSE;
    }
    g_message("opened /dev/uinput");
    return TRUE;
}

static void emit(unsigned int type, unsigned int code, int value) {
    if (!ensure_uinput()) {
        return;
    }
    libevdev_uinput_write_event(uinput, type, code, value);
    libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
}

gboolean actions_init(GError **error) {
    if (uinput) {
        return TRUE;
    }
    struct libevdev *device = libevdev_new();
    libevdev_set_name(device, "dialmenu");
    libevdev_set_id_bustype(device, BUS_VIRTUAL);
    libevdev_set_id_vendor(device, 0x0001);
    libevdev_set_id_product(device, 0x0001);
    libevdev_enable_event_type(device, EV_KEY);
    for (int code = KEY_ESC; code < KEY_MAX; code++) {
        libevdev_enable_event_code(device, EV_KEY, code, NULL);
    }
    libevdev_enable_event_type(device, EV_REL);
    libevdev_enable_event_code(device, EV_REL, REL_WHEEL, NULL);
    libevdev_enable_event_code(device, EV_REL, REL_HWHEEL, NULL);

    int rc = libevdev_uinput_create_from_device(device, LIBEVDEV_UINPUT_OPEN_MANAGED, &uinput);
    libevdev_free(device);
    if (rc < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(-rc),
                    "could not open /dev/uinput (%s). Scroll, zoom, and key actions need it",
                    g_strerror(-rc));
        uinput = NULL;
        return FALSE;
    }
    return TRUE;
}

static int pending_volume;
static guint volume_source;
static int pending_brightness;
static guint brightness_source;
static int pending_media;
static guint media_source;
static void spawn(char *const argv[]);

static gboolean flush_volume(gpointer unused) {
    (void)unused;
    int steps = pending_volume;
    pending_volume = 0;
    volume_source = 0;
    if (steps == 0) {
        return G_SOURCE_REMOVE;
    }
    char amount[16];
    g_snprintf(amount, sizeof amount, "%+d", steps * 5);
    char *argv[] = {"omarchy-audio-output-volume", amount, NULL};
    spawn(argv);
    return G_SOURCE_REMOVE;
}

static void spawn(char *const argv[]) {
    GError *error = NULL;
    if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
        g_warning("%s", error->message);
        g_error_free(error);
    }
}

static void command(const char *line) {
    if (!line || !line[0]) {
        return;
    }
    char *argv[] = {"sh", "-c", (char *)line, NULL};
    spawn(argv);
}

static gboolean flush_brightness(gpointer unused) {
    (void)unused;
    int steps = pending_brightness;
    pending_brightness = 0;
    brightness_source = 0;
    if (steps == 0) {
        return G_SOURCE_REMOVE;
    }
    char amount[16];
    if (steps > 0) {
        g_snprintf(amount, sizeof amount, "+%d%%", steps * 5);
    } else {
        g_snprintf(amount, sizeof amount, "%d%%-", -steps * 5);
    }
    char *argv[] = {"omarchy-brightness-display", amount, NULL};
    spawn(argv);
    return G_SOURCE_REMOVE;
}

static gboolean flush_media(gpointer unused) {
    (void)unused;
    int steps = pending_media;
    pending_media = 0;
    media_source = 0;
    if (steps == 0) {
        return G_SOURCE_REMOVE;
    }
    const char *which = steps > 0 ? "next" : "previous";
    int count = steps > 0 ? steps : -steps;
    if (count > 8) {
        count = 8;
    }
    char script[160];
    g_snprintf(script, sizeof script,
               "n=0; while [ \"$n\" -lt %d ]; do omarchy-shell -q media %s; n=$((n+1)); done", count, which);
    command(script);
    return G_SOURCE_REMOVE;
}

static int token_code(const char *token) {
    if (strcasecmp(token, "ctrl") == 0 || strcasecmp(token, "control") == 0) return KEY_LEFTCTRL;
    if (strcasecmp(token, "shift") == 0) return KEY_LEFTSHIFT;
    if (strcasecmp(token, "alt") == 0) return KEY_LEFTALT;
    if (strcasecmp(token, "super") == 0 || strcasecmp(token, "meta") == 0) return KEY_LEFTMETA;
    if (strcasecmp(token, "enter") == 0 || strcasecmp(token, "return") == 0) return KEY_ENTER;
    if (strcasecmp(token, "space") == 0) return KEY_SPACE;
    if (strcasecmp(token, "tab") == 0) return KEY_TAB;
    if (strcasecmp(token, "esc") == 0 || strcasecmp(token, "escape") == 0) return KEY_ESC;
    if (strcasecmp(token, "left") == 0) return KEY_LEFT;
    if (strcasecmp(token, "right") == 0) return KEY_RIGHT;
    if (strcasecmp(token, "up") == 0) return KEY_UP;
    if (strcasecmp(token, "down") == 0) return KEY_DOWN;
    if (strcasecmp(token, "plus") == 0 || strcmp(token, "+") == 0) return KEY_EQUAL;
    if (strcasecmp(token, "minus") == 0 || strcmp(token, "-") == 0) return KEY_MINUS;
    if (strlen(token) == 1 && g_ascii_isalpha(token[0])) {
        return KEY_A + (g_ascii_tolower(token[0]) - 'a');
    }
    if (strlen(token) == 1 && g_ascii_isdigit(token[0])) {
        return token[0] == '0' ? KEY_0 : KEY_1 + (token[0] - '1');
    }
    return -1;
}

static int parse_chord(const char *spec, int *codes, int max) {
    if (!spec || !spec[0]) {
        return 0;
    }
    char *copy = g_strdup(spec);
    int count = 0;
    char *save = NULL;
    for (char *token = strtok_r(copy, "+", &save); token && count < max; token = strtok_r(NULL, "+", &save)) {
        while (*token == ' ') token++;
        int code = token_code(token);
        if (code < 0) {
            g_warning("unknown key \"%s\" in \"%s\"", token, spec);
            g_free(copy);
            return 0;
        }
        codes[count++] = code;
    }
    g_free(copy);
    return count;
}

static void send_chord(const char *spec) {
    int codes[8];
    int count = parse_chord(spec, codes, 8);
    for (int i = 0; i < count; i++) {
        emit(EV_KEY, codes[i], 1);
    }
    for (int i = count - 1; i >= 0; i--) {
        emit(EV_KEY, codes[i], 0);
    }
}

static void send_ctrl_wheel(int notches) {
    emit(EV_REL, REL_WHEEL, notches);
}

static void send_zoom(int direction) {
    emit(EV_KEY, KEY_LEFTCTRL, 1);
    emit(EV_REL, REL_WHEEL, direction);
    emit(EV_KEY, KEY_LEFTCTRL, 0);
}

static void send_undo(int direction) {
    /* Negative rotation undoes. Positive rotation redoes. */
    if (direction < 0) {
        send_chord("ctrl+z");
    } else if (direction > 0) {
        send_chord("ctrl+shift+z");
    }
}

void actions_run(const Action *action, int direction) {
    if (!action || action->kind == ACT_NONE) {
        return;
    }
    switch (action->kind) {
    case ACT_VOLUME:
        pending_volume += direction >= 0 ? 1 : -1;
        if (volume_source == 0) {
            volume_source = g_timeout_add(60, flush_volume, NULL);
        }
        break;
    case ACT_MUTE: {
        char *argv[] = {"omarchy-audio-output-volume", "mute-toggle", NULL};
        spawn(argv);
        break;
    }
    case ACT_BRIGHTNESS:
        pending_brightness += direction >= 0 ? 1 : -1;
        if (brightness_source == 0) {
            brightness_source = g_timeout_add(60, flush_brightness, NULL);
        }
        break;
    case ACT_MEDIA:
        pending_media += direction >= 0 ? 1 : -1;
        if (media_source == 0) {
            media_source = g_timeout_add(60, flush_media, NULL);
        }
        break;
    case ACT_PLAY: {
        char *argv[] = {"omarchy-shell", "-q", "media", "playPause", NULL};
        spawn(argv);
        break;
    }
    case ACT_SCROLL:
        send_ctrl_wheel(direction >= 0 ? 1 : -1);
        break;
    case ACT_ZOOM:
        send_zoom(direction >= 0 ? 1 : -1);
        break;
    case ACT_UNDO:
        send_undo(direction);
        break;
    case ACT_EXIT:
        break;
    case ACT_COMMAND:
        if (direction > 0) command(action->up);
        else if (direction < 0) command(action->down);
        else command(action->run);
        break;
    case ACT_KEY:
        if (direction > 0) send_chord(action->up);
        else if (direction < 0) send_chord(action->down);
        else send_chord(action->run);
        break;
    case ACT_NONE:
        break;
    }
}
