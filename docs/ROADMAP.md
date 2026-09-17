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
- Discord tab: the `discord_rpc` block (see item 3).
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

## 3. Discord Rich Presence

Upstream ships Discord RPC. Nightcap turns it on by default; see the
[FAQ](../FAQ.md) to switch it off. It shows the experience name, elapsed
time, the experience icon, and a join button when the server is public.

- A Discord tab in the settings window to toggle each field.

Done when a friend can see what you are playing and join in one click, and
you can switch it all off in one click.

## 4. Performance profiling

Every performance change should come with numbers. Today they come with a
feeling.

Roblox's own stats overlays (Shift+F4, Shift+F5) already cover the game's
frame time, so this item measures only the layer Roblox cannot see.

- A `--profile` flag that writes a Chrome-format trace of the Vulkan
  adapter's work, readable in `ui.perfetto.dev` and `chrome://tracing`.
- Shader compile and pipeline cache tracing in that trace, so first-load
  stutter can be measured and cut.
- A short benchmark guide, [BENCHMARKING.md](BENCHMARKING.md), with a fixed
  set of experiences, so before and after numbers can be compared on one
  machine.

Done when a pull request that claims a speedup can show the trace that
proves it.

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
- Discord Rich Presence on by default.
- Custom Discord presence: title, details, state, and large and small icons
  from `config.yaml`, with `{place_name}` and `{place_icon}` placeholders.
- Own app ID `io.github.CoderDayton.nightcap`, so Nightcap installs next to
  upstream.
- AppImage and signed Flatpak releases on every `v*` tag, with the Flatpak
  repo on GitHub Pages.

## Not planned

- Replacing the `mocktail` binary and config names. Keeping them makes
  upstream merges clean.
- DEB, RPM, and AUR packages. Upstream ships those for unmodified Mocktail.
