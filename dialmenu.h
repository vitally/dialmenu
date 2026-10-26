#ifndef DIALMENU_H
#define DIALMENU_H

#include <cairo.h>
#include <glib.h>

typedef enum {
    ACT_NONE = 0,
    ACT_VOLUME,
    ACT_MUTE,
    ACT_SCROLL,
    ACT_ZOOM,
    ACT_UNDO,
    ACT_EXIT,
    ACT_BRIGHTNESS,
    ACT_MEDIA,
    ACT_PLAY,
    ACT_COMMAND,
    ACT_KEY,
} ActionKind;

typedef struct {
    ActionKind kind;
    char *up;
    char *down;
    char *run;
} Action;

typedef struct Item Item;
struct Item {
    char *label;
    char *icon;
    Action rotate;
    Action click;
    gboolean folder;
    Item *children;
    int child_count;
};

typedef struct {
    int menu_step;
    int menu_friction;
    int action_step;
    gboolean invert;
    gboolean haptics;
    Item *items;
    int count;
} Config;

char *config_default_path(void);
Config *config_load(const char *path, GError **error);
void config_free(Config *config);
gboolean config_ensure_file(const char *path, GError **error);
gboolean config_add_item(const char *path, const char *label, const char *icon,
                         const char *rotate, const char *click, const char *folder, GError **error);
gboolean config_add_folder(const char *path, const char *label, const char *parent, GError **error);
gboolean config_add_launch(const char *path, const char *label, const char *icon,
                           const char *command, const char *folder, GError **error);
gboolean config_remove_item(const char *path, const char *label, const char *folder, GError **error);
void config_print(const Config *config);
const char *action_name(const Action *action);

#define DIAL_BUTTON 1
#define DIAL_ROTATE 2

typedef void (*DialEventFn)(int kind, int value, gpointer user);

void dial_device_start(DialEventFn fn, gpointer user);
void dial_device_stop(void);

gboolean actions_init(GError **error);
void actions_run(const Action *action, int direction);

void dial_draw(cairo_t *cr, int width, int height, Item **items, int count, int selected,
               double accent_r, double accent_g, double accent_b);

/* Reads the accent from the applied Omarchy theme. */
gboolean theme_read_accent(double *r, double *g, double *b);

/* One haptic click. No-op when disabled or the dial hidraw is unavailable. */
void haptics_pulse(gboolean enabled);
void haptics_shutdown(void);

#endif
