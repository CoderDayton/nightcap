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
- Input tab: raw mouse and controller options (see item 2).
- Advanced tab: FFlag overrides with a plain text editor for `fflags.json`.
- Discord tab: the `discord_rpc` block (see item 3).
- Changes apply on next launch. A restart button restarts Nightcap.

Done when every option in `config/mocktail.example.yaml` that a player would
touch can be set without a text editor.

## 2. Controller and raw mouse input

Upstream added Xbox and PlayStation gamepad support through SDL. Desktop play
still has rough edges.

- Raw mouse input in first-person games, with no acceleration or clamping.
- Better controller support: hot-plug without a restart, correct button
  glyphs for the connected pad, and no stuck inputs after a disconnect.

Done when a mouse feels like a mouse and a plugged-in controller just works.

## 3. Discord Rich Presence

Upstream ships Discord RPC. Nightcap turns it on by default; see the
[FAQ](../FAQ.md) to switch it off. It shows the experience name, elapsed
time, the experience icon, and a join button when the server is public.

- Show the current server region and player count when Roblox exposes them.
- Make the join button work for private servers the player has a link for.
- A Discord tab in the settings window to toggle each field.

Done when a friend can see what you are playing and join in one click, and
you can switch it all off in one click.

## 4. Performance profiling

Every performance change should come with numbers. Today they come with a
feeling.

- A frame time overlay, toggled by a key and a config flag, showing CPU time,
  GPU time, and present latency.
- Shader compile and pipeline cache tracing, so first-load stutter can be
  measured and cut.
- A `--profile` flag that writes a trace file readable by Perfetto.
- A short benchmark guide in `docs/` with a fixed set of experiences, so
  before and after numbers can be compared across machines.

Done when a pull request that claims a speedup can show the trace that
proves it.

## Shipped

- Lower input latency: frames are presented as soon as they are ready.
- Small textures upscaled 4x by default, with PNG overrides in
  `~/.config/mocktail/textures`.
- ETC2 textures decoded on desktop GPUs without stutter.
- No more error 319 kicks after joining a server.
- Roblox sees the real host RAM and screen size.
- Desktop launcher that works on Wayland and finds the installed binary.
- Discord Rich Presence on by default.
- Own app ID `io.github.CoderDayton.nightcap`, so Nightcap installs next to
  upstream.
- AppImage and signed Flatpak releases on every `v*` tag, with the Flatpak
  repo on GitHub Pages.

## Not planned

- Replacing the `mocktail` binary and config names. Keeping them makes
  upstream merges clean.
- DEB, RPM, and AUR packages. Upstream ships those for unmodified Mocktail.
