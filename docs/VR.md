# VR

Experimental OpenXR VR is opt-in: launch with `--vr`, or set `vr.enabled: true`
in `config.yaml`. Everything here only applies while VR is active. The
[README](../README.md#experimental-vr) covers building and running.

## Menus and typing

The home menu uses Roblox's `More` button. The desktop side panel is turned
off in VR, because the VR pointer cannot click it.

When a text box takes focus (chat, search), a keyboard opens in front of you,
below eye level. It is a full US layout without the function row or
Ctrl/Alt. Point at a key and pull the trigger. While you point at the keyboard,
the trigger types and does not click the game behind it. Hold Backspace to
delete repeatedly. Shift applies to the next key and selects its upper symbol;
Caps Lock stays on until you tap it again. Close dismisses the keyboard and
leaves the text box. Set `MOCKTAIL_VR_KEYBOARD=0` to turn the keyboard off.

## Environment variables

## Runtime and display

| Variable | Default | Effect |
| --- | --- | --- |
| `MOCKTAIL_VR_ENABLED` | `0` | Same as `--vr` (`1`) or `--no-vr` (`0`). |
| `MOCKTAIL_VR_RUNTIME` | `auto` | OpenXR runtime: `auto`, `system`, `wivrn`, `steamvr` or `alvr`. `XR_RUNTIME_JSON` overrides it. |
| `MOCKTAIL_VR_BACKEND` | `xr` | `native` keeps Roblox's stereo rendering without an OpenXR runtime, for debugging. |
| `MOCKTAIL_VR_FRAME_RATE_LIMIT` | `unlimited` | Frame-rate cap in VR, same values as `graphics.frame_rate_limit` (also `vr.frame_rate_limit`). With `unlimited` the headset's refresh rate paces frames. |
| `MOCKTAIL_VR_RENDER_SCALE` | `1.0` | Scales each axis of the runtime's recommended eye size, `0.25` to `2.0`. |
| `MOCKTAIL_VR_UNPACED` | `0` | `1` stops waiting for the headset between frames, to measure the maximum frame rate. Frames past the headset refresh are not shown. Not for playing. |
| `MOCKTAIL_VR_DESKTOP_MIRROR` | `1` | `0` stops copying the left eye to the desktop window. |
| `MOCKTAIL_VR_MIRROR_SHM` | unset | A shared-memory name such as `/nightcap-mirror` publishes both eyes for an external viewer. The segment is private to your user. |
| `MOCKTAIL_VR_KEYBOARD` | `1` | `0` turns off the in-headset keyboard. |

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
| `MOCKTAIL_VR_PERF` | unset | `1` logs per-stage frame timings and counters once a second. |
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
