#include "dialmenu.h"

#include <cairo.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define MENU_SIZE 420
#define MAX_MONITORS 16

typedef struct {
    GtkWidget *window;
    GtkWidget *area;
    Config *config;
    char *config_path;
    char *state_path;
    Item *held;
    int highlight;
    gboolean menu_open;
    gboolean moved;
    int accum;
    Item *stack[4];
    int depth;
    Item *slots[13];
    int view_count;
    Item back;
    double accent_r;
    double accent_g;
    double accent_b;
    gboolean accent_known;
    guint reload_source;
    guint hide_source;
    GFileMonitor *monitor;
    GFileMonitor *current_monitor;
    GFileMonitor *theme_monitor;
} App;

typedef struct {
    char name[64];
    int x;
    int y;
    int w;
    int h;
    gboolean focused;
} MonitorInfo;

static MonitorInfo monitor_cache[MAX_MONITORS];
static int monitor_cache_count;

static char *state_path(void) {
    return g_build_filename(g_get_user_state_dir(), "dialmenu", "selected", NULL);
}

static gboolean find_path(Item *items, int count, const Item *target, GString *path) {
    for (int i = 0; i < count; i++) {
        if (&items[i] == target) {
            g_string_append(path, items[i].label);
            return TRUE;
        }
        if (!items[i].folder) {
            continue;
        }
        gsize mark = path->len;
        g_string_append(path, items[i].label);
        g_string_append_c(path, '/');
        if (find_path(items[i].children, items[i].child_count, target, path)) {
            return TRUE;
        }
        g_string_truncate(path, mark);
    }
    return FALSE;
}

static char *item_path(const Config *config, const Item *item) {
    GString *path = g_string_new(NULL);
    if (!find_path(config->items, config->count, item, path)) {
        g_string_free(path, TRUE);
        return NULL;
    }
    return g_string_free(path, FALSE);
}

static Item *find_in_items(Item *items, int count, char **parts, int index) {
    while (parts[index] && parts[index][0] == '\0') {
        index++;
    }
    if (!parts[index]) {
        return NULL;
    }
    int next = index + 1;
    while (parts[next] && parts[next][0] == '\0') {
        next++;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(items[i].label, parts[index]) != 0) {
            continue;
        }
        if (!parts[next]) {
            return &items[i];
        }
        if (!items[i].folder) {
            return NULL;
        }
        return find_in_items(items[i].children, items[i].child_count, parts, next);
    }
    return NULL;
}

static Item *find_item_by_path(const Config *config, const char *path) {
    char **parts = g_strsplit(path, "/", -1);
    Item *found = find_in_items(config->items, config->count, parts, 0);
    g_strfreev(parts);
    return found;
}

static gboolean item_contains(const Item *item, const Item *target) {
    if (item == target) {
        return TRUE;
    }
    if (!item->folder) {
        return FALSE;
    }
    for (int i = 0; i < item->child_count; i++) {
        if (item_contains(&item->children[i], target)) {
            return TRUE;
        }
    }
    return FALSE;
}

static int held_root_index(App *app) {
    if (!app->held) {
        return 0;
    }
    for (int i = 0; i < app->config->count; i++) {
        if (item_contains(&app->config->items[i], app->held)) {
            return i;
        }
    }
    return 0;
}

static void rebuild_view(App *app);

static gboolean ancestor_chain(Item *items, int count, const Item *target, Item **chain, int *depth) {
    for (int i = 0; i < count; i++) {
        if (&items[i] == target) {
            return TRUE;
        }
        if (!items[i].folder || *depth >= 4) {
            continue;
        }
        chain[(*depth)++] = &items[i];
        if (ancestor_chain(items[i].children, items[i].child_count, target, chain, depth)) {
            return TRUE;
        }
        (*depth)--;
    }
    return FALSE;
}

static void show_held(App *app) {
    app->depth = 0;
    Item *chain[4];
    int depth = 0;
    if (app->held && ancestor_chain(app->config->items, app->config->count, app->held, chain, &depth)) {
        for (int i = 0; i < depth; i++) {
            app->stack[i] = chain[i];
        }
        app->depth = depth;
    }
    rebuild_view(app);
    app->highlight = 0;
    for (int i = 0; i < app->view_count; i++) {
        if (app->slots[i] == app->held) {
            app->highlight = i;
            break;
        }
    }
}

static void save_held(App *app) {
    if (!app->held) {
        return;
    }
    char *path = item_path(app->config, app->held);
    if (!path) {
        return;
    }
    char *dir = g_path_get_dirname(app->state_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_file_set_contents(app->state_path, path, -1, NULL);
    g_free(path);
}

static void restore_held(App *app) {
    app->held = app->config->count > 0 ? &app->config->items[0] : NULL;
    char *saved = NULL;
    if (!g_file_get_contents(app->state_path, &saved, NULL, NULL)) {
        return;
    }
    g_strchomp(saved);
    Item *found = find_item_by_path(app->config, saved);
    g_free(saved);
    if (found && !found->folder && found->rotate.kind != ACT_NONE && found->rotate.kind != ACT_EXIT &&
        found->click.kind != ACT_EXIT) {
        app->held = found;
    }
}

static char *capture(char **argv) {
    char *output = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &output, NULL,
                      NULL, NULL)) {
        g_free(output);
        return NULL;
    }
    return output;
}

static char *hypr_socket_path(void) {
    const char *runtime = g_get_user_runtime_dir();
    const char *signature = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature && signature[0]) {
        return g_build_filename(runtime, "hypr", signature, ".socket.sock", NULL);
    }
    char *dir_path = g_build_filename(runtime, "hypr", NULL);
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    char *found = NULL;
    int matches = 0;
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            char *candidate = g_build_filename(dir_path, name, ".socket.sock", NULL);
            if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
                matches++;
                g_free(found);
                found = candidate;
            } else {
                g_free(candidate);
            }
        }
        g_dir_close(dir);
    }
    g_free(dir_path);
    if (matches != 1) {
        g_free(found);
        return NULL;
    }
    return found;
}

static char *hypr_request(const char *request) {
    char *path = hypr_socket_path();
    if (!path) {
        return NULL;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g_free(path);
        return NULL;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof address.sun_path) {
        close(fd);
        g_free(path);
        return NULL;
    }
    memcpy(address.sun_path, path, strlen(path) + 1);
    g_free(path);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        close(fd);
        return NULL;
    }
    size_t length = strlen(request);
    size_t sent = 0;
    while (sent < length) {
        ssize_t wrote = send(fd, request + sent, length - sent, MSG_NOSIGNAL);
        if (wrote < 0) {
            close(fd);
            return NULL;
        }
        sent += (size_t)wrote;
    }
    GString *body = g_string_new(NULL);
    char buffer[4096];
    while (body->len < 1024 * 1024) {
        ssize_t got = recv(fd, buffer, sizeof buffer, 0);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        g_string_append_len(body, buffer, got);
    }
    close(fd);
    if (body->len == 0) {
        g_string_free(body, TRUE);
        return NULL;
    }
    return g_string_free(body, FALSE);
}

static char *hypr_query(const char *command) {
    char *request = g_strdup_printf("j/%s", command);
    char *body = hypr_request(request);
    g_free(request);
    if (body) {
        return body;
    }
    char *argv[] = {"hyprctl", "-j", (char *)command, NULL};
    return capture(argv);
}

static gboolean parse_cursor(const char *json, int *x, int *y) {
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *object = json_node_get_object(root);
            if (json_object_has_member(object, "x") && json_object_has_member(object, "y")) {
                *x = json_object_get_int_member(object, "x");
                *y = json_object_get_int_member(object, "y");
                g_object_unref(parser);
                return TRUE;
            }
        }
    }
    g_object_unref(parser);
    return sscanf(json, "%d, %d", x, y) == 2 || sscanf(json, "%d,%d", x, y) == 2;
}

static gboolean load_monitors(const char *json) {
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, NULL)) {
        g_object_unref(parser);
        return FALSE;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        g_object_unref(parser);
        return FALSE;
    }
    monitor_cache_count = 0;
    JsonArray *monitors = json_node_get_array(root);
    for (guint i = 0; i < json_array_get_length(monitors) && monitor_cache_count < MAX_MONITORS; i++) {
        JsonObject *monitor = json_array_get_object_element(monitors, i);
        if (!monitor) {
            continue;
        }
        if (json_object_has_member(monitor, "disabled") && json_object_get_boolean_member(monitor, "disabled")) {
            continue;
        }
        const char *name = json_object_get_string_member(monitor, "name");
        if (!name || !name[0]) {
            continue;
        }
        double scale = 1;
        if (json_object_has_member(monitor, "scale")) {
            scale = json_object_get_double_member(monitor, "scale");
        }
        if (scale < 0.5) {
            scale = 1;
        }
        MonitorInfo *slot = &monitor_cache[monitor_cache_count++];
        g_strlcpy(slot->name, name, sizeof slot->name);
        slot->x = json_object_get_int_member(monitor, "x");
        slot->y = json_object_get_int_member(monitor, "y");
        slot->w = (int)(json_object_get_int_member(monitor, "width") / scale + 0.5);
        slot->h = (int)(json_object_get_int_member(monitor, "height") / scale + 0.5);
        slot->focused = json_object_has_member(monitor, "focused") && json_object_get_boolean_member(monitor, "focused");
    }
    g_object_unref(parser);
    return monitor_cache_count > 0;
}

static int monitor_at(int x, int y) {
    for (int i = 0; i < monitor_cache_count; i++) {
        MonitorInfo *monitor = &monitor_cache[i];
        if (x >= monitor->x && x < monitor->x + monitor->w && y >= monitor->y && y < monitor->y + monitor->h) {
            return i;
        }
    }
    return -1;
}

static void apply_monitor(GtkWindow *window, const MonitorInfo *monitor, gboolean have_cursor, int cx, int cy) {
    GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());
    gboolean matched = FALSE;
    for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
        GdkMonitor *gdk_monitor = g_list_model_get_item(monitors, i);
        if (g_strcmp0(gdk_monitor_get_connector(gdk_monitor), monitor->name) == 0) {
            gtk_layer_set_monitor(window, gdk_monitor);
            matched = TRUE;
        }
        g_object_unref(gdk_monitor);
        if (matched) {
            break;
        }
    }
    if (!matched) {
        g_warning("could not place the menu on %s", monitor->name);
        return;
    }
    int left = 0;
    int top = 0;
    if (have_cursor) {
        left = cx - monitor->x - MENU_SIZE / 2;
        top = cy - monitor->y - MENU_SIZE / 2;
    } else if (monitor->w > MENU_SIZE) {
        left = (monitor->w - MENU_SIZE) / 2;
        top = (monitor->h - MENU_SIZE) / 2;
    }
    if (monitor->w > MENU_SIZE) {
        if (left < 0) {
            left = 0;
        }
        if (left > monitor->w - MENU_SIZE) {
            left = monitor->w - MENU_SIZE;
        }
    } else {
        left = 0;
    }
    if (monitor->h > MENU_SIZE) {
        if (top < 0) {
            top = 0;
        }
        if (top > monitor->h - MENU_SIZE) {
            top = monitor->h - MENU_SIZE;
        }
    } else {
        top = 0;
    }
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, left);
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, top);
    g_message("menu on %s", monitor->name);
}

static void place_on_focused_monitor(GtkWindow *window) {
    char *cursor_body = hypr_query("cursorpos");
    int cx = 0;
    int cy = 0;
    gboolean have_cursor = cursor_body && parse_cursor(cursor_body, &cx, &cy);
    g_free(cursor_body);

    int index = have_cursor ? monitor_at(cx, cy) : -1;
    if (index < 0 && (have_cursor || monitor_cache_count == 0)) {
        char *monitors_body = hypr_query("monitors");
        if (monitors_body && load_monitors(monitors_body) && have_cursor) {
            index = monitor_at(cx, cy);
        }
        g_free(monitors_body);
    }
    if (index < 0) {
        for (int i = 0; i < monitor_cache_count; i++) {
            if (monitor_cache[i].focused) {
                index = i;
                have_cursor = FALSE;
                break;
            }
        }
    }
    if (index < 0) {
        g_warning("could not place the menu on a monitor");
        return;
    }
    apply_monitor(window, &monitor_cache[index], have_cursor, cx, cy);
}

static void load_accent(App *app) {
    double r = 0.86, g = 0.86, b = 0.86;
    gboolean found = theme_read_accent(&r, &g, &b);
    if (!found && !app->accent_known) {
        g_warning("no theme accent found; the highlight is neutral until a theme is applied");
    }
    gboolean changed = !app->accent_known || app->accent_r != r || app->accent_g != g || app->accent_b != b;
    app->accent_r = r;
    app->accent_g = g;
    app->accent_b = b;
    app->accent_known = TRUE;
    if (changed && found) {
        g_message("highlight follows theme accent #%02x%02x%02x", (int)(r * 255 + 0.5), (int)(g * 255 + 0.5),
                  (int)(b * 255 + 0.5));
    }
    if (app->menu_open && app->area) {
        gtk_widget_queue_draw(app->area);
    }
}

static void show_menu(App *app, gboolean open) {
    app->menu_open = open;
    if (open) {
        load_accent(app);
        place_on_focused_monitor(GTK_WINDOW(app->window));
        gtk_widget_queue_draw(app->area);
    }
    gtk_widget_set_visible(app->window, open);
}

static gboolean item_is_exit(const Item *item) {
    return item->rotate.kind == ACT_EXIT || item->click.kind == ACT_EXIT;
}

static gboolean click_exits(const Item *item) {
    if (item->click.kind == ACT_EXIT) {
        return TRUE;
    }
    return item->click.kind == ACT_NONE && item->rotate.kind == ACT_EXIT;
}

static gboolean hide_menu(gpointer user);

static void rebuild_view(App *app) {
    app->view_count = 0;
    if (app->depth <= 0) {
        app->depth = 0;
        for (int i = 0; i < app->config->count && i < 12; i++) {
            app->slots[i] = &app->config->items[i];
        }
        app->view_count = app->config->count;
        return;
    }
    Item *folder = app->stack[app->depth - 1];
    memset(&app->back, 0, sizeof app->back);
    app->back.label = "Back";
    app->back.icon = "back";
    app->slots[0] = &app->back;
    int count = folder->child_count;
    if (count > 11) {
        count = 11;
    }
    for (int i = 0; i < count; i++) {
        app->slots[i + 1] = &folder->children[i];
    }
    app->view_count = count + 1;
}

static Item *highlighted(App *app) {
    if (app->highlight < 0 || app->highlight >= app->view_count) {
        return NULL;
    }
    return app->slots[app->highlight];
}

static void hold_menu(App *app) {
    if (app->hide_source) {
        g_source_remove(app->hide_source);
    }
    app->hide_source = g_timeout_add(app->depth > 0 ? 8000 : 1400, hide_menu, app);
}

static void enter_folder(App *app, Item *folder) {
    if (app->depth >= 4) {
        return;
    }
    app->stack[app->depth++] = folder;
    rebuild_view(app);
    app->highlight = app->view_count > 1 ? 1 : 0;
    app->moved = FALSE;
    app->accum = 0;
    haptics_pulse(app->config->haptics);
    hold_menu(app);
    gtk_widget_queue_draw(app->area);
}

static void leave_folder(App *app) {
    if (app->depth <= 0) {
        return;
    }
    Item *leaving = app->stack[app->depth - 1];
    app->depth--;
    rebuild_view(app);
    app->highlight = 0;
    for (int i = 0; i < app->view_count; i++) {
        if (app->slots[i] == leaving) {
            app->highlight = i;
            break;
        }
    }
    app->moved = FALSE;
    app->accum = 0;
    haptics_pulse(app->config->haptics);
    hold_menu(app);
    gtk_widget_queue_draw(app->area);
}

static void step_selection(App *app, int direction) {
    int count = app->view_count;
    if (count < 1) {
        return;
    }
    app->highlight = (app->highlight + direction + count) % count;
    app->moved = TRUE;
    haptics_pulse(app->config->haptics);
    gtk_widget_queue_draw(app->area);
}

static void run_selected(App *app, int direction, gboolean click) {
    if (!app->held) {
        return;
    }
    actions_run(click ? &app->held->click : &app->held->rotate, direction);
}

static void close_menu(App *app) {
    if (app->hide_source) {
        g_source_remove(app->hide_source);
        app->hide_source = 0;
    }
    if (app->moved) {
        Item *item = highlighted(app);
        if (item && item != &app->back && !item->folder && !item_is_exit(item) && item->rotate.kind != ACT_NONE) {
            app->held = item;
            save_held(app);
        }
    }
    app->depth = 0;
    app->accum = 0;
    app->moved = FALSE;
    show_menu(app, FALSE);
}

static gboolean hide_menu(gpointer user) {
    App *app = user;
    app->hide_source = 0;
    close_menu(app);
    return G_SOURCE_REMOVE;
}

static void activate_click(App *app) {
    Item *item = highlighted(app);
    if (!item) {
        return;
    }
    if (item == &app->back) {
        leave_folder(app);
        return;
    }
    if (item->folder) {
        enter_folder(app, item);
        return;
    }
    if (click_exits(item)) {
        close_menu(app);
        return;
    }
    actions_run(&item->click, 0);
    if (item->click.kind == ACT_COMMAND) {
        close_menu(app);
    } else {
        hold_menu(app);
    }
}

static void commit_highlight(App *app) {
    Item *item = highlighted(app);
    if (!item) {
        close_menu(app);
        return;
    }
    if (item == &app->back) {
        leave_folder(app);
        return;
    }
    if (item->folder) {
        enter_folder(app, item);
        return;
    }
    if (item_is_exit(item)) {
        close_menu(app);
        return;
    }
    if (item->rotate.kind != ACT_NONE) {
        close_menu(app);
        return;
    }
    if (item->click.kind != ACT_NONE) {
        actions_run(&item->click, 0);
    }
    close_menu(app);
}

static void on_dial(int kind, int value, gpointer user) {
    App *app = user;
    if (app->config->invert) {
        if (kind == DIAL_ROTATE) value = -value;
    }
    if (kind == DIAL_BUTTON && value == 1) {
        g_message("dial pressed");
        if (app->hide_source) {
            g_source_remove(app->hide_source);
            app->hide_source = 0;
        }
        if (!app->menu_open) {
            show_held(app);
            app->moved = FALSE;
            app->accum = 0;
            show_menu(app, TRUE);
            haptics_pulse(app->config->haptics);
        }
        return;
    }
    if (kind == DIAL_BUTTON && value == 0) {
        if (!app->menu_open) {
            return;
        }
        if (app->moved) {
            commit_highlight(app);
        } else {
            activate_click(app);
        }
        return;
    }
    if (kind != DIAL_ROTATE || value == 0) {
        return;
    }
    app->accum += value;
    int step = app->menu_open ? app->config->menu_step + app->config->menu_friction : app->config->action_step;
    while (app->accum >= step) {
        app->accum -= step;
        if (app->menu_open) {
            step_selection(app, +1);
            hold_menu(app);
        } else {
            run_selected(app, +1, FALSE);
        }
    }
    while (app->accum <= -step) {
        app->accum += step;
        if (app->menu_open) {
            step_selection(app, -1);
            hold_menu(app);
        } else {
            run_selected(app, -1, FALSE);
        }
    }
}

static void reload_config(App *app) {
    GError *error = NULL;
    Config *config = config_load(app->config_path, &error);
    if (!config) {
        g_warning("keeping the current menu: %s", error->message);
        g_error_free(error);
        return;
    }
    char *saved = app->held ? item_path(app->config, app->held) : NULL;
    app->held = NULL;
    app->depth = 0;
    config_free(app->config);
    app->config = config;
    if (saved) {
        app->held = find_item_by_path(config, saved);
    }
    g_free(saved);
    if (!app->held && config->count > 0) {
        app->held = &config->items[0];
    }
    rebuild_view(app);
    app->highlight = held_root_index(app);
    load_accent(app);
    g_message("reloaded %s (%d items, menu friction %d)", app->config_path, config->count, config->menu_friction);
    if (app->menu_open) {
        gtk_widget_queue_draw(app->area);
    }
}

static gboolean reload_later(gpointer user) {
    App *app = user;
    app->reload_source = 0;
    reload_config(app);
    return G_SOURCE_REMOVE;
}

static void on_config_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
                              gpointer user) {
    (void)monitor;
    (void)other;
    if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_CREATED &&
        event != G_FILE_MONITOR_EVENT_MOVED) {
        return;
    }
    char *base = g_file_get_basename(file);
    gboolean ours = g_strcmp0(base, "config.json") == 0;
    g_free(base);
    if (!ours) {
        return;
    }
    App *app = user;
    if (app->reload_source) {
        g_source_remove(app->reload_source);
    }
    app->reload_source = g_timeout_add(150, reload_later, app);
}

static void pass_through(GtkWidget *widget) {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (!surface) {
        return;
    }
    cairo_region_t *empty = cairo_region_create();
    gdk_surface_set_input_region(surface, empty);
    cairo_region_destroy(empty);
}

static void draw_menu(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user) {
    (void)area;
    App *app = user;
    dial_draw(cr, width, height, app->slots, app->view_count, app->menu_open ? app->highlight : held_root_index(app),
              app->accent_r, app->accent_g, app->accent_b);
}

static gboolean theme_event_interesting(GFileMonitorEvent event) {
    return event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || event == G_FILE_MONITOR_EVENT_CREATED ||
           event == G_FILE_MONITOR_EVENT_MOVED || event == G_FILE_MONITOR_EVENT_CHANGED ||
           event == G_FILE_MONITOR_EVENT_DELETED;
}

static void arm_theme_monitor(App *app);

static void on_theme_file_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
                                  gpointer user) {
    (void)monitor;
    (void)other;
    if (!theme_event_interesting(event)) {
        return;
    }
    char *base = g_file_get_basename(file);
    if (g_strcmp0(base, "colors.toml") == 0) {
        load_accent(user);
    }
    g_free(base);
}

static void on_current_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
                               gpointer user) {
    (void)monitor;
    (void)other;
    if (!theme_event_interesting(event)) {
        return;
    }
    char *base = g_file_get_basename(file);
    gboolean theme_dir = g_strcmp0(base, "theme") == 0;
    gboolean theme_name = g_strcmp0(base, "theme.name") == 0;
    g_free(base);
    if (!theme_dir && !theme_name) {
        return;
    }
    if (theme_dir) {
        arm_theme_monitor(user);
    }
    load_accent(user);
}

static void arm_theme_monitor(App *app) {
    if (app->theme_monitor) {
        g_object_unref(app->theme_monitor);
        app->theme_monitor = NULL;
    }
    char *theme_dir = g_build_filename(g_get_user_state_dir(), "omarchy", "current", "theme", NULL);
    GFile *folder = g_file_new_for_path(theme_dir);
    app->theme_monitor = g_file_monitor_directory(folder, G_FILE_MONITOR_NONE, NULL, NULL);
    g_free(theme_dir);
    g_object_unref(folder);
    if (app->theme_monitor) {
        g_signal_connect(app->theme_monitor, "changed", G_CALLBACK(on_theme_file_changed), app);
    }
}

static void setup_window(App *app, GtkApplication *application) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window.dialmenu, window.dialmenu * { background-color: transparent; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    app->window = gtk_application_window_new(application);
    gtk_widget_add_css_class(app->window, "dialmenu");
    gtk_window_set_decorated(GTK_WINDOW(app->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(app->window), MENU_SIZE, MENU_SIZE);
    gtk_layer_init_for_window(GTK_WINDOW(app->window));
    gtk_layer_set_layer(GTK_WINDOW(app->window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(app->window), "dialmenu");
    gtk_layer_set_anchor(GTK_WINDOW(app->window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(app->window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(app->window), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(app->window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_respect_close(GTK_WINDOW(app->window), FALSE);

    app->area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(app->area), MENU_SIZE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app->area), MENU_SIZE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->area), draw_menu, app, NULL);
    gtk_window_set_child(GTK_WINDOW(app->window), app->area);
    g_signal_connect(app->window, "realize", G_CALLBACK(pass_through), NULL);
    g_signal_connect(app->window, "map", G_CALLBACK(pass_through), NULL);
}

static void on_shutdown(GApplication *application, gpointer user) {
    (void)application;
    App *app = user;
    dial_device_stop();
    if (app->reload_source) {
        g_source_remove(app->reload_source);
    }
    if (app->hide_source) {
        g_source_remove(app->hide_source);
    }
    haptics_shutdown();
}

static void on_activate(GApplication *application, gpointer user) {
    if (!gtk_layer_is_supported()) {
        g_printerr("this compositor has no layer shell\n");
        g_application_quit(application);
        return;
    }
    App *app = user;
    g_application_hold(application);
    setup_window(app, GTK_APPLICATION(application));
    if (g_getenv("DIALMENU_PREVIEW")) {
        show_held(app);
        show_menu(app, TRUE);
        return;
    }
    dial_device_start(on_dial, app);
}

static int print_usage(void) {
    char *path = config_default_path();
    g_printerr(
        "Surface Dial menu for Hyprland.\n"
        "\n"
        "Hold or click the dial to open the wheel. Turn to move the highlight,\n"
        "then release to keep that tool. Exit closes the wheel. A click without\n"
        "turning runs the tool's click action. Turning while the wheel is closed\n"
        "runs the selected tool.\n"
        "\n"
        "A folder opens another wheel. Back is the top wedge. Releasing on a\n"
        "tool keeps that tool, including one inside a folder. A launch item\n"
        "runs its command and closes the wheel.\n"
        "\n"
        "  dialmenu                         run in the background\n"
        "  dialmenu list                    show the current items\n"
        "  dialmenu add LABEL ICON ROTATE [CLICK] [FOLDER]\n"
        "  dialmenu folder LABEL [PARENT]\n"
        "  dialmenu launch LABEL ICON COMMAND [FOLDER]\n"
        "  dialmenu remove LABEL [FOLDER]\n"
        "  dialmenu check [FILE]\n"
        "\n"
        "ROTATE and CLICK are volume, mute, scroll, zoom, undo, exit, brightness,\n"
        "media, play, or none. ICON is volume, scroll, zoom, undo, exit, brightness,\n"
        "media, or any short text.\n"
        "\n"
        "The menu is %s\n"
        "Edit that file to add a command or a key. Saving it reloads the wheel.\n"
        "\n"
        "  \"rotate\": { \"type\": \"command\", \"up\": \"notify-send louder\", \"down\": \"notify-send quieter\" }\n"
        "  \"click\":  { \"type\": \"key\", \"run\": \"ctrl+z\" }\n"
        "\n"
        "  dialmenu folder Apps\n"
        "  dialmenu launch Browser B \"omarchy launch browser\" Apps\n"
        "\n"
        "The highlight moves after menu_step + menu_friction dial units. Raise\n"
        "menu_friction when the wheel jumps past the item you want. 0 is the old\n"
        "sensitivity. action_step is how much rotation runs the tool once, and it\n"
        "ignores menu_friction. Set invert to true if the direction is backwards.\n"
        "Set haptics to false to stop the dial clicking on each item.\n",
        path);
    g_free(path);
    return 2;
}

static int command(int argc, char **argv) {
    if (argc < 2) {
        return -1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "--preview") == 0) {
        g_setenv("DIALMENU_PREVIEW", "1", TRUE);
        return -1;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        return print_usage();
    }
    char *path = (argc >= 3 && strcmp(cmd, "check") == 0) ? g_strdup(argv[2]) : config_default_path();
    GError *error = NULL;
    if (strcmp(cmd, "check") == 0) {
        Config *config = config_load(path, &error);
        if (!config) {
            g_printerr("%s\n", error->message);
            g_error_free(error);
            g_free(path);
            return 78;
        }
        config_print(config);
        config_free(config);
        g_free(path);
        return 0;
    }
    if (strcmp(cmd, "list") == 0) {
        if (!config_ensure_file(path, &error)) {
            g_printerr("%s\n", error->message);
            g_error_free(error);
            g_free(path);
            return 1;
        }
        Config *config = config_load(path, &error);
        if (!config) {
            g_printerr("%s\n", error->message);
            g_error_free(error);
            g_free(path);
            return 78;
        }
        g_print("%s\n", path);
        config_print(config);
        config_free(config);
        g_free(path);
        return 0;
    }
    if (strcmp(cmd, "add") == 0) {
        if (argc < 5) {
            g_printerr("usage: dialmenu add LABEL ICON ROTATE [CLICK] [FOLDER]\n");
            g_free(path);
            return 2;
        }
        const char *click = argc >= 6 ? argv[5] : "none";
        const char *folder = argc >= 7 ? argv[6] : NULL;
        if (!config_add_item(path, argv[2], argv[3], argv[4], click, folder, &error)) {
            g_printerr("%s\n", error->message);
            g_free(path);
            return 1;
        }
        g_print("added %s to %s\n", argv[2], path);
        g_free(path);
        return 0;
    }
    if (strcmp(cmd, "folder") == 0) {
        if (argc < 3) {
            g_printerr("usage: dialmenu folder LABEL [PARENT]\n");
            g_free(path);
            return 2;
        }
        const char *parent = argc >= 4 ? argv[3] : NULL;
        if (!config_add_folder(path, argv[2], parent, &error)) {
            g_printerr("%s\n", error->message);
            g_free(path);
            return 1;
        }
        g_print("added folder %s to %s\n", argv[2], path);
        g_free(path);
        return 0;
    }
    if (strcmp(cmd, "launch") == 0) {
        if (argc < 5) {
            g_printerr("usage: dialmenu launch LABEL ICON COMMAND [FOLDER]\n");
            g_free(path);
            return 2;
        }
        const char *folder = argc >= 6 ? argv[5] : NULL;
        if (!config_add_launch(path, argv[2], argv[3], argv[4], folder, &error)) {
            g_printerr("%s\n", error->message);
            g_free(path);
            return 1;
        }
        g_print("added %s to %s\n", argv[2], path);
        g_free(path);
        return 0;
    }
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) {
            g_printerr("usage: dialmenu remove LABEL [FOLDER]\n");
            g_free(path);
            return 2;
        }
        const char *folder = argc >= 4 ? argv[3] : NULL;
        if (!config_remove_item(path, argv[2], folder, &error)) {
            g_printerr("%s\n", error->message);
            g_free(path);
            return 1;
        }
        g_print("removed %s from %s\n", argv[2], path);
        g_free(path);
        return 0;
    }
    g_free(path);
    return print_usage();
}

int main(int argc, char **argv) {
    int status = command(argc, argv);
    if (status >= 0) {
        return status;
    }

    App app = {0};
    app.config_path = config_default_path();
    app.state_path = state_path();
    GError *error = NULL;
    if (!config_ensure_file(app.config_path, &error) || !(app.config = config_load(app.config_path, &error))) {
        g_printerr("%s\n", error->message);
        return 78;
    }
    restore_held(&app);
    load_accent(&app);

    GError *input_error = NULL;
    if (!actions_init(&input_error)) {
        g_warning("%s", input_error->message);
        g_error_free(input_error);
    }

    char *dir = g_path_get_dirname(app.config_path);
    GFile *folder = g_file_new_for_path(dir);
    app.monitor = g_file_monitor_directory(folder, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    g_free(dir);
    g_object_unref(folder);
    if (app.monitor) {
        g_signal_connect(app.monitor, "changed", G_CALLBACK(on_config_changed), &app);
    }
    char *current_dir = g_build_filename(g_get_user_state_dir(), "omarchy", "current", NULL);
    GFile *current_folder = g_file_new_for_path(current_dir);
    app.current_monitor = g_file_monitor_directory(current_folder, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    g_free(current_dir);
    g_object_unref(current_folder);
    if (app.current_monitor) {
        g_signal_connect(app.current_monitor, "changed", G_CALLBACK(on_current_changed), &app);
    }
    arm_theme_monitor(&app);

    GtkApplication *application = gtk_application_new("dev.dialmenu.Menu", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), &app);
    g_signal_connect(application, "shutdown", G_CALLBACK(on_shutdown), &app);
    char *app_argv[] = {argv[0], NULL};
    status = g_application_run(G_APPLICATION(application), 1, app_argv);
    g_object_unref(application);
    if (app.monitor) {
        g_object_unref(app.monitor);
    }
    if (app.current_monitor) {
        g_object_unref(app.current_monitor);
    }
    if (app.theme_monitor) {
        g_object_unref(app.theme_monitor);
    }
    config_free(app.config);
    g_free(app.config_path);
    g_free(app.state_path);
    return status;
}
