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
| `texture` | `etc2 decode`: CPU decode of ETC2 textures before a submit | `uploads`, `compressed_bytes`, `decoded_bytes` |
| `pipeline` | `vkCreateGraphicsPipelines`, `vkCreateComputePipelines` | `count`, `cache` (1 when a pipeline cache was passed), `result` |
| `pipeline` | `vkCreateShaderModule` | `bytes`, `result` |
| `pipeline` | `vkCreatePipelineCache` | `initial_bytes`, `result` |

The `frame interval (ms)` counter is the time between successive adapter
presents. `vkQueuePresentKHR` time minus `host present` time is the adapter's
own cost per frame.

## The benchmark runs

Run every scenario before and after the change, on the same build type
(`make build`), with the same `config.yaml`. Close other GPU-heavy apps.
Record `graphics.frame_rate_limit` and `graphics.vsync` in the pull request,
because a frame cap hides frame time gains.

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
