#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <wayland-client.h>

#include "xueify.h"

int timer_fd = -1;

uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
			puts("xueify " VERSION);
			return 0;
		}
		bool help = !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help");
		fprintf(help ? stdout : stderr,
			"Usage: %s [-h|--help] [-v|--version]\n\n"
			"Lightweight notification daemon for wlroots-style Wayland compositors.\n"
			"It owns org.freedesktop.Notifications on the session bus and takes no options.\n",
			argv[0]);
		return help ? 0 : 2;
	}

	int wl_fd = wl_init();
	if (wl_fd < 0)
		return 1;
	render_init();
	int bus_fd = dbus_init();
	if (bus_fd < 0)
		return 1;
	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (timer_fd < 0) {
		perror("timerfd_create");
		return 1;
	}

	// Sleeps in poll() until a notification, a click or an expiry arrives
	struct pollfd fds[3] = {
		{ .fd = wl_fd, .events = POLLIN },
		{ .fd = bus_fd },
		{ .fd = timer_fd, .events = POLLIN },
	};
	for (;;) {
		dbus_dispatch();
		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		if (wl_display_flush(display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(display);
			break;
		}

		fds[1].events = dbus_events();
		if (poll(fds, 3, dbus_timeout_ms()) < 0) {
			wl_display_cancel_read(display);
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(display) < 0)
				break;
		} else {
			wl_display_cancel_read(display);
		}
		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
		if (wl_display_dispatch_pending(display) < 0)
			break;
		if (fds[2].revents & POLLIN)
			notif_expire();
	}
	return 0;
}
