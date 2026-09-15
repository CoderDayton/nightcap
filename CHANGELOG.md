# Changelog

All notable changes to Nightcap. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Release notes on
GitHub are taken from the matching section here.

## [Unreleased]

### Added

- FAQ covering Discord RPC, log locations, and VR, adapted from upstream.

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

[Unreleased]: https://github.com/CoderDayton/nightcap/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/CoderDayton/nightcap/releases/tag/v0.1.0
