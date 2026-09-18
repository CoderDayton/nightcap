# Changelog

All notable changes to Nightcap. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Release notes on
GitHub are taken from the matching section here.

## [Unreleased]

### Added

- Custom Discord presence. `integrations.discord_rpc.text.title` replaces the
  name in "Playing Nightcap", and `integrations.discord_rpc.images` sets the
  large and small icons and their hover text. Text and image fields accept
  `{place_name}`, `{place_icon}`, and `{creator_name}`. A field that expands
  an empty placeholder is left out, and `text.state: ""` hides the state line.
- The default Discord card now matches Sober: the title is "Roblox", the
  details line is "Playing <experience>", the state is "by <creator>", and
  the Roblox badge sits inside the experience icon. The creator comes from the
  same Roblox games request that already fetches the experience name. With
  `show_place_name: false` the creator is hidden too, so the card shows only
  the title and timer.
- `--profile <file>` writes a Chrome-format trace of the Vulkan adapter's
  work: presents, GPU waits, submits, ETC2 texture decode, and pipeline and
  shader creation. Open it in ui.perfetto.dev. `docs/BENCHMARKING.md`
  describes the benchmark runs, and `scripts/summarize_profile_trace.py`
  turns traces into a before and after table.

### Fixed

- Texture uploads stalled the frame they were submitted on. ETC2 batches
  decoded on worker threads but were then resampled one upload at a time on
  the submitting thread, which at the default 4x upscale is sixteen times the
  decoded texels. Resampling now runs on the same worker pool, in two passes so
  mip level 0 still resolves before the mips built from it. In a 70k-part
  place the worst `vkQueueSubmit` fell from 167ms to 92ms.
- Most ETC2 batches decoded on a single thread. Workers were created and
  joined per batch, so the parallel floor had to cover that cost and sat at a
  1024x1024 image; batches under it ran on the caller, which was 54% of all
  decode time in a 70k-part place. Decode workers are now a persistent pool,
  and the floor drops to a 128x128 image. Mean decode time in that place fell
  from 8.2ms to 4.7ms, and 11.8ms before both texture fixes.
- On CPUs with a wide core-speed spread, Feral GameMode pinned the process to
  the few fastest cores, concentrating Roblox's worker pool onto them instead
  of spreading it. With `performance.gamemode: auto`, a request that narrows
  the CPU affinity is now released again, which releases the pin. Set
  `pin_cores=no` in `~/.config/gamemode.ini` to keep GameMode without the
  pinning, or `performance.gamemode: on` to accept it. See
  `docs/PERFORMANCE.md`.
- With `show_place_name: false`, Discord no longer shows the experience
  thumbnail or its name on hover.
- The host main thread no longer spins on the engine's message pump, which
  cost about half a core and 146,000 empty calls a second. It now rests 100 µs
  between polls: sleeping when the governor is `performance`, parking the
  core with TPAUSE on CPUs that have it, spinning otherwise. With GameMode
  setting the governor, process CPU in a 60k-part place fell from 216% to
  107% at the same 120 fps. `MOCKTAIL_ENGINE_PUMP_REST` forces a mode; see
  `docs/PERFORMANCE.md`.
- ETC2 uploads with a `bufferRowLength` or `bufferImageHeight` larger than
  the image read the padded rows as packed data and decoded garbage.
- ETC2 resampling created and joined worker threads on every batch. It now
  runs on the persistent decode pool.
- A failed trace write left the profile writer thread waking every 250 ms
  for the rest of the session. It now exits.
- The `--profile` trace gains a `pump` slice for the engine's main-thread
  step.

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
