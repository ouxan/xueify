#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xueify.h"

#define ICON_PX (COVER * 2) // kept at 2x so HiDPI outputs stay sharp

static PangoFontDescription *title_font, *body_font;
static cairo_t *measure_cr;

static void set_rgb(cairo_t *cr, uint32_t c)
{
	cairo_set_source_rgb(cr, (c >> 16 & 0xff) / 255.0, (c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void render_init(void)
{
	title_font = pango_font_description_from_string(FONT);
	pango_font_description_set_absolute_size(title_font, TITLE_PX * PANGO_SCALE);
	pango_font_description_set_weight(title_font, PANGO_WEIGHT_SEMIBOLD);
	body_font = pango_font_description_from_string(FONT);
	pango_font_description_set_absolute_size(body_font, BODY_PX * PANGO_SCALE);

	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	measure_cr = cairo_create(s);
	cairo_surface_destroy(s);
}

// Summaries are plain text on one line
char *render_text(const char *s)
{
	char *t = strdup(s);
	for (char *p = t; p && *p; p++)
		if (*p == '\n' || *p == '\r' || *p == '\t')
			*p = ' ';
	return t;
}

static bool tag_kept(const char *tag, size_t len)
{
	static const char *kept[] = { "b", "/b", "i", "/i", "u", "/u" };
	for (size_t i = 0; i < sizeof kept / sizeof *kept; i++)
		if (strlen(kept[i]) == len && !strncmp(tag, kept[i], len))
			return true;
	return false;
}

// Keeps the tags Pango shares with the spec (b, i, u), turns <br> into a space
// and drops the rest (a, img, ...); falls back to plain text if Pango rejects it
char *render_body(const char *s, bool *markup)
{
	size_t len = strlen(s);
	char *marked = malloc(len + 1), *plain = malloc(len + 1);
	char *m = marked, *p = plain;
	if (!marked || !plain) {
		free(marked);
		free(plain);
		*markup = false;
		return strdup("");
	}
	for (const char *c = s; *c; c++) {
		if (*c == '<') {
			const char *end = strchr(c, '>');
			if (end) {
				size_t tlen = end - c - 1;
				if (tag_kept(c + 1, tlen)) {
					memcpy(m, c, tlen + 2);
					m += tlen + 2;
				} else if (!strncmp(c + 1, "br", 2)) {
					*m++ = ' ';
					*p++ = ' ';
				}
				c = end;
				continue;
			}
		}
		char ch = (*c == '\n' || *c == '\r' || *c == '\t') ? ' ' : *c;
		*m++ = ch;
		*p++ = ch;
	}
	*m = *p = '\0';

	*markup = pango_parse_markup(marked, -1, 0, NULL, NULL, NULL, NULL);
	if (*markup) {
		free(plain);
		return marked;
	}
	free(marked);
	return plain;
}

static PangoLayout *title_layout(cairo_t *cr, const struct notif *n)
{
	PangoLayout *l = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(l, title_font);
	pango_layout_set_width(l, TEXT_W * PANGO_SCALE);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	pango_layout_set_text(l, n->summary, -1);
	return l;
}

static PangoLayout *body_layout(cairo_t *cr, const struct notif *n)
{
	if (!*n->body)
		return NULL;
	PangoLayout *l = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(l, body_font);
	pango_layout_set_width(l, TEXT_W * PANGO_SCALE);
	pango_layout_set_height(l, -2);
	pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	if (n->body_markup)
		pango_layout_set_markup(l, n->body, -1);
	else
		pango_layout_set_text(l, n->body, -1);
	return l;
}

void render_measure(struct notif *n)
{
	int h;
	PangoLayout *l = title_layout(measure_cr, n);
	pango_layout_get_pixel_size(l, NULL, &n->title_h);
	g_object_unref(l);
	n->text_h = n->title_h;
	if ((l = body_layout(measure_cr, n))) {
		pango_layout_get_pixel_size(l, NULL, &h);
		n->text_h += h;
		g_object_unref(l);
	}
	n->card_h = n->text_h + 2 * CARD_PAD_Y;
	if (n->card_h < CARD_MIN_H)
		n->card_h = CARD_MIN_H;
}

// Three box blurs approximate the Gaussian of CSS box-shadow
static void blur_line(uint8_t *p, int n, int step, int r, uint8_t *tmp)
{
	int d = 2 * r + 1, sum = 0;
	for (int i = 0; i < n; i++)
		tmp[i] = p[i * step];
	for (int i = 0; i < r && i < n; i++)
		sum += tmp[i];
	for (int i = 0; i < n; i++) {
		if (i + r < n)
			sum += tmp[i + r];
		if (i - r - 1 >= 0)
			sum -= tmp[i - r - 1];
		p[i * step] = (sum + d / 2) / d;
	}
}

static void draw_shadow(cairo_t *cr, const struct notif *n, int w, int h, int scale)
{
	cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
	cairo_t *mc = cairo_create(mask);
	cairo_scale(mc, scale, scale);
	rounded_rect(mc, PAD, PAD + SHADOW_Y, CARD_W, n->card_h, CARD_R);
	cairo_fill(mc);
	cairo_destroy(mc);
	cairo_surface_flush(mask);

	uint8_t *px = cairo_image_surface_get_data(mask);
	int stride = cairo_image_surface_get_stride(mask);
	int r = (int)lround(sqrt(4.0 * SHADOW_SIGMA * SHADOW_SIGMA * scale * scale + 1) / 2 - 0.5);
	uint8_t *tmp = malloc(w > h ? w : h);
	if (tmp) {
		for (int pass = 0; pass < 3; pass++) {
			for (int y = 0; y < h; y++)
				blur_line(px + y * stride, w, 1, r, tmp);
			for (int x = 0; x < w; x++)
				blur_line(px + x, h, stride, r, tmp);
		}
		free(tmp);
	}
	cairo_surface_mark_dirty(mask);

	cairo_save(cr);
	cairo_identity_matrix(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, SHADOW_ALPHA);
	cairo_mask_surface(cr, mask, 0, 0);
	cairo_restore(cr);
	cairo_surface_destroy(mask);
}

// Bell from the Lucide set, 24x24 with a 2px round stroke
static void draw_placeholder(cairo_t *cr, double x, double y)
{
	cairo_save(cr);
	rounded_rect(cr, x, y, COVER, COVER, COVER_R);
	set_rgb(cr, COL_COVER_BG);
	cairo_fill(cr);

	cairo_translate(cr, x + (COVER - 24) / 2.0, y + (COVER - 24) / 2.0);
	cairo_move_to(cr, 6, 8);
	cairo_arc(cr, 12, 8, 6, M_PI, 2 * M_PI);
	cairo_curve_to(cr, 18, 15, 21, 17, 21, 17);
	cairo_line_to(cr, 3, 17);
	cairo_curve_to(cr, 3, 17, 6, 15, 6, 8);
	cairo_new_sub_path(cr);
	cairo_arc_negative(cr, 12, 20, 2, 5 * M_PI / 6, M_PI / 6);
	set_rgb(cr, COL_BODY);
	cairo_set_line_width(cr, 2);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
	cairo_restore(cr);
}

void render_card(const struct notif *n, cairo_surface_t *target, int scale)
{
	cairo_t *cr = cairo_create(target);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_scale(cr, scale, scale);

	draw_shadow(cr, n, cairo_image_surface_get_width(target), cairo_image_surface_get_height(target), scale);

	rounded_rect(cr, PAD, PAD, CARD_W, n->card_h, CARD_R);
	set_rgb(cr, COL_CARD);
	cairo_fill(cr);
	if (n->critical) {
		rounded_rect(cr, PAD + 0.5, PAD + 0.5, CARD_W - 1, n->card_h - 1, CARD_R - 0.5);
		set_rgb(cr, COL_ACCENT);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}

	double cx = PAD + CARD_PAD_X, cy = PAD + (n->card_h - COVER) / 2.0;
	if (n->icon) {
		cairo_save(cr);
		rounded_rect(cr, cx, cy, COVER, COVER, COVER_R);
		cairo_clip(cr);
		cairo_translate(cr, cx, cy);
		cairo_scale(cr, (double)COVER / ICON_PX, (double)COVER / ICON_PX);
		cairo_set_source_surface(cr, n->icon, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
	} else {
		draw_placeholder(cr, cx, cy);
	}

	double tx = cx + COVER + COVER_GAP, ty = PAD + (n->card_h - n->text_h) / 2.0;
	PangoLayout *l = title_layout(cr, n);
	cairo_move_to(cr, tx, ty);
	set_rgb(cr, COL_TITLE);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
	if ((l = body_layout(cr, n))) {
		cairo_move_to(cr, tx, ty + n->title_h);
		set_rgb(cr, COL_BODY);
		pango_cairo_show_layout(cr, l);
		g_object_unref(l);
	}
	cairo_destroy(cr);
}

// Scales and centre-crops like CSS object-fit: cover
static cairo_surface_t *cover_crop(cairo_surface_t *src, int w, int h)
{
	cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICON_PX, ICON_PX);
	cairo_t *cr = cairo_create(dst);
	double sc = (double)ICON_PX / (w < h ? w : h);
	cairo_translate(cr, (ICON_PX - w * sc) / 2, (ICON_PX - h * sc) / 2);
	cairo_scale(cr, sc, sc);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	return dst;
}

cairo_surface_t *render_icon_pixels(int w, int h, int stride, bool alpha, int channels, const uint8_t *px, size_t len)
{
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || channels != (alpha ? 4 : 3) ||
	    stride < w * channels || (size_t)stride * (h - 1) + (size_t)w * channels > len)
		return NULL;
	cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(src);
		return NULL;
	}
	uint8_t *d = cairo_image_surface_get_data(src);
	int ds = cairo_image_surface_get_stride(src);
	for (int y = 0; y < h; y++) {
		const uint8_t *s = px + (size_t)y * stride;
		uint32_t *row = (uint32_t *)(d + (size_t)y * ds);
		for (int x = 0; x < w; x++, s += channels) {
			uint32_t a = alpha ? s[3] : 255;
			row[x] = a << 24 | (s[0] * a + 127) / 255 << 16 | (s[1] * a + 127) / 255 << 8 | (s[2] * a + 127) / 255;
		}
	}
	cairo_surface_mark_dirty(src);
	cairo_surface_t *dst = cover_crop(src, w, h);
	cairo_surface_destroy(src);
	return dst;
}

static cairo_surface_t *icon_from_file(const char *path)
{
	int w, h;
	if (!gdk_pixbuf_get_file_info(path, &w, &h) || w <= 0 || h <= 0)
		return NULL;
	GdkPixbuf *pb = w < h ? gdk_pixbuf_new_from_file_at_scale(path, ICON_PX, -1, TRUE, NULL)
			      : gdk_pixbuf_new_from_file_at_scale(path, -1, ICON_PX, TRUE, NULL);
	if (!pb)
		return NULL;
	const guint8 *px = gdk_pixbuf_read_pixels(pb);
	gsize len = gdk_pixbuf_get_byte_length(pb);
	cairo_surface_t *s = render_icon_pixels(gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb),
						gdk_pixbuf_get_rowstride(pb), gdk_pixbuf_get_has_alpha(pb),
						gdk_pixbuf_get_n_channels(pb), px, len);
	g_object_unref(pb);
	return s;
}

// A path, a file:// URI or an icon name; PNGs are tried first since SVGs load librsvg
cairo_surface_t *render_icon_lookup(const char *name)
{
	if (!strncmp(name, "file://", 7))
		name += 7;
	if (name[0] == '/')
		return icon_from_file(name);
	if (strchr(name, '/'))
		return NULL;

	static const char *sizes[] = { "96x96", "128x128", "64x64", "256x256", "48x48", "512x512", "32x32" };
	static const char *dirs[] = { "apps", "status", "devices" };
	const char *home = getenv("HOME");
	char local[256], path[512];
	snprintf(local, sizeof local, "%s/.local/share/icons/hicolor", home ? home : "");
	const char *bases[] = { local, "/usr/share/icons/hicolor", "/usr/share/icons/Adwaita" };

	for (int b = 0; b < 3; b++)
		for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
			for (size_t d = 0; d < sizeof dirs / sizeof *dirs; d++) {
				snprintf(path, sizeof path, "%s/%s/%s/%s.png", bases[b], sizes[s], dirs[d], name);
				if (!access(path, R_OK))
					return icon_from_file(path);
			}
	snprintf(path, sizeof path, "/usr/share/pixmaps/%s.png", name);
	if (!access(path, R_OK))
		return icon_from_file(path);

	for (int b = 0; b < 3; b++)
		for (size_t d = 0; d < sizeof dirs / sizeof *dirs; d++) {
			snprintf(path, sizeof path, "%s/scalable/%s/%s.svg", bases[b], dirs[d], name);
			if (!access(path, R_OK))
				return icon_from_file(path);
		}
	snprintf(path, sizeof path, "/usr/share/pixmaps/%s.svg", name);
	if (!access(path, R_OK))
		return icon_from_file(path);
	return NULL;
}
