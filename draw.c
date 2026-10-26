#include "dialmenu.h"

#include <cairo.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

static void stroke_icon(cairo_t *cr) {
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_set_line_width(cr, 1.7);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
}

static void icon_volume(cairo_t *cr, double x, double y) {
    stroke_icon(cr);
    cairo_move_to(cr, x - 8, y - 3);
    cairo_line_to(cr, x - 4, y - 3);
    cairo_line_to(cr, x + 1, y - 8);
    cairo_line_to(cr, x + 1, y + 8);
    cairo_line_to(cr, x - 4, y + 3);
    cairo_line_to(cr, x - 8, y + 3);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + 2, y, 6, -0.8, 0.8);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + 2, y, 10, -0.7, 0.7);
    cairo_stroke(cr);
}

static void icon_scroll(cairo_t *cr, double x, double y) {
    stroke_icon(cr);
    cairo_move_to(cr, x - 6, y - 4);
    cairo_line_to(cr, x, y - 10);
    cairo_line_to(cr, x + 6, y - 4);
    cairo_move_to(cr, x - 6, y + 4);
    cairo_line_to(cr, x, y + 10);
    cairo_line_to(cr, x + 6, y + 4);
    cairo_stroke(cr);
}

static void icon_zoom(cairo_t *cr, double x, double y) {
    stroke_icon(cr);
    cairo_arc(cr, x - 2, y - 2, 6.5, 0, 2 * G_PI);
    cairo_move_to(cr, x + 3, y + 3);
    cairo_line_to(cr, x + 9, y + 9);
    cairo_stroke(cr);
}

static void icon_folder(cairo_t *cr, double x, double y) {
    stroke_icon(cr);
    cairo_move_to(cr, x - 9, y - 2);
    cairo_line_to(cr, x - 9, y - 6);
    cairo_line_to(cr, x - 2, y - 6);
    cairo_line_to(cr, x, y - 3);
    cairo_line_to(cr, x + 9, y - 3);
    cairo_line_to(cr, x + 9, y + 7);
    cairo_line_to(cr, x - 9, y + 7);
    cairo_close_path(cr);
    cairo_stroke(cr);
}

static void icon_back(cairo_t *cr, double x, double y) {
    stroke_icon(cr);
    cairo_move_to(cr, x + 6, y - 6);
    cairo_line_to(cr, x - 4, y);
    cairo_line_to(cr, x + 6, y + 6);
    cairo_move_to(cr, x - 4, y);
    cairo_line_to(cr, x + 8, y);
    cairo_stroke(cr);
}

static void icon_exit(cairo_t *cr, double x, double y) {
    stroke_icon(cr);
    cairo_move_to(cr, x - 8, y - 7);
    cairo_line_to(cr, x - 8, y + 7);
    cairo_line_to(cr, x + 1, y + 7);
    cairo_move_to(cr, x + 1, y - 7);
    cairo_line_to(cr, x - 8, y - 7);
    cairo_move_to(cr, x - 2, y);
    cairo_line_to(cr, x + 8, y);
    cairo_move_to(cr, x + 4, y - 4);
    cairo_line_to(cr, x + 8, y);
    cairo_line_to(cr, x + 4, y + 4);
    cairo_stroke(cr);
}

static void icon_undo(cairo_t *cr, double x, double y) {
    stroke_icon(cr);
    cairo_arc(cr, x + 1, y + 1, 7, -0.4, 3.6);
    cairo_stroke(cr);
    cairo_move_to(cr, x - 7, y - 2);
    cairo_line_to(cr, x - 7, y + 5);
    cairo_line_to(cr, x, y + 5);
    cairo_stroke(cr);
}

static void icon_brightness(cairo_t *cr, double x, double y) {
    stroke_icon(cr);
    cairo_arc(cr, x, y, 4, 0, 2 * G_PI);
    cairo_stroke(cr);
    for (int i = 0; i < 8; i++) {
        double angle = i * G_PI / 4;
        cairo_move_to(cr, x + cos(angle) * 7, y + sin(angle) * 7);
        cairo_line_to(cr, x + cos(angle) * 11, y + sin(angle) * 11);
    }
    cairo_stroke(cr);
}

static void icon_media(cairo_t *cr, double x, double y) {
    stroke_icon(cr);
    cairo_move_to(cr, x - 4, y - 7);
    cairo_line_to(cr, x + 7, y);
    cairo_line_to(cr, x - 4, y + 7);
    cairo_close_path(cr);
    cairo_stroke(cr);
}

static void icon_letter(cairo_t *cr, double x, double y, const char *text) {
    char letter[8] = {0};
    if (text && text[0]) {
        const char *next = g_utf8_next_char(text);
        int bytes = (int)(next - text);
        if (bytes > 7) bytes = 7;
        memcpy(letter, text, (size_t)bytes);
    } else {
        letter[0] = '?';
    }
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, letter, -1);
    PangoFontDescription *font = pango_font_description_from_string("Sans Bold 16");
    pango_layout_set_font_description(layout, font);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_move_to(cr, x - width / 2.0, y - height / 2.0);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}

static void draw_icon(cairo_t *cr, const char *icon, double x, double y) {
    if (!icon || strcmp(icon, "dot") == 0) {
        cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
        cairo_arc(cr, x, y, 3.5, 0, 2 * G_PI);
        cairo_fill(cr);
        return;
    }
    if (strcmp(icon, "volume") == 0) icon_volume(cr, x, y);
    else if (strcmp(icon, "scroll") == 0) icon_scroll(cr, x, y);
    else if (strcmp(icon, "zoom") == 0) icon_zoom(cr, x, y);
    else if (strcmp(icon, "undo") == 0) icon_undo(cr, x, y);
    else if (strcmp(icon, "exit") == 0) icon_exit(cr, x, y);
    else if (strcmp(icon, "folder") == 0) icon_folder(cr, x, y);
    else if (strcmp(icon, "back") == 0) icon_back(cr, x, y);
    else if (strcmp(icon, "brightness") == 0) icon_brightness(cr, x, y);
    else if (strcmp(icon, "media") == 0 || strcmp(icon, "play") == 0) icon_media(cr, x, y);
    else icon_letter(cr, x, y, icon);
}

static void draw_label(cairo_t *cr, const char *label, double cx, double cy, double width) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, label, -1);
    PangoFontDescription *font = pango_font_description_from_string("Sans 16");
    pango_layout_set_font_description(layout, font);
    pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    int text_width = 0, text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    cairo_set_source_rgb(cr, 0.96, 0.96, 0.96);
    cairo_move_to(cr, cx - width / 2, cy - text_height / 2.0);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}

void dial_draw(cairo_t *cr, int width, int height, Item **items, int count, int selected, double accent_r,
               double accent_g, double accent_b) {
    double cx = width / 2.0;
    double cy = height / 2.0;
    double radius = MIN(width, height) * 0.36;
    double inner = radius * 0.48;
    if (count < 1 || !items) {
        return;
    }
    if (selected < 0 || selected >= count) {
        selected = 0;
    }

    cairo_save(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_source_rgb(cr, 0.09, 0.09, 0.09);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
    cairo_arc(cr, cx, cy, inner, 0, 2 * G_PI);
    cairo_fill(cr);

    double span = (2 * G_PI / count) * 0.34;
    double mid = -G_PI_2 + selected * (2 * G_PI / count);
    cairo_set_source_rgb(cr, accent_r, accent_g, accent_b);
    cairo_set_line_width(cr, 4.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx, cy, radius - 7, mid - span, mid + span);
    cairo_stroke(cr);

    double icon_radius = (radius + inner) / 2.0;
    for (int i = 0; i < count; i++) {
        double angle = -G_PI_2 + i * (2 * G_PI / count);
        draw_icon(cr, items[i]->icon, cx + cos(angle) * icon_radius, cy + sin(angle) * icon_radius);
    }
    draw_label(cr, items[selected]->label, cx, cy, inner * 1.7);
    cairo_restore(cr);
}
