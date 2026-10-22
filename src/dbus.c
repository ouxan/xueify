#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

#include "xueify.h"

#define BUS_NAME "org.freedesktop.Notifications"
#define BUS_PATH "/org/freedesktop/Notifications"

static sd_bus *bus;

static int read_variant_int(sd_bus_message *m, const char *sig, int64_t *out)
{
	union { uint8_t y; int32_t i; uint32_t u; } v;
	int r = sd_bus_message_enter_container(m, 'v', sig);
	if (r < 0)
		return r;
	switch (sig[0]) {
	case 'y': r = sd_bus_message_read_basic(m, 'y', &v.y); *out = v.y; break;
	case 'i': r = sd_bus_message_read_basic(m, 'i', &v.i); *out = v.i; break;
	case 'u': r = sd_bus_message_read_basic(m, 'u', &v.u); *out = v.u; break;
	default:  r = sd_bus_message_skip(m, sig); *out = -1; break;
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

static int read_variant_string(sd_bus_message *m, const char *sig, char **out)
{
	if (strcmp(sig, "s"))
		return sd_bus_message_skip(m, "v");
	const char *s;
	int r = sd_bus_message_read(m, "v", "s", &s);
	if (r < 0)
		return r;
	free(*out);
	*out = strdup(s);
	return 0;
}

static int read_variant_image(sd_bus_message *m, const char *sig, struct notif *n)
{
	if (strcmp(sig, "(iiibiiay)") || n->icon)
		return sd_bus_message_skip(m, "v");
	int32_t w, h, stride, bps, channels;
	int alpha;
	const void *px;
	size_t len;
	int r;
	if ((r = sd_bus_message_enter_container(m, 'v', sig)) < 0 ||
	    (r = sd_bus_message_enter_container(m, 'r', "iiibiiay")) < 0 ||
	    (r = sd_bus_message_read(m, "iiibii", &w, &h, &stride, &alpha, &bps, &channels)) < 0 ||
	    (r = sd_bus_message_read_array(m, 'y', &px, &len)) < 0 ||
	    (r = sd_bus_message_exit_container(m)) < 0 ||
	    (r = sd_bus_message_exit_container(m)) < 0)
		return r;
	if (bps == 8)
		n->icon = render_icon_pixels(w, h, stride, alpha, channels, px, len);
	return 0;
}

static int read_hints(sd_bus_message *m, struct notif *n, char **image_path)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key, *sig;
		char type;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0 ||
		    (r = sd_bus_message_peek_type(m, &type, &sig)) < 0)
			return r;

		if (!strcmp(key, "urgency")) {
			int64_t v = 1;
			r = read_variant_int(m, sig, &v);
			n->critical = v == 2;
		} else if (!strcmp(key, "image-data") || !strcmp(key, "image_data") || !strcmp(key, "icon_data")) {
			r = read_variant_image(m, sig, n);
		} else if (!strcmp(key, "image-path") || !strcmp(key, "image_path")) {
			r = read_variant_string(m, sig, image_path);
		} else if (!strcmp(key, "x-canonical-private-synchronous") || !strcmp(key, "synchronous") ||
			   !strcmp(key, "x-dunst-stack-tag")) {
			r = read_variant_string(m, sig, &n->tag);
		} else {
			r = sd_bus_message_skip(m, "v");
		}
		if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0)
			return r;
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

static int method_notify(sd_bus_message *m, void *data, sd_bus_error *error)
{
	(void)data; (void)error;
	const char *app, *icon, *summary, *body;
	uint32_t replaces;
	int32_t timeout;
	char **actions = NULL, *image_path = NULL;
	struct notif *n = calloc(1, sizeof *n);
	if (!n)
		return -ENOMEM;

	int r = sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body);
	if (r >= 0)
		r = sd_bus_message_read_strv(m, &actions);
	if (r >= 0)
		r = read_hints(m, n, &image_path);
	if (r >= 0)
		r = sd_bus_message_read(m, "i", &timeout);
	if (r < 0) {
		free(image_path);
		notif_free(n);
		return r;
	}

	for (char **a = actions; a && a[0] && a[1]; a += 2)
		if (!strcmp(a[0], "default"))
			n->has_default = true;
	for (char **a = actions; a && *a; a++)
		free(*a);
	free(actions);

	n->summary = render_text(*summary ? summary : app);
	n->body = render_body(body, &n->body_markup);
	if (n->critical)
		n->timeout = 0;
	else
		n->timeout = timeout < 0 ? DEFAULT_TIMEOUT : timeout;

	// Precedence from the spec: image-data, image-path, app_icon
	if (!n->icon && image_path && *image_path)
		n->icon = render_icon_lookup(image_path);
	if (!n->icon && *icon)
		n->icon = render_icon_lookup(icon);
	free(image_path);

	return sd_bus_reply_method_return(m, "u", notif_add(n, replaces));
}

static int method_close(sd_bus_message *m, void *data, sd_bus_error *error)
{
	(void)data; (void)error;
	uint32_t id;
	int r = sd_bus_message_read(m, "u", &id);
	if (r < 0)
		return r;
	notif_close_id(id, CLOSE_CALLED);
	return sd_bus_reply_method_return(m, "");
}

static int method_caps(sd_bus_message *m, void *data, sd_bus_error *error)
{
	(void)data; (void)error;
	return sd_bus_reply_method_return(m, "as", 4, "body", "body-markup", "actions", "icon-static");
}

static int method_info(sd_bus_message *m, void *data, sd_bus_error *error)
{
	(void)data; (void)error;
	return sd_bus_reply_method_return(m, "ssss", "xueify", "xueify", VERSION, "1.2");
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", method_notify, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification", "u", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetCapabilities", "", "as", method_caps, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation", "", "ssss", method_info, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
	SD_BUS_SIGNAL("ActionInvoked", "us", 0),
	SD_BUS_VTABLE_END
};

int dbus_init(void)
{
	int r = sd_bus_open_user(&bus);
	if (r < 0) {
		fprintf(stderr, "xueify: cannot connect to the session bus: %s\n", strerror(-r));
		return -1;
	}
	r = sd_bus_add_object_vtable(bus, NULL, BUS_PATH, BUS_NAME, vtable, NULL);
	if (r >= 0)
		r = sd_bus_request_name(bus, BUS_NAME, 0);
	if (r < 0) {
		fprintf(stderr, "xueify: cannot own %s (%s); is another notification daemon such as dunst running?\n",
			BUS_NAME, strerror(-r));
		return -1;
	}
	return sd_bus_get_fd(bus);
}

int dbus_events(void)
{
	int r = sd_bus_get_events(bus);
	return r < 0 ? 0 : r;
}

int dbus_timeout_ms(void)
{
	uint64_t until;
	if (sd_bus_get_timeout(bus, &until) < 0 || until == UINT64_MAX)
		return -1;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
	return until <= now ? 0 : (int)((until - now + 999) / 1000);
}

void dbus_dispatch(void)
{
	while (sd_bus_process(bus, NULL) > 0)
		;
}

void dbus_closed(uint32_t id, uint32_t reason)
{
	sd_bus_emit_signal(bus, BUS_PATH, BUS_NAME, "NotificationClosed", "uu", id, reason);
}

void dbus_action(uint32_t id, const char *key)
{
	sd_bus_emit_signal(bus, BUS_PATH, BUS_NAME, "ActionInvoked", "us", id, key);
}
