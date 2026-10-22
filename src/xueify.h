#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <cairo.h>

#ifndef VERSION
#define VERSION "dev"
#endif

// Card geometry and palette
#define PAD          24   // transparent room around the card for its shadow
#define CARD_W       332
#define CARD_MIN_H   72
#define CARD_R       12
#define CARD_PAD_X   16
#define CARD_PAD_Y   12
#define COVER        48
#define COVER_R      6
#define COVER_GAP    12
#define TEXT_W       (CARD_W - 2 * CARD_PAD_X - COVER - COVER_GAP)
#define SHADOW_Y     8
#define SHADOW_SIGMA 15   // CSS blur radius 30px
#define SHADOW_ALPHA 0.4
#define SURF_W       (CARD_W + 2 * PAD)

#define COL_CARD     0x1f1f27
#define COL_COVER_BG 0x17171d
#define COL_TITLE    0xf2f2f5
#define COL_BODY     0x9a9aa5
#define COL_ACCENT   0x7c5cff

#define FONT         "Adwaita Sans"
#define TITLE_PX     14
#define BODY_PX      12

#define STACK_GAP       8
#define MAX_VISIBLE     5
#define DEFAULT_TIMEOUT 5000

enum close_reason { CLOSE_EXPIRED = 1, CLOSE_DISMISSED = 2, CLOSE_CALLED = 3, CLOSE_UNDEFINED = 4 };

struct wl_surface;
struct zwlr_layer_surface_v1;

struct notif {
	struct notif *next;
	uint32_t id;
	char *summary, *body, *tag;
	bool body_markup, critical, has_default;
	int timeout;            // ms, 0 = stays until dismissed
	uint64_t expire_at;     // CLOCK_MONOTONIC ms, 0 = no timer
	bool hovered;
	cairo_surface_t *icon;  // ICON_PX square, NULL draws the placeholder
	int card_h, text_h, title_h;

	// Wayland state, wl_surface is NULL while queued
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer;
	int scale, bottom;
	bool configured, dirty;
};

// main.c
extern int timer_fd;
uint64_t now_ms(void);

// dbus.c
int dbus_init(void);
int dbus_events(void);
int dbus_timeout_ms(void);
void dbus_dispatch(void);
void dbus_closed(uint32_t id, uint32_t reason);
void dbus_action(uint32_t id, const char *key);

// wl.c
extern struct wl_display *display;
int wl_init(void);
void wl_show(struct notif *n);
void wl_update(struct notif *n, bool resized);
void wl_hide(struct notif *n);
void wl_adopt(struct notif *from, struct notif *to);

// render.c
void render_init(void);
void render_measure(struct notif *n);
void render_card(const struct notif *n, cairo_surface_t *target, int scale);
char *render_text(const char *s);
char *render_body(const char *s, bool *markup);
cairo_surface_t *render_icon_pixels(int w, int h, int stride, bool alpha, int channels, const uint8_t *px, size_t len);
cairo_surface_t *render_icon_lookup(const char *name);

// notif.c
uint32_t notif_add(struct notif *n, uint32_t replaces);
void notif_close(struct notif *n, enum close_reason reason);
void notif_close_id(uint32_t id, enum close_reason reason);
void notif_free(struct notif *n);
void notif_expire(void);
void notif_hover(struct notif *n, bool on);
void notif_click(struct notif *n, uint32_t button);
