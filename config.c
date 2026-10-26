#include "dialmenu.h"

#include <errno.h>
#include <json-glib/json-glib.h>
#include <string.h>

static const char *DEFAULT_JSON =
    "{\n"
    "  \"menu_step\": 6,\n"
    "  \"menu_friction\": 120,\n"
    "  \"action_step\": 4,\n"
    "  \"invert\": false,\n"
    "  \"haptics\": true,\n"
    "  \"items\": [\n"
    "    { \"label\": \"Scroll\", \"icon\": \"scroll\", \"rotate\": \"scroll\" },\n"
    "    { \"label\": \"Zoom\", \"icon\": \"zoom\", \"rotate\": \"zoom\" },\n"
    "    { \"label\": \"Exit\", \"icon\": \"exit\", \"rotate\": \"exit\", \"click\": \"exit\" },\n"
    "    { \"label\": \"Volume\", \"icon\": \"volume\", \"rotate\": \"volume\", \"click\": \"mute\" }\n"
    "  ]\n"
    "}\n";

static void action_clear(Action *action) {
    g_free(action->up);
    g_free(action->down);
    g_free(action->run);
    memset(action, 0, sizeof *action);
}

static void item_clear(Item *item) {
    if (!item) {
        return;
    }
    for (int i = 0; i < item->child_count; i++) {
        item_clear(&item->children[i]);
    }
    g_free(item->children);
    g_free(item->label);
    g_free(item->icon);
    action_clear(&item->rotate);
    action_clear(&item->click);
    memset(item, 0, sizeof *item);
}

void config_free(Config *config) {
    if (!config) {
        return;
    }
    for (int i = 0; i < config->count; i++) {
        item_clear(&config->items[i]);
    }
    g_free(config->items);
    g_free(config);
}

char *config_default_path(void) {
    return g_build_filename(g_get_user_config_dir(), "dialmenu", "config.json", NULL);
}

static char *dup_string_member(JsonObject *object, const char *key) {
    if (!json_object_has_member(object, key)) {
        return NULL;
    }
    JsonNode *node = json_object_get_member(object, key);
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) {
        return NULL;
    }
    const char *value = json_node_get_string(node);
    return value ? g_strdup(value) : NULL;
}

static int builtin_kind(const char *name) {
    if (!name || strcmp(name, "none") == 0) return ACT_NONE;
    if (strcmp(name, "volume") == 0) return ACT_VOLUME;
    if (strcmp(name, "mute") == 0) return ACT_MUTE;
    if (strcmp(name, "scroll") == 0) return ACT_SCROLL;
    if (strcmp(name, "zoom") == 0) return ACT_ZOOM;
    if (strcmp(name, "undo") == 0) return ACT_UNDO;
    if (strcmp(name, "exit") == 0) return ACT_EXIT;
    if (strcmp(name, "brightness") == 0) return ACT_BRIGHTNESS;
    if (strcmp(name, "media") == 0) return ACT_MEDIA;
    if (strcmp(name, "play") == 0) return ACT_PLAY;
    return -1;
}

const char *action_name(const Action *action) {
    switch (action->kind) {
    case ACT_NONE: return "none";
    case ACT_VOLUME: return "volume";
    case ACT_MUTE: return "mute";
    case ACT_SCROLL: return "scroll";
    case ACT_ZOOM: return "zoom";
    case ACT_UNDO: return "undo";
    case ACT_EXIT: return "exit";
    case ACT_BRIGHTNESS: return "brightness";
    case ACT_MEDIA: return "media";
    case ACT_PLAY: return "play";
    case ACT_COMMAND: return "command";
    case ACT_KEY: return "key";
    }
    return "none";
}

static gboolean parse_action(JsonNode *node, Action *action, const char *what, GError **error) {
    memset(action, 0, sizeof *action);
    if (!node || json_node_is_null(node)) {
        return TRUE;
    }
    if (JSON_NODE_HOLDS_VALUE(node)) {
        const char *name = json_node_get_string(node);
        int kind = builtin_kind(name);
        if (kind < 0) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                        "%s action \"%s\" is unknown", what, name ? name : "");
            return FALSE;
        }
        action->kind = kind;
        return TRUE;
    }
    if (!JSON_NODE_HOLDS_OBJECT(node)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s action must be a string or object", what);
        return FALSE;
    }
    JsonObject *object = json_node_get_object(node);
    const char *type = dup_string_member(object, "type");
    if (!type) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s action object needs a type", what);
        return FALSE;
    }
    if (strcmp(type, "command") == 0) {
        action->kind = ACT_COMMAND;
    } else if (strcmp(type, "key") == 0) {
        action->kind = ACT_KEY;
    } else {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "%s action type \"%s\" is unknown", what, type);
        g_free((char *)type);
        return FALSE;
    }
    g_free((char *)type);
    action->up = dup_string_member(object, "up");
    action->down = dup_string_member(object, "down");
    action->run = dup_string_member(object, "run");
    if (!action->run) {
        action->run = dup_string_member(object, "key");
    }
    return TRUE;
}

static gboolean parse_items(JsonArray *array, Item **out_items, int *out_count, int depth, gboolean root,
                            const char *where, GError **error);

static gboolean parse_item(JsonObject *object, Item *item, int index, int depth, GError **error) {
    memset(item, 0, sizeof *item);
    item->label = dup_string_member(object, "label");
    if (!item->label || item->label[0] == '\0' || strchr(item->label, '/') != NULL) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "item %d needs a label without \"/\"", index + 1);
        item_clear(item);
        return FALSE;
    }
    if (json_object_has_member(object, "items")) {
        JsonNode *node = json_object_get_member(object, "items");
        if (!JSON_NODE_HOLDS_ARRAY(node)) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "\"%s\" items must be an array", item->label);
            item_clear(item);
            return FALSE;
        }
        item->folder = TRUE;
        if (!parse_items(json_node_get_array(node), &item->children, &item->child_count, depth + 1, FALSE,
                         item->label, error)) {
            item_clear(item);
            return FALSE;
        }
    }
    item->icon = dup_string_member(object, "icon");
    if (!item->icon) {
        item->icon = g_strdup(item->folder ? "folder" : "dot");
    }
    if (json_object_has_member(object, "rotate") &&
        !parse_action(json_object_get_member(object, "rotate"), &item->rotate, item->label, error)) {
        item_clear(item);
        return FALSE;
    }
    if (json_object_has_member(object, "click") &&
        !parse_action(json_object_get_member(object, "click"), &item->click, item->label, error)) {
        item_clear(item);
        return FALSE;
    }
    return TRUE;
}

static gboolean parse_items(JsonArray *array, Item **out_items, int *out_count, int depth, gboolean root,
                            const char *where, GError **error) {
    guint count = json_array_get_length(array);
    int limit = root ? 12 : 11;
    if (depth > 4) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "folders can only be nested 4 deep");
        return FALSE;
    }
    if ((root && count == 0) || count > (guint)limit) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    root ? "%s needs between 1 and 12 items" : "\"%s\" can hold at most 11 items", where);
        return FALSE;
    }
    if (count == 0) {
        *out_items = NULL;
        *out_count = 0;
        return TRUE;
    }
    Item *items = g_new0(Item, count);
    for (guint i = 0; i < count; i++) {
        JsonNode *node = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_OBJECT(node) ||
            !parse_item(json_node_get_object(node), &items[i], (int)i, depth, error)) {
            for (guint j = 0; j < i; j++) {
                item_clear(&items[j]);
            }
            g_free(items);
            return FALSE;
        }
        for (guint j = 0; j < i; j++) {
            if (strcmp(items[j].label, items[i].label) == 0) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST, "\"%s\" is listed twice", items[i].label);
                for (guint k = 0; k <= i; k++) {
                    item_clear(&items[k]);
                }
                g_free(items);
                return FALSE;
            }
        }
    }
    *out_items = items;
    *out_count = (int)count;
    return TRUE;
}

Config *config_load(const char *path, GError **error) {
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, error)) {
        g_object_unref(parser);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s must contain an object", path);
        g_object_unref(parser);
        return NULL;
    }
    JsonObject *object = json_node_get_object(root);
    if (!json_object_has_member(object, "items") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "items"))) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s needs an items array", path);
        g_object_unref(parser);
        return NULL;
    }
    JsonArray *array = json_object_get_array_member(object, "items");
    Config *config = g_new0(Config, 1);
    config->menu_step = 6;
    config->menu_friction = 120;
    config->action_step = 4;
    config->haptics = TRUE;
    if (json_object_has_member(object, "menu_step")) {
        config->menu_step = json_object_get_int_member(object, "menu_step");
    }
    if (json_object_has_member(object, "menu_friction")) {
        config->menu_friction = json_object_get_int_member(object, "menu_friction");
    }
    if (json_object_has_member(object, "action_step")) {
        config->action_step = json_object_get_int_member(object, "action_step");
    }
    if (json_object_has_member(object, "invert")) {
        config->invert = json_object_get_boolean_member(object, "invert");
    }
    if (json_object_has_member(object, "haptics")) {
        config->haptics = json_object_get_boolean_member(object, "haptics");
    }
    if (config->menu_step < 1) config->menu_step = 1;
    if (config->menu_friction < 0) config->menu_friction = 0;
    if (config->action_step < 1) config->action_step = 1;

    if (!parse_items(array, &config->items, &config->count, 0, TRUE, path, error)) {
        config_free(config);
        g_object_unref(parser);
        return NULL;
    }
    g_object_unref(parser);
    return config;
}

static gboolean ensure_parent(const char *path, GError **error) {
    char *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "could not create %s", dir);
        g_free(dir);
        return FALSE;
    }
    g_free(dir);
    return TRUE;
}

gboolean config_ensure_file(const char *path, GError **error) {
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        return TRUE;
    }
    if (!ensure_parent(path, error)) {
        return FALSE;
    }
    return g_file_set_contents(path, DEFAULT_JSON, -1, error);
}

static gboolean rewrite_root(JsonParser *parser, const char *path, GError **error) {
    JsonGenerator *generator = json_generator_new();
    json_generator_set_pretty(generator, TRUE);
    json_generator_set_indent(generator, 2);
    json_generator_set_root(generator, json_parser_get_root(parser));
    gboolean ok = json_generator_to_file(generator, path, error);
    g_object_unref(generator);
    return ok;
}

static JsonParser *load_parser(const char *path, GError **error) {
    if (!config_ensure_file(path, error)) {
        return NULL;
    }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, error)) {
        g_object_unref(parser);
        return NULL;
    }
    return parser;
}

static const char *item_label(JsonObject *item) {
    if (!item || !json_object_has_member(item, "label")) {
        return NULL;
    }
    JsonNode *node = json_object_get_member(item, "label");
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) {
        return NULL;
    }
    return json_node_get_string(node);
}

static gboolean label_ok(const char *label, GError **error) {
    if (!label || label[0] == '\0' || strchr(label, '/') != NULL) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "a label cannot be empty or contain \"/\"");
        return FALSE;
    }
    return TRUE;
}

static JsonArray *resolve_items(JsonObject *root, const char *folder, gboolean *root_level, GError **error) {
    if (!json_object_has_member(root, "items") || !JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "items"))) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "the menu has no items array");
        return NULL;
    }
    JsonArray *items = json_object_get_array_member(root, "items");
    if (root_level) {
        *root_level = TRUE;
    }
    if (!folder || folder[0] == '\0') {
        return items;
    }
    char **parts = g_strsplit(folder, "/", -1);
    for (int p = 0; parts[p]; p++) {
        if (parts[p][0] == '\0') {
            continue;
        }
        JsonObject *match = NULL;
        for (guint i = 0; i < json_array_get_length(items); i++) {
            JsonObject *item = json_array_get_object_element(items, i);
            const char *label = item_label(item);
            if (label && strcmp(label, parts[p]) == 0) {
                match = item;
                break;
            }
        }
        if (!match || !json_object_has_member(match, "items") ||
            !JSON_NODE_HOLDS_ARRAY(json_object_get_member(match, "items"))) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "no folder \"%s\"", parts[p]);
            g_strfreev(parts);
            return NULL;
        }
        items = json_object_get_array_member(match, "items");
        if (root_level) {
            *root_level = FALSE;
        }
    }
    g_strfreev(parts);
    return items;
}

static gboolean label_taken(JsonArray *items, const char *label) {
    for (guint i = 0; i < json_array_get_length(items); i++) {
        const char *existing = item_label(json_array_get_object_element(items, i));
        if (existing && strcmp(existing, label) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean config_add_item(const char *path, const char *label, const char *icon,
                         const char *rotate, const char *click, const char *folder, GError **error) {
    if (!label_ok(label, error)) {
        return FALSE;
    }
    if (builtin_kind(rotate) < 0 || (click && builtin_kind(click) < 0)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "rotate and click must be builtin names (volume, mute, scroll, zoom, undo, exit, brightness, media, play, none)");
        return FALSE;
    }
    JsonParser *parser = load_parser(path, error);
    if (!parser) {
        return FALSE;
    }
    gboolean root_level = TRUE;
    JsonArray *items = resolve_items(json_node_get_object(json_parser_get_root(parser)), folder, &root_level, error);
    if (!items) {
        g_object_unref(parser);
        return FALSE;
    }
    if (label_taken(items, label)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST, "\"%s\" is already in that menu", label);
        g_object_unref(parser);
        return FALSE;
    }
    guint limit = root_level ? 12 : 11;
    if (json_array_get_length(items) >= limit) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    root_level ? "the dial already has 12 items" : "that folder already has 11 items");
        g_object_unref(parser);
        return FALSE;
    }
    JsonObject *item = json_object_new();
    json_object_set_string_member(item, "label", label);
    json_object_set_string_member(item, "icon", icon && icon[0] ? icon : "dot");
    json_object_set_string_member(item, "rotate", rotate);
    if (click && strcmp(click, "none") != 0) {
        json_object_set_string_member(item, "click", click);
    }
    json_array_add_object_element(items, item);
    gboolean ok = rewrite_root(parser, path, error);
    g_object_unref(parser);
    return ok;
}

gboolean config_add_folder(const char *path, const char *label, const char *parent, GError **error) {
    if (!label_ok(label, error)) {
        return FALSE;
    }
    JsonParser *parser = load_parser(path, error);
    if (!parser) {
        return FALSE;
    }
    gboolean root_level = TRUE;
    JsonArray *items = resolve_items(json_node_get_object(json_parser_get_root(parser)), parent, &root_level, error);
    if (!items) {
        g_object_unref(parser);
        return FALSE;
    }
    if (label_taken(items, label)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST, "\"%s\" is already in that menu", label);
        g_object_unref(parser);
        return FALSE;
    }
    guint limit = root_level ? 12 : 11;
    if (json_array_get_length(items) >= limit) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    root_level ? "the dial already has 12 items" : "that folder already has 11 items");
        g_object_unref(parser);
        return FALSE;
    }
    JsonObject *item = json_object_new();
    json_object_set_string_member(item, "label", label);
    json_object_set_string_member(item, "icon", "folder");
    json_object_set_array_member(item, "items", json_array_new());
    json_array_add_object_element(items, item);
    gboolean ok = rewrite_root(parser, path, error);
    g_object_unref(parser);
    return ok;
}

gboolean config_add_launch(const char *path, const char *label, const char *icon, const char *command,
                           const char *folder, GError **error) {
    if (!label_ok(label, error)) {
        return FALSE;
    }
    if (!command || command[0] == '\0') {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "a launch item needs a command");
        return FALSE;
    }
    JsonParser *parser = load_parser(path, error);
    if (!parser) {
        return FALSE;
    }
    gboolean root_level = TRUE;
    JsonArray *items = resolve_items(json_node_get_object(json_parser_get_root(parser)), folder, &root_level, error);
    if (!items) {
        g_object_unref(parser);
        return FALSE;
    }
    if (label_taken(items, label)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST, "\"%s\" is already in that menu", label);
        g_object_unref(parser);
        return FALSE;
    }
    guint limit = root_level ? 12 : 11;
    if (json_array_get_length(items) >= limit) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    root_level ? "the dial already has 12 items" : "that folder already has 11 items");
        g_object_unref(parser);
        return FALSE;
    }
    JsonObject *click = json_object_new();
    json_object_set_string_member(click, "type", "command");
    json_object_set_string_member(click, "run", command);
    JsonObject *item = json_object_new();
    json_object_set_string_member(item, "label", label);
    json_object_set_string_member(item, "icon", icon && icon[0] ? icon : "dot");
    json_object_set_object_member(item, "click", click);
    json_array_add_object_element(items, item);
    gboolean ok = rewrite_root(parser, path, error);
    g_object_unref(parser);
    return ok;
}

gboolean config_remove_item(const char *path, const char *label, const char *folder, GError **error) {
    JsonParser *parser = load_parser(path, error);
    if (!parser) {
        return FALSE;
    }
    gboolean root_level = TRUE;
    JsonArray *items = resolve_items(json_node_get_object(json_parser_get_root(parser)), folder, &root_level, error);
    if (!items) {
        g_object_unref(parser);
        return FALSE;
    }
    int found = -1;
    for (guint i = 0; i < json_array_get_length(items); i++) {
        const char *existing = item_label(json_array_get_object_element(items, i));
        if (existing && strcmp(existing, label) == 0) {
            found = (int)i;
            break;
        }
    }
    if (found < 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "no item labeled \"%s\"", label);
        g_object_unref(parser);
        return FALSE;
    }
    if (root_level && json_array_get_length(items) == 1) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "the dial needs at least one item");
        g_object_unref(parser);
        return FALSE;
    }
    json_array_remove_element(items, found);
    gboolean ok = rewrite_root(parser, path, error);
    g_object_unref(parser);
    return ok;
}

static void print_items(const Item *items, int count, int indent) {
    for (int i = 0; i < count; i++) {
        const Item *item = &items[i];
        g_print("%*s%s  icon=%s", indent, "", item->label, item->icon);
        if (item->folder) {
            g_print("  folder (%d)\n", item->child_count);
            print_items(item->children, item->child_count, indent + 2);
        } else {
            g_print("  rotate=%s  click=%s\n", action_name(&item->rotate), action_name(&item->click));
            if (item->click.kind == ACT_COMMAND && item->click.run) {
                g_print("%*s  %s\n", indent + 2, "", item->click.run);
            }
        }
    }
}

void config_print(const Config *config) {
    g_print("menu_step %d, menu_friction %d, action_step %d%s%s\n", config->menu_step, config->menu_friction,
            config->action_step, config->invert ? ", inverted" : "", config->haptics ? ", haptics" : "");
    print_items(config->items, config->count, 0);
}
