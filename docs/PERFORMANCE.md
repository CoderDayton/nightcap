# Performance and CPU load

What Nightcap does to your CPU and GPU, which knobs change it, and how to
measure rather than guess.

## GameMode core pinning

Nightcap asks Feral GameMode for a performance session at startup
(`performance.gamemode`, default `auto`). GameMode raises the CPU governor and
the process priority, which helps.

On a CPU with a wide spread of per-core maximum frequencies — Intel P-core and
E-core parts, AMD parts with preferred cores — GameMode also pins the process
to the fastest cores. On a 32-thread host that can mean 4 usable threads out of
32. Roblox spreads its work across a worker pool, so the pin concentrates that
pool onto a couple of physical cores instead of spreading it. Those cores sit
near their boost ceiling and run hot while the rest of the chip idles.

GameMode re-applies the pin roughly every 5 seconds, so widening the mask from
inside the process does not hold. Releasing the request does release the pin.

Under `auto`, Nightcap therefore checks its CPU affinity immediately after the
request. If GameMode narrowed it, Nightcap ends the request and prints:

```
  [gamemode] released: it pinned this process to a subset of the CPUs, ...
```

The trade is that GameMode's governor and priority boost go with it. To keep
those and lose only the pinning, turn pinning off for every game on the host.

These are the settings to run with:

```ini
# ~/.config/gamemode.ini
[general]
desiredgov=performance

[cpu]
pin_cores=no
park_cores=no
```

`desiredgov` is the one to check first. The packaged default in
`/usr/share/gamemode/gamemode.ini` is already `performance`, but a value in
your own file replaces it, and `desiredgov=powersave` there means GameMode
clocks the CPU **down** for the whole session while still reporting
`[gamemode] performance request active`. The symptoms are a low CPU clock
under load, a GPU that never leaves its lower SM clocks, and the engine pump
falling back to `tpause` — which costs about 70% of a core instead of 3%,
because `sleep` is only chosen when the governor reads `performance`. On one
13900K host, correcting `powersave` to `performance` took the process from
177% CPU to 42% and the package from 70°C to 53-63°C, at the same frame rate.

GameMode reads `$XDG_CONFIG_HOME`, `/usr/share/gamemode` and `/etc`, in that
order. It does not read anything under Nightcap's own config directory, so this
file has to live in one of those three places. The daemon reads it when it
starts, so restart it after editing: `systemctl --user restart gamemoded`.

Restarting the daemon drops the session of any game already running; relaunch
the game for new settings to apply to it.

`gamemoded` is D-Bus activated through
`com.feralinteractive.GameMode.service`, so `systemctl --user stop gamemoded`
does not hold — the next client request starts it again within seconds. To
measure with GameMode genuinely off, mask it:

```bash
systemctl --user mask gamemoded     # and --unmask afterwards
```

The governor switch runs through polkit. Debian and Ubuntu only allow it for
members of the `gamemode` group; without that, the daemon's journal shows
`cpugovctl set performance: Not authorized` and the governor stays put while
the session is otherwise active. Add yourself and log in again:

```bash
sudo usermod -aG gamemode "$USER"
```

Fedora and Arch allow any logged-in local user.

Setting `performance.gamemode: on` accepts the pinning instead: `on` is an
explicit request for GameMode, and Nightcap keeps the session whatever it does
to the affinity. `off` never contacts the daemon.

To see which CPUs the process may use:

```bash
pid=$(pgrep -x Main)
for t in /proc/$pid/task/*; do taskset -pc "${t##*/}"; done
```

Two traps here. The engine process reports its name as `Main`, not
`mocktail`; `pgrep -x mocktail` finds only the launcher, which sits near 0%
CPU and tells you nothing. And `taskset -p` reads one thread, not the whole
process, so a single call reports the main thread's mask and says nothing
about the workers — which is where the game's load actually is.

For the same reason, `ps -L -o psr=` shows which core each thread is running
on right now, not which cores it is allowed on. On a hybrid CPU the scheduler
preferring P-cores for runnable work is normal and is not a mask.

## The main-thread pump

Roblox posts its per-frame main-thread step at a time of its own choosing,
and nothing signals the host when it does. The host main thread therefore
polls `nativeCallMessagesFromMainThread`, and a post that waits more than a
fraction of a millisecond costs a whole frame. Between polls the thread rests
for 100 µs, and how it rests decides the CPU clock:

| Mode | When | What it costs |
| --- | --- | --- |
| `sleep` | The governor is `performance`, which GameMode sets | The thread idles at about 3% of a core. The governor holds the clock. |
| `tpause` | Otherwise, on a CPU with WAITPKG (Intel 12th gen and newer) | The core stays awake in a light C0 state. Frame rate holds; the OS books the time as busy, so the thread shows around 70% of a core, at lower power than a spin. |
| `spin` | Otherwise | The core stays awake at full power. |

Under a power-saving governor the CPU clocks a bursty core down unless some
core stays awake, and Roblox's render thread then runs slower and drops
frames. That is why `sleep` is not the default there. Startup prints the
chosen mode:

```
  [main] engine pump rest: tpause (MOCKTAIL_ENGINE_PUMP_REST=sleep|tpause|spin)
```

`MOCKTAIL_ENGINE_PUMP_REST` forces a mode. `sleep` without the governor's help
runs cooler for a few frames per second less; measure before keeping it.

## Small texture upscaling

ETC2 colour textures of 64px or smaller get two sharper levels above their
own, 4× upscaled from the texture's largest level. A desktop screen draws
these, such as crosshairs, cursors and icons, larger than a phone does. The
texture's own levels keep their size and the texels Roblox sent, so the card
picks them at normal draw size and the sharper ones only when the texture is
drawn larger. 4× is 16 times the pixels per affected texture: a 64px texture
grows from 16 KiB to 256 KiB uncompressed.

Upscaled textures share a budget of an eighth of the card's video memory,
as Vulkan reports it: 256 MiB on a 2 GB card, 3 GiB on a 24 GB one. Roblox
sizes its own texture budget for the textures it sends, so it cannot see
this cost. Past the budget a new small texture steps down to 2×, then to
its own size, so it stays as sharp as memory allows:

- Textures of 32px or less, such as crosshairs and cursors, cost a quarter
  of a 64px one or less, and keep 4× until the budget is full. Larger ones
  take 2× once half of it is used, unless the driver reports twice the
  share below free.
- Where the driver reports free video memory (`VK_EXT_memory_budget`, read
  at most every 250 ms), an upscale must also leave a tenth of video memory
  free, and at least 96 MiB, which counts what Roblox and other programs
  use. 96 MiB covers what a loading burst adds between two reads.
- Destroyed textures give their share back, so later ones get 4× again.
- The upscale an override file asks for is not limited, but counts toward
  the budget.

The session log says the first time textures step down to each size.
`MOCKTAIL_ETC2_VIDEO_MEMORY_MB` sets the video memory for one run, so a large
card acts as a small one; the driver's free memory is then not read.

A texture of 64px or less with an override file is sized at least to its
PNG: the smaller axis's ratio, rounded down to a power of two, up to 8×. A
128px override for a 64px texture shows at 2×. The factor applies to every
colour texture of that size, not only the one the override replaces, which
is why larger textures don't take it. The session log names each size once.

`MOCKTAIL_SMALL_TEXTURE_UPSCALE` sets the factor for one run: 1 turns it off
for textures without an override, and 2, 4 or 8 apply, with other numbers
rounded down to a power of two. There is no config setting; an old
`graphics.small_texture_upscale` is ignored.

At startup Nightcap times the card's BC7 encoder on a few textures, minus the
round trip of an empty submit, in both its full and fast modes. An RTX 4090
measures about 9 ns per block, and about 5 ns fast; llvmpipe measures about
700 ns. Half the timed blocks are opaque and half have alpha, since the fast
encoder saves more on alpha blocks. With texture compression off there is no
encode to time, so it is skipped.

The encode budget aims BC encoding at a tenth of wall time, with a 16 ms
burst. Creating a texture prepays its first upload; later uploads, re-uploads
included, are charged as they are recorded, and a texture destroyed before
its upload gives its prepayment back. When the budget runs dry, new textures
are still compressed, with the fast encoder: one least-squares fit per BC7
mode and fewer modes tried. That cuts the GPU cost by about 40% and loses
under 1 dB on random blocks. It is not a hard cap: a burst of new textures
still encodes in the frames that load them, only cheaper. Those textures
keep the fast encode for their life. The budget goes into debt for the fast
cost, at most four bursts deep, so it refills within 0.64 s of a burst.
`MOCKTAIL_ETC2_ENCODE_BUDGET=0`, or `graphics.smooth_texture_loading: false`
(Smooth Texture Loading in Settings), turns the budget off and skips the
startup measurement; the log then says `[vulkan] BC encode budget off`.

The session log shows the measured speed once:

```text
[vulkan] BC encode speed: transcode 10.0 ns/block (fast 4.9), upscale 8.9 ns/block; small texture upscale 4x; encode budget on
```

It also shows, every 5 seconds while textures are created, how many were
transcoded at 1x and how many upscaled, each split by the image's largest
side, how many upscales got each factor, how many used the fast encoder, and
the estimated GPU time with the upscaling share. For example (the numbers
are made up):

```text
[vulkan] BC encodes over 5.0 s: transcode 350 (<=64px 300, <=256px 40, >256px 10), upscale 40 (<=64px 30, <=256px 10, >256px 0; 4x 30, 2x 10); fast 22; est 38.1 ms GPU (upscale 9.4 ms)
```

At the first texture and then every 5 seconds while textures are created, it
shows what emulated textures take in GPU memory, how much of that is
upscaling against its budget and the driver's free video memory, how many
small textures got each scale, and how many ETC2 blocks stay in system
memory for later copies:

```text
[vulkan] ETC2 textures: 412.3 MiB on the GPU, 38.1 MiB of it upscaled (budget 256.0 MiB, 640.2 MiB free; 4x 310, 2x 42, 1x 0); 12.4 MiB of blocks kept on the CPU
```

Upscaling adds no new detail. Textures grow with a sharpened bicubic filter,
and texels between just two flat colours stay nearest-neighbour so hard edges
stay hard. The extra size matters most when a replacement PNG in
`~/.config/mocktail/textures` has more detail than the original.

The cost lands in two places. With GPU decode on, the graphics card decodes
and scales them, and on cards with BC support keeps them compressed as BC7.
With GPU decode off, or for textures an override may replace, decoding and
resampling is CPU
work on up to 8 threads while textures load, which shows up as a burst rather
than a steady load. The enlarged textures then stay in GPU memory for the
session and cost
extra sampling bandwidth on every frame that uses them. In Blade Ball the
memory share is small: decoded textures came to 5.1 times their ETC2 size both
with upscaling at 4 and with it off. The `emulated texture (MB)` and
`compressed texture (MB)` counters in a `--profile` trace give the figures for
any game.

That decode does not run on the thread that submits the frame. On a device
with a timeline semaphore, uploads decode on a background dispatcher and the
submit carries a semaphore wait, so the GPU waits for the texture instead of
the render thread. In the trace this is the `etc2 decode` slice on its own
thread, with only a negligible `etc2 gather` left on the submitting one. A
device without that feature decodes inline as before.

Set it for one run:

```bash
MOCKTAIL_SMALL_TEXTURE_UPSCALE=1 mocktail
```

## Native code in the menus

The home screen and avatar editor run Roblox's Lua app in the Luau interpreter,
and its long frames come from that thread. PC profiles set
`FFlagDebugLuauCodegenAll`, which compiles every script to native code. That
makes the menus' Lua about 2.5 times cheaper for about 56 MB of compiled code.

In a game the same flag compiles every game script too: Blade Ball grew from 7
MB to about 300 MB and hit Roblox's native code limit, which shows warnings in
F9. So Nightcap turns the flag off when a game starts joining and back on when
you return to the menus. Roblox reads the flag each time it compiles a script,
and a game's compiled code is freed when you leave it. Games run with Roblox's
own per-script choice, the same as without the flag.

The log shows each switch:

```text
[luau-codegen] menus-only switch at +0x<offset in libroblox.so>
[luau-codegen] native code for all scripts off
[luau-codegen] native code for all scripts on
```

Setting `FFlagDebugLuauCodegenAll` in `fflags.json` turns the switching off
and your value applies everywhere. To keep the interpreter in the menus too:

```json
{
  "FFlagDebugLuauCodegenAll": false
}
```

## Frame rate and present mode

`graphics.frame_rate_limit` (default `-1`) leaves Roblox's own in-game
framerate cap in charge; Nightcap sets no `DFIntTaskSchedulerTargetFps`
override. A fixed value up to 240 forwards that number to the scheduler.
Roblox stops the scheduler flag at 240, so `unlimited` and higher values write
`FramerateCap` into Roblox's `GlobalBasicSettings_13.xml` at launch instead
(`unlimited` writes 9999).

In VR, `vr.frame_rate_limit` (`MOCKTAIL_VR_FRAME_RATE_LIMIT`) replaces
`graphics.frame_rate_limit` and takes the same values. Its default is
`unlimited`: the scheduler target goes to 240 and `xrWaitFrame` holds each
frame to the headset's refresh rate. Set `-1` to restore Roblox's own cap, or a
number such as `90` to pin it.

`MOCKTAIL_VR_RENDER_SCALE` (default `1.0`, range `0.25`–`2.0`) scales each
axis of the OpenXR runtime's recommended eye size. SteamVR's recommendation
already includes its own supersampling, so on a Quest 2 at 90 Hz it is about
2244x2352 per eye. If a frame misses the 11.1 ms budget, the runtime halves
the rate to 45 FPS; `0.75` renders about 56% of the pixels.

`MOCKTAIL_VR_UNPACED=1` is a benchmark mode. Roblox no longer waits for the
headset between frames; each XR frame stays open across several Roblox frames
and is submitted with the newest eyes once per display period. Pair it with a
fixed scheduler target above 240 and `MOCKTAIL_VSYNC=off` to find the
maximum rate:

```bash
MOCKTAIL_VR_UNPACED=1 MOCKTAIL_VR_FRAME_RATE_LIMIT=9999 MOCKTAIL_VSYNC=off mocktail --vr
```

Frames beyond the headset refresh are never displayed, and they all reuse the
open frame's head pose, so expect judder. Do not play in this mode.

`graphics.vsync` (default `auto`) selects the presentation mode. `auto` and
`on` both pick the lowest-latency synchronized mode the driver offers, in
order: `FIFO_LATEST_READY`, `MAILBOX`, `FIFO_RELAXED`, `FIFO`. The first two do
not block the render thread on the display refresh, so the frame rate is
governed by the in-game cap rather than by presentation. `off` selects
`IMMEDIATE` and does not synchronize at all.

If the frame rate is higher than your refresh rate, the extra frames are work
you cannot see. Lower the in-game cap to your refresh rate first.

## Measuring

Shift+F4 in game shows Roblox's own scheduler jobs, with a millisecond cost and
a percentage for each. It does not show Nightcap's threads — the Vulkan
adapter, the ETC2 decode workers, the libc shim — so it is a reading on the
game, not on the process.

For the whole process:

```bash
top -H -p $(pgrep -x mocktail)      # per-thread CPU, sorted
ps -L -o psr=,pcpu=,comm= -p $(pgrep -x mocktail)   # which core each thread is on
nvidia-smi --query-gpu=utilization.gpu,temperature.gpu,power.draw --format=csv
```

A thread pinned near 100% while the frame rate is steady is a busy-wait. The
one exception is the host main thread in `tpause` mode, which the OS reports
as busy while the core is parked (see above). Load spread across many
`RBX Worker` threads is the game's own scheduler, and its cost tracks the
place you are in — part count, moving parts and joints all show in the
Shift+F4 World section.

`--profile <file>` writes a Chrome trace of the Vulkan adapter's work for
ui.perfetto.dev. See [BENCHMARKING.md](BENCHMARKING.md).
