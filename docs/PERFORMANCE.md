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
those and lose only the pinning, turn pinning off for every game on the host:

```ini
# ~/.config/gamemode.ini
[cpu]
pin_cores=no
park_cores=no
```

GameMode reads `$XDG_CONFIG_HOME`, `/usr/share/gamemode` and `/etc`, in that
order. It does not read anything under Nightcap's own config directory, so this
file has to live in one of those three places. The daemon reads it when it
starts, so restart it after editing: `systemctl --user restart gamemoded`.

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
taskset -pc $(pgrep -x mocktail)
```

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

`MOCKTAIL_SMALL_TEXTURE_UPSCALE` (default `4`) redraws ETC2 textures of 64px or
smaller at N times their width and height. At the default that is 16 times the
pixels per affected texture.

The cost lands in two places. Decoding and resampling is CPU work on up to 8
threads while textures load, which shows up as a burst rather than a steady
load. The enlarged textures then stay in GPU memory for the session and cost
extra sampling bandwidth on every frame that uses them.

Set it to `1` to turn upscaling off:

```bash
MOCKTAIL_SMALL_TEXTURE_UPSCALE=1 mocktail
```

## Frame rate and present mode

`graphics.frame_rate_limit` (default `-1`) leaves Roblox's own in-game
framerate cap in charge; Nightcap sets no `DFIntTaskSchedulerTargetFps`
override. A fixed value forwards that number to the scheduler.

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
