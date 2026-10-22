# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [0.2.0] - 2026-10-23

### Added
- `ICON_RULES` in `src/xueify.h` replace the sender's icon by app name or summary
  prefix with a file, a themed icon or a built-in drawing; notifications from
  Claude Code show its pixel mascot.

## [0.1.0] - 2026-10-23

### Added
- freedesktop.org notification server (`Notify`, `CloseNotification`,
  `GetCapabilities`, `GetServerInformation`, `NotificationClosed`, `ActionInvoked`).
- Rounded cards on wlr-layer-shell surfaces, stacked in the bottom-right corner,
  up to five at once with a queue for the rest.
- Images from `image-data`, `image-path` and `app_icon`, cropped to a square cover;
  a bell placeholder otherwise.
- Body markup (`b`, `i`, `u`) with a plain-text fallback, two-line ellipsized bodies.
- In-place replacement through `replaces_id` and synchronous/stack-tag hints.
- Hover pauses timeouts; left click invokes the default action, right click closes,
  middle click closes everything.
- HiDPI rendering through `wl_surface.preferred_buffer_scale`.
- systemd user unit, D-Bus activation file, man page and Arch Linux PKGBUILD.

[0.2.0]: https://github.com/ouxan/xueify/releases/tag/v0.2.0
[0.1.0]: https://github.com/ouxan/xueify/releases/tag/v0.1.0
