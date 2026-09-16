# Changelog

All notable changes to Nightcap. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Release notes on
GitHub are taken from the matching section here.

## [Unreleased]

### Added

- Custom Discord presence. `integrations.discord_rpc.text.title` replaces the
  name in "Playing Nightcap", and `integrations.discord_rpc.images` sets the
  large and small icons and their hover text. Text and image fields accept
  `{place_name}` and `{place_icon}`. Set `text.state: ""` to hide the
  "Playing Roblox" line.

### Fixed

- With `show_place_name: false`, Discord no longer shows the experience
  thumbnail or its name on hover.

## [0.2.0] - 2026-09-16

### Added

- FAQ covering Discord RPC, log locations, and VR, adapted from upstream.
- `input.raw_mouse` in `config.yaml` sends mouse look deltas in the host's
  own units. Turn it on when first-person aiming feels too slow or too fast
  under fractional display scaling. Off by default.
- Nightly AppImage builds, published as the `continuous` prerelease. They are
  untested; use a tagged release for normal play.

### Changed

- Values in `fflags.json` now win over the performance preset instead of
  stopping the launch. Startup prints each key it kept.
- Discord Rich Presence uses Nightcap's own Discord application, so it shows
  as "Playing Nightcap". Terms and privacy pages for it live on the
  project site.
- Discord Rich Presence is on by default. Set
  `integrations.discord_rpc.enabled: false` to turn it off.
- The AppImage build verifies its reduced dependency packages against digests
  taken at build time, rather than a digest list kept in the repository.

### Fixed

- Mouse look keeps responding after a long turn. The tracked pointer position
  could drift past the edge of the view, which swallowed every reversal until
  the mouse travelled all the way back.
- Nightcap no longer refuses to start with `cannot lock appStorage`. It
  created its own credential directory using the process umask, then rejected
  that directory for being group writable. Systems with a permissive umask,
  such as `002`, failed on first run.

## [0.1.0] - 2026-09-15

First Nightcap release, forked from komaruworld/mocktail.

### Added

- ETC2 and EAC texture emulation on Vulkan, so games that use compressed
  mobile textures render correctly on desktop GPUs. Decoding runs on worker
  threads to avoid stutter.
- PNG texture overrides in `~/.config/mocktail/textures`, keyed by upload
  hash, with a dump mode to find the texture you want to replace.
- Small textures are upscaled 4x by default. Set
  `MOCKTAIL_SMALL_TEXTURE_UPSCALE=1` to turn it off.
- AppImage and signed Flatpak builds on every release tag. The Flatpak repo
  is published at https://coderdayton.github.io/nightcap/.
- Roadmap in `docs/ROADMAP.md`.

### Changed

- Frames are presented as soon as they are ready instead of waiting in a
  queue, which lowers input latency.
- Roblox now sees the real host RAM and display size instead of a low-end
  phone profile.
- App ID is `io.github.CoderDayton.nightcap`, so Nightcap installs next to
  upstream Mocktail. The binary and config paths keep the `mocktail` name.
- New name, icon, and colors.
- The desktop entry launches on Wayland and points at the installed binary.

### Fixed

- Servers no longer kick with error 319 shortly after joining.
- The FMOD output-device bridge is optional, so the updater no longer fails
  without it.
- The AppImage build no longer fails on stale checksums for the slimmed
  Arch packages.

[Unreleased]: https://github.com/CoderDayton/nightcap/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/CoderDayton/nightcap/releases/tag/v0.2.0
[0.1.0]: https://github.com/CoderDayton/nightcap/releases/tag/v0.1.0
