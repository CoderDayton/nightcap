# Nightcap roadmap

What Nightcap is working toward, in priority order. Items move to
[Shipped](#shipped) when they land on `main`. Open an issue to propose a
change to this list.

## 1. Settings UI

Nightcap is configured by editing `~/.config/mocktail/config.yaml`. Most
people never open it, so they never find the FPS unlock or the vsync switch.

Goal: a small GTK4 settings window, reachable from the app menu and from a
`--settings` flag, that reads and writes the same `config.yaml`.

- Graphics tab: backend, frame rate limit, vsync, small texture upscale.
- Input tab: the `input.raw_mouse` toggle and controller options.
- Advanced tab: FFlag overrides with a plain text editor for `fflags.json`.
- Discord tab: every field in the `discord_rpc` block, so a friend can see
  what you are playing and you can switch it all off in one click.
- Changes apply on next launch. A restart button restarts Nightcap.

Done when every option in `config/mocktail.example.yaml` that a player would
touch can be set without a text editor.

## 2. Controller button glyphs

Upstream added Xbox and PlayStation gamepad support through SDL. Controllers
already hot-plug and release their held inputs when unplugged. One rough edge
is left.

- Correct button glyphs for the connected pad. `SDL_GetGamepadType` is only
  read for Xbox and PlayStation pads, so a Switch Pro, a GameCube pad, or any
  generic pad SDL reports as `SDL_GAMEPAD_TYPE_STANDARD` reaches Roblox as
  type 0 and draws generic glyphs. `SDL_GetRealGamepadType`, and the vendor
  and product IDs, are available and unused.

Done when a plugged-in pad shows its own buttons.

## 3. First-load stutter

The `--profile` trace now shows shader compiles and pipeline creation, so
the stutter on first entering an experience can be measured.

- Cut the stutter the trace shows, and prove it with a before and after
  trace per [BENCHMARKING.md](BENCHMARKING.md).

Done when entering a fresh experience no longer drops frames on shader
compiles.

## Shipped

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
- DEB, RPM, and AUR packages. Upstream ships those for unmodified Mocktail.
