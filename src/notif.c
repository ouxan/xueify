#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "xueify.h"

static struct notif *head; // arrival order; the oldest MAX_VISIBLE are on screen
static uint32_t next_id = 1;

void notif_free(struct notif *n)
{
	free(n->summary);
	free(n->body);
	free(n->tag);
	if (n->icon)
		cairo_surface_destroy(n->icon);
	free(n);
}

// One timerfd for the nearest expiry; disarmed when nothing can expire
static void arm_timer(void)
{
	uint64_t next = 0;
	for (struct notif *n = head; n; n = n->next)
		if (n->wl_surface && n->expire_at && !n->hovered && (!next || n->expire_at < next))
			next = n->expire_at;
	struct itimerspec its = { 0 };
	its.it_value.tv_sec = next / 1000;
	its.it_value.tv_nsec = (next % 1000) * 1000000;
	timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
}

static void start_timer(struct notif *n)
{
	n->expire_at = n->timeout ? now_ms() + n->timeout : 0;
}

// Shows queued notifications while there is room, newest at the bottom
static void layout(void)
{
	struct notif *vis[MAX_VISIBLE];
	int count = 0;
	for (struct notif *n = head; n && count < MAX_VISIBLE; n = n->next)
		vis[count++] = n;

	int bottom = 0;
	for (int i = count - 1; i >= 0; i--) {
		struct notif *n = vis[i];
		if (!n->wl_surface) {
			n->bottom = bottom;
			wl_show(n);
			start_timer(n);
		} else if (n->bottom != bottom) {
			n->bottom = bottom;
			wl_update(n, false);
		}
		bottom += n->card_h + STACK_GAP;
	}
	arm_timer();
}

uint32_t notif_add(struct notif *n, uint32_t replaces)
{
	render_measure(n);

	struct notif **link = &head;
	for (; *link; link = &(*link)->next)
		if ((replaces && (*link)->id == replaces) || (n->tag && (*link)->tag && !strcmp(n->tag, (*link)->tag)))
			break;

	struct notif *old = *link;
	if (!old) {
		n->id = next_id++;
		if (!next_id)
			next_id = 1;
		*link = n;
		layout();
		return n->id;
	}

	// Replacing keeps the id, the slot and the surface, so it redraws in place without animating
	n->id = old->id;
	n->next = old->next;
	*link = n;
	wl_adopt(old, n);
	if (n->wl_surface) {
		start_timer(n);
		if (n->card_h != old->card_h)
			wl_update(n, true);
		else
			wl_update(n, false);
	}
	notif_free(old);
	layout();
	return n->id;
}

void notif_close(struct notif *n, enum close_reason reason)
{
	struct notif **link = &head;
	while (*link && *link != n)
		link = &(*link)->next;
	if (!*link)
		return;
	*link = n->next;
	if (n->wl_surface)
		wl_hide(n);
	dbus_closed(n->id, reason);
	notif_free(n);
	layout();
}

void notif_close_id(uint32_t id, enum close_reason reason)
{
	for (struct notif *n = head; n; n = n->next)
		if (n->id == id) {
			notif_close(n, reason);
			return;
		}
}

void notif_expire(void)
{
	uint64_t ticks;
	if (read(timer_fd, &ticks, sizeof ticks) < 0) {
		// Spurious wakeup, nothing expired yet
	}
	uint64_t now = now_ms();
	bool closed;
	do {
		closed = false;
		for (struct notif *n = head; n; n = n->next)
			if (n->wl_surface && n->expire_at && !n->hovered && n->expire_at <= now) {
				notif_close(n, CLOSE_EXPIRED);
				closed = true;
				break;
			}
	} while (closed);
	arm_timer();
}

// Hovering holds the notification; leaving restarts its full timeout
void notif_hover(struct notif *n, bool on)
{
	n->hovered = on;
	if (!on)
		start_timer(n);
	arm_timer();
}

void notif_click(struct notif *n, uint32_t button)
{
	if (button == BTN_MIDDLE) {
		while (head) {
			n = head;
			head = n->next;
			if (n->wl_surface)
				wl_hide(n);
			dbus_closed(n->id, CLOSE_DISMISSED);
			notif_free(n);
		}
		arm_timer();
		return;
	}
	if (button == BTN_LEFT && n->has_default)
		dbus_action(n->id, "default");
	notif_close(n, CLOSE_DISMISSED);
}
