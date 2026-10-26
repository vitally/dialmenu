#include "dialmenu.h"

#include <cairo.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <json-glib/json-glib.h>
#include <string.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *area;
    Config *config;
    char *config_path;
    char *state_path;
    int selected;
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
    GFileMonitor *theme_monitor;
} App;

static char *state_path(void) {
    return g_build_filename(g_get_user_state_dir(), "dialmenu", "selected", NULL);
}

static void save_selected(App *app) {
    if (app->selected < 0 || app->selected >= app->config->count) {
        return;
    }
    char *dir = g_path_get_dirname(app->state_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_file_set_contents(app->state_path, app->config->items[app->selected].label, -1, NULL);
}

static void restore_selected(App *app) {
    app->selected = 0;
    char *label = NULL;
    if (!g_file_get_contents(app->state_path, &label, NULL, NULL)) {
        return;
    }
    g_strchomp(label);
    for (int i = 0; i < app->config->count; i++) {
        if (strcmp(app->config->items[i].label, label) == 0) {
            app->selected = i;
            break;
        }
    }
    g_free(label);
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

static void place_on_focused_monitor(GtkWindow *window) {
    char *cursor_argv[] = {"hyprctl", "cursorpos", NULL};
    char *monitor_argv[] = {"hyprctl", "monitors", "-j", NULL};
    char *cursor = capture(cursor_argv);
    char *json = capture(monitor_argv);
    int cx = 0, cy = 0;
    gboolean have_cursor = cursor && sscanf(cursor, "%d, %d", &cx, &cy) == 2;
    const char *name = NULL;
    JsonParser *parser = json_parser_new();
    if (json && json_parser_load_from_data(parser, json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_ARRAY(root)) {
            JsonArray *monitors = json_node_get_array(root);
            for (guint i = 0; i < json_array_get_length(monitors); i++) {
                JsonObject *monitor = json_array_get_object_element(monitors, i);
                if (!monitor) {
                    continue;
                }
                if (have_cursor) {
                    int x = json_object_get_int_member(monitor, "x");
                    int y = json_object_get_int_member(monitor, "y");
                    int w = json_object_get_int_member(monitor, "width");
                    int h = json_object_get_int_member(monitor, "height");
                    if (cx >= x && cx < x + w && cy >= y && cy < y + h) {
                        name = json_object_get_string_member(monitor, "name");
                        break;
                    }
                } else if (json_object_has_member(monitor, "focused") &&
                           json_object_get_boolean_member(monitor, "focused")) {
                    name = json_object_get_string_member(monitor, "name");
                    break;
                }
            }
        }
    }
    if (name) {
        GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());
        for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
            GdkMonitor *monitor = g_list_model_get_item(monitors, i);
            if (g_strcmp0(gdk_monitor_get_connector(monitor), name) == 0) {
                gtk_layer_set_monitor(window, monitor);
                g_message("menu on %s", name);
            }
            g_object_unref(monitor);
        }
    } else {
        g_warning("could not place the menu on a monitor");
    }
    g_object_unref(parser);
    g_free(json);
    g_free(cursor);
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
    if (app->selected < 0 || app->selected >= app->config->count) {
        return;
    }
    const Item *item = &app->config->items[app->selected];
    actions_run(click ? &item->click : &item->rotate, direction);
}

static void close_menu(App *app) {
    if (app->hide_source) {
        g_source_remove(app->hide_source);
        app->hide_source = 0;
    }
    if (app->depth == 0 && app->moved) {
        Item *item = highlighted(app);
        if (item && !item->folder && !item_is_exit(item) && item->rotate.kind != ACT_NONE) {
            for (int i = 0; i < app->config->count; i++) {
                if (&app->config->items[i] == item) {
                    app->selected = i;
                    save_selected(app);
                    break;
                }
            }
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
    if (app->depth == 0 && item->rotate.kind != ACT_NONE) {
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
            app->depth = 0;
            rebuild_view(app);
            app->highlight = app->selected;
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
    char *label = g_strdup(app->config->items[app->selected].label);
    app->depth = 0;
    config_free(app->config);
    app->config = config;
    app->selected = 0;
    for (int i = 0; i < config->count; i++) {
        if (strcmp(config->items[i].label, label) == 0) {
            app->selected = i;
            break;
        }
    }
    g_free(label);
    rebuild_view(app);
    app->highlight = app->selected;
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
    dial_draw(cr, width, height, app->slots, app->view_count, app->menu_open ? app->highlight : app->selected,
              app->accent_r, app->accent_g, app->accent_b);
}

static void on_theme_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer user) {
    (void)monitor;
    (void)other;
    if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_CREATED &&
        event != G_FILE_MONITOR_EVENT_MOVED && event != G_FILE_MONITOR_EVENT_CHANGED) {
        return;
    }
    char *base = g_file_get_basename(file);
    gboolean ours = g_strcmp0(base, "colors.toml") == 0 || g_strcmp0(base, "theme.name") == 0;
    g_free(base);
    if (ours) {
        load_accent(user);
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
    gtk_window_set_default_size(GTK_WINDOW(app->window), 420, 420);
    gtk_layer_init_for_window(GTK_WINDOW(app->window));
    gtk_layer_set_layer(GTK_WINDOW(app->window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(app->window), "dialmenu");
    gtk_layer_set_exclusive_zone(GTK_WINDOW(app->window), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(app->window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_respect_close(GTK_WINDOW(app->window), FALSE);

    app->area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(app->area), 420);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app->area), 420);
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
        app->depth = 0;
        rebuild_view(app);
        app->highlight = app->selected;
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
        "shortcut inside a folder runs it and closes the wheel.\n"
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
    if (strcmp(cmd, "check") == 0 || strcmp(cmd, "list") == 0) {
        if (!config_ensure_file(path, &error)) {
            g_printerr("%s\n", error->message);
            return 1;
        }
        Config *config = config_load(path, &error);
        if (!config) {
            g_printerr("%s\n", error->message);
            g_free(path);
            return 78;
        }
        if (strcmp(cmd, "list") == 0) {
            g_print("%s\n", path);
        }
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
    return -1;
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
    restore_selected(&app);
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
    char *theme_dir = g_build_filename(g_get_user_state_dir(), "omarchy", "current", "theme", NULL);
    GFile *theme_folder = g_file_new_for_path(theme_dir);
    app.theme_monitor = g_file_monitor_directory(theme_folder, G_FILE_MONITOR_NONE, NULL, NULL);
    g_free(theme_dir);
    g_object_unref(theme_folder);
    if (app.theme_monitor) {
        g_signal_connect(app.theme_monitor, "changed", G_CALLBACK(on_theme_changed), &app);
    }

    GtkApplication *application = gtk_application_new("dev.dialmenu.Menu", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), &app);
    g_signal_connect(application, "shutdown", G_CALLBACK(on_shutdown), &app);
    char *app_argv[] = {argv[0], NULL};
    status = g_application_run(G_APPLICATION(application), 1, app_argv);
    g_object_unref(application);
    if (app.monitor) {
        g_object_unref(app.monitor);
    }
    if (app.theme_monitor) {
        g_object_unref(app.theme_monitor);
    }
    config_free(app.config);
    g_free(app.config_path);
    g_free(app.state_path);
    return status;
}
