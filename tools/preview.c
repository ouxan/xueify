// Renders sample cards with the daemon's own drawing code, for docs and visual checks
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xueify.h"

#define SCALE  2
#define MARGIN 24

static cairo_surface_t *sample_cover(void)
{
	enum { S = 96 };
	static uint8_t px[S * S * 4];
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			double t = (x + y) / (2.0 * S);
			double sun = fmin(fmax(S * 0.24 - hypot(x + 0.5 - S * 0.5, y + 0.5 - S * 0.62) + 0.5, 0), 1);
			uint8_t *p = px + (y * S + x) * 4;
			p[0] = (uint8_t)(sun * 255 + (1 - sun) * (124 + t * 120));
			p[1] = (uint8_t)(sun * 196 + (1 - sun) * (92 - t * 40));
			p[2] = (uint8_t)(sun * 120 + (1 - sun) * (255 - t * 120));
			p[3] = 255;
		}
	return render_icon_pixels(S, S, S * 4, true, 4, px, sizeof px);
}

static struct notif *sample(const char *summary, const char *body, bool critical, cairo_surface_t *icon)
{
	struct notif *n = calloc(1, sizeof *n);
	n->summary = render_text(summary);
	n->body = render_body(body, &n->body_markup);
	n->critical = critical;
	n->icon = icon;
	render_measure(n);
	return n;
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "preview.png";
	render_init();

	struct notif *cards[] = {
		sample("Midnight Drive", "Neon Avenue", false, sample_cover()),
		sample("Build finished", "<b>xueify</b> compiled in 0.4 s with no warnings, ready to install.", false, NULL),
		sample("Low battery", "5% remaining, plug in soon", true, NULL),
	};
	int count = sizeof cards / sizeof *cards;

	int h = 2 * MARGIN - STACK_GAP;
	for (int i = 0; i < count; i++)
		h += cards[i]->card_h + STACK_GAP;
	int w = CARD_W + 2 * MARGIN;

	cairo_surface_t *canvas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * SCALE, h * SCALE);
	cairo_t *cr = cairo_create(canvas);
	cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, w * SCALE, h * SCALE);
	cairo_pattern_add_color_stop_rgb(bg, 0, 0x2a / 255.0, 0x2d / 255.0, 0x3e / 255.0);
	cairo_pattern_add_color_stop_rgb(bg, 1, 0x12 / 255.0, 0x13 / 255.0, 0x1c / 255.0);
	cairo_set_source(cr, bg);
	cairo_paint(cr);
	cairo_pattern_destroy(bg);

	int y = MARGIN;
	for (int i = 0; i < count; i++) {
		struct notif *n = cards[i];
		cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SURF_W * SCALE,
								(n->card_h + 2 * PAD) * SCALE);
		render_card(n, s, SCALE);
		cairo_set_source_surface(cr, s, (MARGIN - PAD) * SCALE, (y - PAD) * SCALE);
		cairo_paint(cr);
		cairo_surface_destroy(s);
		y += n->card_h + STACK_GAP;
	}
	cairo_destroy(cr);

	cairo_status_t st = cairo_surface_write_to_png(canvas, out);
	cairo_surface_destroy(canvas);
	if (st != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "preview: cannot write %s: %s\n", out, cairo_status_to_string(st));
		return 1;
	}
	return 0;
}
