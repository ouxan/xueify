#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "cursor-shape-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xueify.h"

struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wp_cursor_shape_manager_v1 *cursor_manager;
static struct wp_cursor_shape_device_v1 *cursor_device;
static struct notif *pointer_on;

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	(void)data;
	wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

// The buffer is drawn once and unmapped right away; the compositor keeps its own mapping
static void draw(struct notif *n)
{
	int w = SURF_W * n->scale, h = (n->card_h + 2 * PAD) * n->scale;
	int stride = w * 4, size = stride * h;

	int fd = memfd_create("xueify", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		perror("xueify: shm");
		if (fd >= 0)
			close(fd);
		return;
	}
	void *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		perror("xueify: mmap");
		close(fd);
		return;
	}
	cairo_surface_t *cs = cairo_image_surface_create_for_data(px, CAIRO_FORMAT_ARGB32, w, h, stride);
	render_card(n, cs, n->scale);
	cairo_surface_destroy(cs);
	munmap(px, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(buffer, &buffer_listener, NULL);

	// Only the card takes clicks, not the transparent shadow margin
	struct wl_region *input = wl_compositor_create_region(compositor);
	wl_region_add(input, PAD, PAD, CARD_W, n->card_h);
	wl_surface_set_input_region(n->wl_surface, input);
	wl_region_destroy(input);

	wl_surface_set_buffer_scale(n->wl_surface, n->scale);
	wl_surface_attach(n->wl_surface, buffer, 0, 0);
	wl_surface_damage(n->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(n->wl_surface);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial, uint32_t w, uint32_t h)
{
	(void)w; (void)h;
	struct notif *n = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if (!n->configured || n->dirty) {
		n->configured = true;
		n->dirty = false;
		draw(n);
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	(void)layer;
	notif_close(data, CLOSE_UNDEFINED);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void surface_enter(void *data, struct wl_surface *s, struct wl_output *o) { (void)data; (void)s; (void)o; }
static void surface_leave(void *data, struct wl_surface *s, struct wl_output *o) { (void)data; (void)s; (void)o; }
static void surface_transform(void *data, struct wl_surface *s, uint32_t t) { (void)data; (void)s; (void)t; }

static void surface_scale(void *data, struct wl_surface *s, int32_t factor)
{
	(void)s;
	struct notif *n = data;
	if (factor < 1 || factor == n->scale)
		return;
	n->scale = factor;
	if (n->configured)
		draw(n);
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
	.preferred_buffer_scale = surface_scale,
	.preferred_buffer_transform = surface_transform,
};

void wl_show(struct notif *n)
{
	n->scale = 1;
	n->wl_surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(n->wl_surface, &surface_listener, n);
	n->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, n->wl_surface, NULL,
							  ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "xueify");
	zwlr_layer_surface_v1_add_listener(n->layer, &layer_listener, n);
	zwlr_layer_surface_v1_set_anchor(n->layer,
					 ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_keyboard_interactivity(n->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_update(n, true);
}

// Applies a new position or height; content changes of the same size redraw at once
void wl_update(struct notif *n, bool resized)
{
	zwlr_layer_surface_v1_set_size(n->layer, SURF_W, n->card_h + 2 * PAD);
	zwlr_layer_surface_v1_set_margin(n->layer, 0, 0, n->bottom, 0);
	if (resized || n->dirty || !n->configured) {
		n->dirty = n->configured;
		wl_surface_commit(n->wl_surface);
	} else {
		draw(n);
	}
}

void wl_hide(struct notif *n)
{
	if (pointer_on == n)
		pointer_on = NULL;
	zwlr_layer_surface_v1_destroy(n->layer);
	wl_surface_destroy(n->wl_surface);
	n->layer = NULL;
	n->wl_surface = NULL;
	n->configured = false;
}

// Moves the surface of a replaced notification to its successor
void wl_adopt(struct notif *from, struct notif *to)
{
	to->wl_surface = from->wl_surface;
	to->layer = from->layer;
	to->scale = from->scale;
	to->bottom = from->bottom;
	to->configured = from->configured;
	to->dirty = from->dirty;
	to->hovered = from->hovered;
	from->wl_surface = NULL;
	from->layer = NULL;
	if (!to->wl_surface)
		return;
	wl_surface_set_user_data(to->wl_surface, to);
	zwlr_layer_surface_v1_set_user_data(to->layer, to);
	if (pointer_on == from)
		pointer_on = to;
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)x; (void)y;
	if (!s)
		return;
	if (cursor_manager && !cursor_device)
		cursor_device = wp_cursor_shape_manager_v1_get_pointer(cursor_manager, p);
	if (cursor_device)
		wp_cursor_shape_device_v1_set_shape(cursor_device, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
	pointer_on = wl_surface_get_user_data(s);
	notif_hover(pointer_on, true);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)data; (void)p; (void)serial; (void)s;
	struct notif *n = pointer_on;
	pointer_on = NULL;
	if (n)
		notif_hover(n, false);
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)p; (void)time; (void)x; (void)y;
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	(void)data; (void)p; (void)serial; (void)time;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && pointer_on)
		notif_click(pointer_on, button);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	(void)data; (void)p; (void)time; (void)axis; (void)value;
}

// The seat is bound at version 1, so these are the only pointer events sent
static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		if (cursor_device)
			wp_cursor_shape_device_v1_destroy(cursor_device);
		cursor_device = NULL;
		wl_pointer_destroy(pointer);
		pointer = NULL;
	}
}

static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities };

static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	(void)data;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 6 ? version : 6);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, version < 3 ? version : 3);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) {
		cursor_manager = wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1);
	}
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

int wl_init(void)
{
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "xueify: cannot connect to the Wayland display\n");
		return -1;
	}
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell) {
		fprintf(stderr, "xueify: the compositor lacks wl_compositor, wl_shm or wlr-layer-shell\n");
		return -1;
	}
	return wl_display_get_fd(display);
}
