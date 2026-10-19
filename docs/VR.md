# VR

Experimental OpenXR VR is opt-in: launch with `--vr`, or set `vr.mode` in
`config.yaml`:

- `off` (default) always starts on the desktop.
- `on` always starts in VR.
- `auto` starts in VR when SteamVR is already running with a connected
  headset, ALVR included, and on the desktop otherwise. It never starts
  SteamVR, so start SteamVR and connect the headset first.

`--vr` and `--no-vr` override `vr.mode` for one launch. The older
`vr.enabled: true`/`false` still works as `on`/`off`. Everything here only
applies while VR is active. The [README](../README.md#experimental-vr) covers
building and running.

## Menus and typing

The home menu uses Roblox's `More` button. The desktop side panel is turned
off in VR, because the VR pointer cannot click it.

In games, VR uses the same floating toolbar as the Quest app, and you can
drag it. Roblox only turns that toolbar on for Quest, so Nightcap sets
`FFlagLuaAppIECVREnabled8`, `FFlagEnableUIManagerPackgify8` and
`FFlagSpatialUIEnableDrag` to `True` in VR. A value you set for one of them in
`fflags.json` is used instead. To get the older bottom bar back, turn off
**Floating toolbar** on the VR tab of the settings window, or set
`vr.spatial_toolbar: false` in `config.yaml`.

When a text box takes focus (chat, search), a keyboard opens in front of you,
below eye level. Point at a key and pull the trigger; a letter types when you
let go, and each key gives a short buzz. While you point at the keyboard, the
trigger types and does not click the game behind it.

- **Layout.** A full US layout without the function row or Ctrl/Alt. Shift
  applies to the next key and selects its upper symbol; Caps Lock stays on
  until you tap it again.
- **Accents.** Hold a letter such as `e`, `a` or `n` for a moment, slide onto
  the accent you want and let go. Letting go on the key types the first one.
  `.`, `-`, `$`, `!`, `?` and quotes have alternates too.
- **Editing.** `Left` and `Right` move the cursor, and so does the thumbstick
  while you point at the keyboard. **Select all** and **Paste** work like
  Ctrl+A and Ctrl+V. Hold Backspace or an arrow to repeat it.
- **Moving it.** Grab the bar under the keyboard with the trigger and move
  it; it turns to face you. It opens in its usual spot the next time.

The keyboard icon closes the keyboard and leaves the text box. Set
`MOCKTAIL_VR_KEYBOARD=0` to turn the keyboard off.

## Environment variables

## Runtime and display

| Variable | Default | Effect |
| --- | --- | --- |
| `MOCKTAIL_VR_ENABLED` | `0` | Same as `--vr` (`1`) or `--no-vr` (`0`). |
| `MOCKTAIL_VR_RUNTIME` | `auto` | OpenXR runtime: `auto`, `system`, `wivrn`, `steamvr` or `alvr`. `XR_RUNTIME_JSON` overrides it. |
| `MOCKTAIL_VR_BACKEND` | `xr` | `native` keeps Roblox's stereo rendering without an OpenXR runtime, for debugging. |
| `MOCKTAIL_VR_FRAME_RATE_LIMIT` | `unlimited` | Frame-rate cap in VR, same values as `graphics.frame_rate_limit` (also `vr.frame_rate_limit`). With `unlimited` the headset's refresh rate paces frames. |
| `MOCKTAIL_VR_ANTI_ALIASING` | `fxaa` | Same as `vr.anti_aliasing`. Roblox draws the VR eyes without anti-aliasing, so each eye is filtered on its way to the headset (Vulkan only). `fxaa` is the cheapest; `smaa` finds edges more precisely and keeps text sharper; `off` sends them as rendered. |
| `MOCKTAIL_VR_RENDER_SCALE` | `1.0` | Scales each axis of the runtime's recommended eye size, `0.25` to `2.0`. |
| `MOCKTAIL_VR_UNPACED` | `0` | `1` stops waiting for the headset between frames, to measure the maximum frame rate. Frames past the headset refresh are not shown. Not for playing. |
| `MOCKTAIL_VR_DESKTOP_MIRROR` | `1` | `0` stops copying the left eye to the desktop window. |
| `MOCKTAIL_VR_MIRROR_SHM` | unset | A shared-memory name such as `/nightcap-mirror` publishes both eyes for an external viewer. The segment is private to your user. |
| `MOCKTAIL_VR_KEYBOARD` | `1` | `0` turns off the in-headset keyboard. |
| `MOCKTAIL_VR_SPATIAL_TOOLBAR` | `on` | Same as `vr.spatial_toolbar`. `off` keeps the older bottom bar in games. |

## Hands and scale

| Variable | Default | Effect |
| --- | --- | --- |
| `MOCKTAIL_VR_HAND_POSE_SOURCE` | `aim` | `grip` orients hands from the controller grip pose instead of the aim pose. |
| `MOCKTAIL_VR_HAND_PITCH_OFFSET` | `-20` | Degrees added to Roblox's own hand pitch. |
| `MOCKTAIL_VR_HAND_PITCH` | unset | Replaces the total hand pitch, in degrees. |
| `MOCKTAIL_VR_WORLD_SCALE` | `1.0` | Scales head, hand and eye offsets. `MOCKTAIL_VR_EYE_OFFSET_SCALE` is an older name for it. |

## Performance tracing

| Variable | Default | Effect |
| --- | --- | --- |
| `MOCKTAIL_VR_PERF` | unset | `1` logs per-stage frame timings and counters once a second. `present_interval` is the frame time; `desktop_mirror`, `copy_record` and `copy_fence_wait` show time the CPU spent on VR copies. |
| `MOCKTAIL_VR_MEASURE_START_NS`, `MOCKTAIL_VR_MEASURE_END_NS` | unset | Limit `MOCKTAIL_VR_PERF` to a `CLOCK_MONOTONIC` window. |
| `MOCKTAIL_VR_POSE_LOG_INTERVAL` | `240` | Frames between head-pose log lines. |

## Diagnostics builds

These only work in builds configured with `-DMOCKTAIL_VR_DIAGNOSTICS=ON`.
Release builds ignore them.

| Variable | Default | Effect |
| --- | --- | --- |
| `MOCKTAIL_VR_POSE_SOURCE` | `runtime` | `script` drives the head from a scripted motion instead of the headset, for testing without wearing it. |
| `MOCKTAIL_VR_POSE_AMPLITUDE_POSITION`, `_YAW`, `_PITCH` | `0.25`, `0.5`, `0.17` | Scripted motion size, in metres and radians. |
| `MOCKTAIL_VR_POSE_PERIOD_POSITION`, `_ROTATION` | `240`, `360` | Scripted motion period, in frames. |
| `MOCKTAIL_VR_XR_EVIDENCE_DIR` | unset | Writes submitted eye images to this directory. |
| `MOCKTAIL_VR_XR_EVIDENCE_INTERVAL` | `20` | Frames between evidence images. |

## Adding a Roblox build

VR needs addresses that are specific to each Roblox build. To use a supported
build that is not the active one, point `--roblox-lib` at its `libroblox.so`.

To add a build, run the offset finder on its library:

```bash
python3 scripts/find_roblox_vr_offsets.py --elf <libroblox.so> --output offsets.json
```

It needs the `capstone` Python package. Add its values to `kContracts` in
`src/vr/roblox_vr_device_bridge.cc`. Nightcap checks those addresses against
Roblox's code before it hooks anything, and leaves VR off if they do not
match. To run the same checks on real libraries, set
`MOCKTAIL_TEST_VR_PAYLOADS` to a folder of `<build>-<build id>/libroblox.so`
payloads and run `ctest -R VrSupportedBuilds`.

For a second opinion, solve the new build from the previous one and compare
the two answers. It needs the previous build's HostAbi profile and
compatibility manifest:

```bash
python3 scripts/roblox_abi_check.py --reference-lib <old libroblox.so> \
  --reference-profile <old profile.json> \
  --reference-compatibility config/roblox_compatibility.json \
  --candidate-lib <new libroblox.so> --vr
```

It lists every field where the finder and the solver disagree and exits 0
when they agree on all of them.

## Testing without a headset

[Monado](https://monado.freedesktop.org) can stand in for a headset. Its
`qwerty` driver gives a head and two controllers that you move with the
keyboard and mouse:

```bash
QWERTY_ENABLE=1 XRT_DEBUG_GUI=1 monado-service
```

Then, in another terminal:

```bash
XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json ./build/mocktail --vr
```

The keys work while Monado's debug window has focus. The simulated display is
small, so text in the headset view looks coarse.

Two tests need an OpenXR runtime and skip without one. To run them, start
Monado with no window and opt in:

```bash
XRT_COMPOSITOR_NULL=1 monado-service &
XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json MOCKTAIL_TEST_XR_GLES=1 \
  ctest --test-dir build -R GlesVrTransportTest.OpenXr
```
