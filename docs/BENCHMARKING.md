# Benchmarking

A pull request that claims a speedup shows before and after numbers taken
the way this guide describes. Numbers from different machines are not
compared with each other. Each run is compared with a run on the same
machine.

## Two layers, two tools

Roblox measures its own work. Nightcap measures the layer between Roblox and
your driver.

| Layer | What it covers | Tool |
| --- | --- | --- |
| Roblox | Scripts, physics, rendering jobs, network | Roblox's stats overlays, Shift+F4 and Shift+F5 in game |
| Nightcap | Vulkan adapter: present, swapchain and GPU waits, submits, ETC2 texture decode, pipeline and shader creation | `--profile` |

A Nightcap change should move the `--profile` numbers. If it only moves the
Roblox overlay numbers, say so in the pull request, since the cause is then
less direct.

## Recording a trace

```sh
mocktail --profile ~/nightcap-before.json
```

The Vulkan adapter writes a Chrome-format trace to that file, a few times a
second. The file is valid JSON after every write, so a session that crashes
or is killed still leaves a readable trace. Exit from the Roblox menu when you are done. Open the file
in [ui.perfetto.dev](https://ui.perfetto.dev) or `chrome://tracing`.

Expect about 130 bytes per event, around 15 MB per minute at 240 FPS.
Profiling is off unless `--profile` is given, and `MOCKTAIL_PROFILE_TRACE`
does the same thing as the flag.

### What the trace contains

Each slice is named after the call it times, on the thread that made it.

| Category | Slices | Arguments |
| --- | --- | --- |
| `present` | `vkQueuePresentKHR`: the whole adapter present, including the text overlay | `result` |
| `present` | `host present`: the host driver's present, nested inside the slice above | |
| `wait` | `vkAcquireNextImageKHR`, `vkAcquireNextImage2KHR`, `vkWaitForFences`, `vkWaitSemaphores`, `vkWaitSemaphoresKHR`, `vkQueueWaitIdle`, `vkDeviceWaitIdle` | |
| `submit` | `vkQueueSubmit`, `vkQueueSubmit2`, `vkQueueSubmit2KHR` | `submits` |
| `texture` | `etc2 decode`: CPU decode of ETC2 textures. On a device with a timeline semaphore this runs on the decode dispatcher, not the submitting thread | `uploads`, `compressed_bytes`, `decoded_bytes` |
| `texture` | `etc2 gather`: the part of an asynchronous upload that stays on the submitting thread. Absent when the decode ran inline | `uploads`, `compressed_bytes`, `decoded_bytes`, `ticket` |
| `texture` | `etc2 staging create`: creating a staging block for ETC2 uploads, when no block has room left | `bytes` |
| `texture` | `etc2 staging destroy`: destroying empty staging blocks beyond the four kept idle | `buffers` |
| `texture` | `etc2 release wait`: a command buffer reset waiting for its uploads' decode. Only waits of 0.1 ms or more are recorded | `ticket` |
| `memory` | `vkAllocateMemory` | `bytes`, `type` (memory type index), `result` |
| `memory` | `vkFreeMemory`, `vkUnmapMemory`: both include waiting for ETC2 decodes that still read that memory | |
| `memory` | `vkMapMemory`, `vkCreateImage`, `vkDestroyImage` | |
| `pipeline` | `vkCreateGraphicsPipelines`, `vkCreateComputePipelines` | `count`, `cache` (1 when a pipeline cache was passed), `result` |
| `pipeline` | `vkCreateShaderModule` | `bytes`, `result` |
| `pipeline` | `vkCreatePipelineCache` | `initial_bytes`, `result` |
| `pump` | `nativeCallMessagesFromMainThread`: the engine's main-thread step, on the host main thread. Only calls of 20 µs or more are recorded; the empty polls between them are not. | `cpu_us`: CPU time the main thread used during the call. Far below the slice's duration means the step spent it waiting |

The `frame interval (ms)` counter is the time between successive adapter
presents. `vkQueuePresentKHR` time minus `host present` time is the adapter's
own cost per frame.

Two more counters cover the same interval. `present thread cpu (ms)` is the
CPU time the thread that presents used in it, and is absent for a frame
presented from a different thread than the one before. `process cpu (ms)` is
the CPU time of every thread together, so dividing it by the frame interval
gives the number of cores busy. A long frame with little present-thread CPU
was spent waiting.

## The benchmark runs

Run every scenario before and after the change, on the same build type
(`make build`), with the same `config.yaml`. Close other GPU-heavy apps.
Record `graphics.frame_rate_limit` and `graphics.vsync` in the pull request,
because a frame cap hides frame time gains.

Record `~/.config/gamemode.ini` too, and do not change it between the before
and after runs. `desiredgov` there decides the CPU governor for the whole
session and therefore which engine pump mode is chosen, which moves process
CPU by more than most changes under test. See
[PERFORMANCE.md](PERFORMANCE.md).

Frame interval p99 varies by 2 ms or more between identical runs, so a
difference that size is not a result. Three runs per scenario is the minimum
for any frame-time claim; slice totals for a specific subsystem are far less
noisy and can be read from fewer.

| Scenario | How to start | What to do | What it shows |
| --- | --- | --- | --- |
| Home screen, cold | `mocktail --profile home-cold.json` with an empty shader cache (below) | Wait 30 s on the home screen, exit | First-load stutter: `pipeline` and `texture` slices |
| Tower of Hell | `mocktail --profile toh.json --launch-uri "roblox://experiences/start?placeId=1962086868"` | Stand still at spawn for 60 s, exit | Steady-state frame time on a light scene |
| Murder Mystery 2 | `mocktail --profile mm2.json --launch-uri "roblox://experiences/start?placeId=142823291"` | Stay in the lobby for 60 s, exit | Frame time with many players and a busier scene |

For the cold run, point the driver's shader cache at an empty directory, so
your normal cache is left alone:

```sh
cache=$(mktemp -d)
MESA_SHADER_CACHE_DIR="$cache" __GL_SHADER_DISK_CACHE_PATH="$cache" \
  mocktail --profile home-cold.json
```

`MESA_SHADER_CACHE_DIR` applies to Mesa drivers (AMD, Intel) and
`__GL_SHADER_DISK_CACHE_PATH` to the NVIDIA driver.

Run each scenario three times and keep the median run, since one run can
catch a background stall.

## Comparing runs

```sh
scripts/summarize_profile_trace.py toh-before.json toh-after.json
```

This prints a Markdown table with one column per trace: frame count, frame
interval mean, p50, p95, p99 and max, then count, total, mean, p99 and max
for every slice name, largest total first. Paste the table into the pull
request and attach both traces.

For frame time, compare p95 and p99 as well as the mean. Stutter shows up in
the tail first. For first-load work, compare the `total (ms)` rows of the
`pipeline` and `texture` slices from the cold run.
