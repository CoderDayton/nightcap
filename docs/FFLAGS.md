# FFlags guide

FFlags are hidden Roblox settings. You don't need them to play; Nightcap
already picks good values. Use them for things the menus don't offer, like
an FPS counter or no grass.

- [Quick start](#quick-start)
- [Recommended flags](#recommended-flags)
- [Leave these alone](#leave-these-alone)
- [Flags that no longer work](#flags-that-no-longer-work)
- [Big flag lists](#big-flag-lists)
- [Flags Nightcap sets](#flags-nightcap-sets)
- [Something broke](#something-broke)

## Quick start

1. Create the file:

   | Install | File |
   |---|---|
   | AppImage, AUR, DEB, RPM, pacman, source | `~/.config/mocktail/fflags.json` |
   | Flatpak | `~/.var/app/io.github.CoderDayton.nightcap/config/mocktail/fflags.json` |

   Or use **Advanced → FFlag overrides** in the settings window
   (`mocktail --settings`, or
   `flatpak run io.github.CoderDayton.nightcap --settings`).

2. Add flags:

   ```json
   {
     "FFlagDebugDisplayFPS": true,
     "FIntFRMMaxGrassDistance": 0,
     "FIntFRMMinGrassDistance": 0
   }
   ```

   Commas go after every line except the last. Names are case-sensitive.

3. Restart Nightcap.

**Values:** `FFlag`/`DFFlag` take `true` or `false`. `FInt`/`DFInt` take a
whole number. `FString`/`DFString` take text in quotes. Decimals aren't
accepted.

**Check it worked:** add `"FStringDebugShowFlagState": "DebugDisplayFPS"`,
listing flag names without the `FFlag`/`DFInt` part, separated by commas.
Roblox shows those flags and their values in the top-left corner. A flag
that isn't shown has a typo or no longer exists.

## Recommended flags

All of these exist in the Roblox version Nightcap runs (2.742), and each
was started in Nightcap to check that it loads cleanly. In-game effects
weren't tested.

✅ safe for most people · ⚠️ only in the case shown · ❌ don't

### Starter set

FPS counter, no menu blur, less CPU in the background:

```json
{
  "FFlagDebugDisplayFPS": true,
  "FIntRobloxGuiBlurIntensity": 0,
  "DFFlagReduceCPUWhenBG": true
}
```

On a weak PC, also add:

```json
  "DFIntDebugFRMQualityLevelOverride": 1,
  "FIntFRMMaxGrassDistance": 0,
  "FIntFRMMinGrassDistance": 0
```

### Graphics

| Flag | What it does | Use it? |
|---|---|---|
| `DFIntDebugFRMQualityLevelOverride` | Locks graphics quality, `1` (lowest) to `21` (highest). | ✅ |
| `FIntDebugForceMSAASamples` | Smooths jagged edges. `0` off, up to `4`. Higher causes visual bugs. | ✅ |
| `DFFlagTextureQualityOverrideEnabled` + `DFIntTextureQualityOverride` | Texture sharpness, `0` to `3` (default). Set the first to `true`. | ✅ `2` or lower on 4 GB graphics cards |
| `FIntFRMMaxGrassDistance` + `FIntFRMMinGrassDistance` | Both `0` removes grass. | ✅ |
| `FIntGrassMovementReducedMotionFactor` | `0` stops grass swaying. | ✅ |
| `DFIntCSGLevelOfDetailSwitchingDistance` (and `L12`, `L23`, `L34`) | Lower makes far objects simpler sooner. Faster, blockier. Defaults `250`, `500`, `2000`, `3000`. | ⚠️ Weak PCs |
| `DFFlagDebugPauseVoxelizer` | Stops lighting updates. Faster, but light and shadows can be wrong. | ⚠️ Weak PCs |
| `DFFlagDebugForceAnisoOff` | Floors look blurrier further away. Slightly faster. | ⚠️ Weak PCs |
| `FIntDebugTextureManagerSkipMips` | Drops texture detail to save graphics memory. `1`–`2` is mild. | ⚠️ Low graphics memory |
| `FIntTerrainArraySliceSize` | Terrain texture detail. Default `256`. Lower uses less memory. | ⚠️ Low graphics memory |
| `FIntRenderShadowmapBias` | Shifts shadows to hide stripes. Default `75`. | ⚠️ Striped shadows |
| `FFlagDebugSkyGray` | Nightcap turns it on for custom skies. `false` lets games keep their own. | ⚠️ Custom sky users |
| `DFIntDebugDynamicRenderKiloPixels` | Lowers render resolution. Blurry. | ❌ Last resort |
| `FFlagDebugGraphicsPreferOpenGL`, `…PreferVulkan` | Nothing useful here. Use **Graphics backend** in settings. | ❌ |

### Performance

| Flag | What it does | Use it? |
|---|---|---|
| `FFlagDebugDisplayFPS` | FPS counter in the top-left. `true` | ✅ |
| `DFFlagReduceCPUWhenBG` | Less CPU while Roblox is in the background. `true` | ✅ |
| `DFFlagEnableMeshPreloading`, `DFFlagEnableTexturePreloading`, `DFFlagEnableSoundPreloading` | Loads what a game asks for up front. Less pop-in, longer loading, more RAM. | ⚠️ 16 GB RAM or more |
| `DFIntAnimationLodThrottlerDistanceMin` + `…Max` | Distance where far-away characters animate less often. Defaults `180` and `380`. Higher is smoother, costs CPU. | ⚠️ Strong CPUs |
| `DFIntAnimatorThrottleMaxFramesToSkip` + `DFIntAnimationLodThrottleMaxFramesToSkip` | `0` stops far animations skipping frames. Defaults `4` and `8`. Costs CPU. | ⚠️ Strong CPUs |
| `DFIntMaxFrameBufferSize` | Frames queued ahead. Causes stutter or input lag. | ❌ |
| `DFIntTaskSchedulerTargetFps` | Frame rate limit. A different value stops Nightcap starting. | ❌ Use **Frame rate limit** in settings |

### Menus and sound

| Flag | What it does | Use it? |
|---|---|---|
| `FIntRobloxGuiBlurIntensity` | `0` turns off the blur behind the Roblox menu. | ✅ |
| `FStringDebugShowFlagState` | Shows flag values on screen. | ✅ While testing |
| `DFFlagDisableDPIScale` | Smaller menus on high-resolution screens. | ⚠️ Taste |
| `FIntFontSizePadding` | More space around menu text. | ⚠️ Taste |
| `FIntScrollWheelDeltaAmount` | Scroll speed in menus. | ⚠️ Taste |
| `DFIntVoiceChatVolumeThousandths` | Voice chat volume. `1000` is normal. | ⚠️ Taste |
| `DFIntMaxLoadableAudioChannelCount` | Fewer sounds at once. Sounds cut out. | ❌ |

### Usage stats

These stop Roblox sending some usage and network stats. Nothing changes in
game. Set each to `0`, or `true` for the first.

`DFFlagDisableFastLogTelemetry`, `DFIntHttpInfluxHundredthsPercentage`,
`DFIntClientNetworkInfluxHundredthsPercentage`,
`DFIntRaknetBandwidthInfluxHundredthsPercentageV2`,
`DFIntParallelLuaStatsEventHundredthsPercent`,
`DFIntHttpGAFailureReportPercent`,
`DFIntHttpStatsErrorInfluxReportsPerMillion`,
`DFIntHttpStatsDnsErrorInfluxReportsPerMillion`,
`DFIntHttp2StatsHundredthsPercent`.

## Leave these alone

These mostly cause lag, broken movement or disconnects. Ones that give an
advantage, like camera zoom or desync, can count as cheating.

- **Movement and physics:** names containing `Physics`, `Humanoid`, `Sim`,
  `Gjk`, `Unstick`, `Freefall`, `Locomotion`, `Interpolation` or
  `BroadPhase`.
- **Network:** `DFIntS2PhysicsSenderRate`, `DFIntDataSenderRate`,
  `DFIntConnectionMTUSize`, and names containing `RakNet`, `Replicator`,
  `Packet`, `Payload` or `Bandwidth`.
- **Camera:** `FIntCameraMaxZoomDistance`, `FIntCameraFarZPlane`.
- **Hiding game UI:** `DFIntCanHideGuiGroupId`.
- **CPU threads:** `FIntTaskSchedulerThreadMin`, `TaskSchedulerJob…Threads`,
  `RuntimeConcurrency`. Nightcap already sets these for your CPU.
- **Huge memory values** like `DFIntMemCacheMaxCapacityMB` set to
  `2147483647`. They eat RAM. `DFIntAssetPreloading` doesn't preload
  anything; Roblox treats it as a logging setting.

## Flags that no longer work

Common in older guides, gone in 2.742. Delete them:

`FFlagDebugForceFutureIsBrightPhase2`, `FFlagDebugForceFutureIsBrightPhase3`,
`DFFlagDebugRenderForceTechnologyVoxel`, `FFlagRenderUnifiedLighting10`,
`FIntRenderShadowIntensity`, `FFlagDisablePostFx`, `FFlagRenderNoLowFrmBloom`,
`FFlagNewLightAttenuation`, `FFlagGlobalWindRendering`,
`FFlagGlobalWindActivated`, `FFlagTaskSchedulerLimitTargetFpsTo2402`,
`FFlagHandleAltEnterFullscreenManually`,
`FFlagEnableDisplaySize9`, `FFlagAdServiceEnabled`, all
`FFlagDebugDisableTelemetry…` flags,
`DFFlagSkipHighResolutiontextureMipsOnLowMemoryDevices` (and `2`),
`FFlagSoundsUsePhysicalVelocity`, `FIntRuntimeMaxNumOfThreads`.

Nightcap already blocks crash reports and error logs, so the telemetry flags
aren't needed.

## Big flag lists

Long "FPS boost" lists get passed around. We checked a large one against
2.742, flag by flag:

- About a third don't exist anymore.
- A few more look like flags but Roblox doesn't use them, or just repeat
  Roblox's own default.
- Most of the rest are network and physics values pushed to extremes, like
  `2147483647`, from [Leave these alone](#leave-these-alone).

The useful ones from that list are in the tables above. Add flags one at a
time so you know which one broke something.

## Flags Nightcap sets

You don't need to set these.

**Your value wins:**

- **Performance preset:** sets CPU threads, memory, textures and audio for
  your PC. On when **Physics worker mode** is `throughput` (default) or
  `latency`, or **Multithreaded rendering** is on. The log says
  `keeping … over the performance preset` when your flag replaces one. Full
  list: [`src/runtime/performance_policy.cc`](../src/runtime/performance_policy.cc).
- `FIntRenderForceVideoMemorySize`: set from your RAM.
- `FFlagLuaAppChartsAppPage` = `false`: the newer Charts page can go blank.
- `FIntFlyoutTestVariantRollout12` = `2`: desktop side menu.
- `FFlagDebugLuauCodegenAll`: on in menus, off in games, for faster menus.
  Setting it yourself applies your value everywhere. See
  [PERFORMANCE.md](PERFORMANCE.md#native-code-in-the-menus).
- `FFlagDebugSkyGray` = `true` when you pick a custom sky.

**Must match, or Nightcap won't start:**

| Flag | Required value |
|---|---|
| `DFIntTaskSchedulerTargetFps` | your frame rate limit, when it's 240 or lower |
| `FFlagGameBasicSettingsFramerateCap5` | `true` |
| `FStringAppConfigurationOverrideAppPolicy` | Nightcap's own value |

**Always set, your value is ignored:**

- Network fixes: `FFlagUseRuntimeMutexRvHttpClient`,
  `DFFlagHttpClientSkipRetryForStreamingRequests`,
  `FFlagLuaAppDefaultHttpRetry`.
- Crash report uploads off. List:
  [`src/runtime/crash_report_policy.cc`](../src/runtime/crash_report_policy.cc).
- `DFFlagVoiceChatSkipPermissionCheckForTests` = `false`, so voice chat asks
  for the microphone.
- `FFlagDebugUseWebRtcAudioDevices`, `FFlagRemoteAudioDeviceSync` = `false`
  when Nightcap lists your microphones in Roblox's audio menu.

**For scripts:** `MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON` takes the same
JSON for one run and wins over `fflags.json`. With `XDG_CONFIG_HOME` set, the
file is `$XDG_CONFIG_HOME/mocktail/fflags.json`.

## Something broke

- **`[FATAL] Cannot load FFlag overrides`:** the file has a typo, like a
  missing comma or a decimal. The line says where.
- **`[FATAL] Cannot apply runtime client-settings policy`:** a
  [must match](#flags-nightcap-sets) flag has the wrong value. Remove it.
- **A game looks or runs wrong:** remove flags one at a time.
- **Start over:** delete `fflags.json` or set it to `{}`.

Logs: **Settings → About → Copy Log**, or the newest file in
`~/.local/state/mocktail/logs/sessions/` (Flatpak:
`~/.var/app/io.github.CoderDayton.nightcap/.local/state/mocktail/logs/sessions/`).
They show how many flags loaded:

```text
  [runtime] loaded 3 FFlag overrides from "/home/you/.config/mocktail/fflags.json"
```
