# Nightcap roadmap

What Nightcap is working toward, in priority order. Items move to
[Shipped](#shipped) when they land on `main`. Open an issue to propose a
change to this list.

Nothing is queued right now.

## Shipped

- Settings window, from the app menu or `mocktail --settings`: graphics,
input, performance, audio, and Discord options, and a text editor for
`fflags.json`. It edits the same `config.yaml`, keeps your comments, and
does not save a value Nightcap could not start with. Changes apply on the
next launch.
- Lower input latency: frames are presented as soon as they are ready.
- Controllers hot-plug without a restart, and a pad that is unplugged mid-game
releases its held buttons and sticks instead of leaving them stuck.
- Mouse look keeps responding after a long turn, instead of going dead until
the pointer travels back from past the screen edge.
- Optional raw mouse input: `input.raw_mouse` in `config.yaml` sends look
deltas in host units, so fractional display scaling no longer changes
first-person sensitivity.
- Small textures upscaled 4x by default, with PNG overrides in
`~/.config/mocktail/textures`.
- ETC2 textures decoded on desktop GPUs without stutter.
- No more error 319 kicks after joining a server.
- Roblox sees the real host RAM and screen size.
- Desktop launcher that works on Wayland and finds the installed binary.
- Discord Rich Presence on by default, with the experience name, creator,
elapsed time, the experience icon with a Roblox badge, and a join button
when the server is public. See the [FAQ](../FAQ.md) to switch it off.
- Custom Discord presence: title, details, state, and large and small icons
from `config.yaml`, with `{place_name}`, `{place_icon}`, and
`{creator_name}` placeholders.
- `--profile` writes a Chrome-format trace of the Vulkan adapter's work,
including shader and pipeline creation, readable in `ui.perfetto.dev`.
[BENCHMARKING.md](BENCHMARKING.md) fixes the experiences to measure and
`scripts/summarize_profile_trace.py` builds the before and after table.
- Texture uploads no longer stall the frame they land on: ETC2 decode and
resampling run on a persistent worker pool.
- Less stutter while joining an experience and while textures load in. See
the [changelog](../CHANGELOG.md) for the numbers.
- The host main thread rests between engine polls instead of spinning, and
GameMode's core pinning is released, so Roblox's workers spread across
every core. See [PERFORMANCE.md](PERFORMANCE.md).
- Own app ID `io.github.CoderDayton.nightcap`, so Nightcap installs next to
upstream.
- AppImage and signed Flatpak releases on every `v*` tag, with the Flatpak
repo on GitHub Pages.

## Not planned

- Replacing the `mocktail` binary and config names. Keeping them makes
upstream merges clean.

