#include "dialmenu.h"

#include <stdio.h>
#include <string.h>

static gboolean parse_hex(const char *hex, double *r, double *g, double *b) {
    if (!hex || hex[0] != '#') {
        return FALSE;
    }
    unsigned value = 0;
    size_t length = strlen(hex);
    if (length == 7 && sscanf(hex, "#%06x", &value) == 1) {
        *r = ((value >> 16) & 0xff) / 255.0;
        *g = ((value >> 8) & 0xff) / 255.0;
        *b = (value & 0xff) / 255.0;
        return TRUE;
    }
    if (length == 4 && sscanf(hex, "#%03x", &value) == 1) {
        unsigned rr = (value >> 8) & 0xf;
        unsigned gg = (value >> 4) & 0xf;
        unsigned bb = value & 0xf;
        *r = (rr * 17) / 255.0;
        *g = (gg * 17) / 255.0;
        *b = (bb * 17) / 255.0;
        return TRUE;
    }
    return FALSE;
}

static gboolean accent_in_toml(const char *text, double *r, double *g, double *b) {
    char **lines = g_strsplit(text, "\n", 0);
    gboolean found = FALSE;
    for (guint i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (strncmp(line, "accent", 6) != 0) {
            continue;
        }
        if (line[6] != '=' && line[6] != ' ' && line[6] != '\t') {
            continue;
        }
        char *hash = strchr(line, '#');
        if (!hash) {
            continue;
        }
        char hex[8];
        int n = 0;
        for (char *p = hash; *p && *p != '"' && *p != '\'' && !g_ascii_isspace(*p) && n < 7; p++) {
            hex[n++] = *p;
        }
        hex[n] = '\0';
        found = parse_hex(hex, r, g, b);
        break;
    }
    g_strfreev(lines);
    return found;
}

static gboolean accent_in_file(const char *path, double *r, double *g, double *b) {
    char *text = NULL;
    if (!path || !g_file_get_contents(path, &text, NULL, NULL)) {
        return FALSE;
    }
    gboolean found = accent_in_toml(text, r, g, b);
    g_free(text);
    return found;
}

gboolean theme_read_accent(double *r, double *g, double *b) {
    char *applied = g_build_filename(g_get_user_state_dir(), "omarchy", "current", "theme", "colors.toml", NULL);
    if (accent_in_file(applied, r, g, b)) {
        g_free(applied);
        return TRUE;
    }
    g_free(applied);

    char *name_path = g_build_filename(g_get_user_state_dir(), "omarchy", "current", "theme.name", NULL);
    char *slug = NULL;
    if (!g_file_get_contents(name_path, &slug, NULL, NULL)) {
        g_free(name_path);
        return FALSE;
    }
    g_free(name_path);
    g_strstrip(slug);
    if (slug[0] == '\0' || strchr(slug, '/') != NULL) {
        g_free(slug);
        return FALSE;
    }

    char *user = g_build_filename(g_get_user_config_dir(), "omarchy", "themes", slug, "colors.toml", NULL);
    if (accent_in_file(user, r, g, b)) {
        g_free(user);
        g_free(slug);
        return TRUE;
    }
    g_free(user);

    const char *root = g_getenv("OMARCHY_PATH");
    if (!root || !root[0]) {
        root = "/usr/share/omarchy";
    }
    char *stock = g_build_filename(root, "themes", slug, "colors.toml", NULL);
    gboolean found = accent_in_file(stock, r, g, b);
    g_free(stock);
    g_free(slug);
    return found;
}
