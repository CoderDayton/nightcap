# Changelog

All notable changes to Nightcap. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Release notes on GitHub are taken from the matching section here.

## [Unreleased]

### Added

- An optional popup shows the game server's location for a few seconds after each join
  (`overlay.location`), and `overlay.location_offset` moves it down. A separate performance
  overlay keeps your frame rate (`overlay.fps`) and ping (`overlay.ping`) on screen, and
  `overlay.position` moves it to a corner or edge. All are in the settings window under Graphics,
  and changes apply while you play, without a restart.
- Advanced settings can clear Roblox's downloaded game assets, and show how much space they use.
  Settings and sign-in are kept. With Fleasion, `integrations.fleasion.clear_cache_on_launch`
  clears them at every launch so new replacements load. A join from the website closes an open
  Nightcap first, so the clear still runs.
- Roblox's own account switcher is on for every account 13 or older. Roblox's servers turn it on
  for some accounts only, and Nightcap's app policy had it off for all. After a switch, Nightcap
  saves the new sign-in and restarts as that account, as the Android app does. The restart waits
  up to 5 seconds for Roblox to save its signed-in account list first. Nightcap also keeps the
  other roblox.com cookies Roblox sets, in `auth/roblox.cookies.json`, and hands them back at
  start-up like the Android app, so the account switcher gets back the cookies it set. Signing out from
  Roblox also restarts Nightcap at the sign-in screen, so adding an account that Roblox can only
  add after signing everyone out finishes instead of loading forever.
- A MangoHud setting under Performance (`performance.mangohud`) launches Nightcap with `MANGOHUD=1`.
- Buying Robux opens roblox.com/upgrades/robux in your browser, since Nightcap has no Google
  Play. This works from a package on the Robux page and from a game's purchase prompt. The game is
  told nothing was bought, so its prompt closes instead of waiting. A desktop notification says
  where the purchase went. Sign in to the same account in the browser, buy there, then try the
  purchase in the game again.

### Fixed

- Roblox web pages such as a game's server list no longer close right after opening on some NVIDIA
  setups on Wayland. GTK's OpenGL setup crashed those windows, so Nightcap now turns it off for them.
- Roblox's in-game Switch Server button joins a different server instead of returning you to the
  home screen. Roblox sends that launch as the game closes, and Nightcap now picks it up.
- Fleasion replacements load again. With `integrations.fleasion.enabled`, Nightcap sets
  `FFlagAssetProviderUseAssetDeliveryV3` to `False`, so Roblox requests assets through the endpoint
  Fleasion rewrites. A value in `~/.config/mocktail/fflags.json` still wins.
- A frame-rate limit of `unlimited` or above 240 now goes past 240 FPS. Nightcap writes it to
  Roblox's own frame-rate cap, since Roblox stops `DFIntTaskSchedulerTargetFps` at 240.
- A new install now gets Roblox's current menus and in-game UI on its first launch instead of its third.
  Roblox downloads them signed out but does not record them, so Nightcap records them and restarts once,
  a few seconds in.
- Each install now sends Roblox its own random Android device ID instead of `0000000000000000`.
- The Flatpak no longer fails to install Roblox on NVIDIA under Wayland with "graphics canary exited with
  status 139". NVIDIA's Vulkan driver crashed when Roblox closed its graphics, so every Roblox version
  failed its startup test.
- Holding right click over a button, the leaderboard, or the Roblox menu no longer freezes the cursor.
  The cursor now only holds still while the camera turns, as on Windows. Once the camera starts turning,
  it keeps turning until you let go, even when a button in the game world passes under the cursor.
- Shift lock no longer lets the cursor wander onto buttons when a game turns its lock off and on quickly.
  Nightcap keeps the pointer captured until the lock has stayed off for a few frames. Leaving shift lock
  or first person leaves the cursor at the center of the window, as on Windows.
- Going into first person while holding right click now centers the crosshair in games that draw their own,
  such as BedWars. Roblox kept the spot where right click was pressed as the mouse position.
- The app menu entry opens Nightcap again on NixOS. The Nix package wrote the install path twice into
  the `.desktop` file, so the menu entry pointed at a file that does not exist.
- When the Roblox updater crashes or quits without saying why, the session log and error dialog now
  give its exit status or signal instead of only "Roblox update preflight failed". When DNS cannot find
  a download server, the error points to a new FAQ entry with the commands to switch DNS servers.
- PCs with 4 GB of RAM now get a larger texture budget instead of Roblox's 64 MB mobile default.
  The budget now applies to any PC with at least 2 GB, at an eighth of its memory.

### Changed

- ETC2 textures now decode on the graphics card instead of the CPU, so loading them no longer takes CPU
  time from the game. Texture overrides still decode on the CPU. A self-test at startup falls back to
  the CPU if the card's output differs, and `MOCKTAIL_ETC2_GPU_DECODE=0` turns it off. Cards with BC
  texture support keep these textures compressed as BC7, BC4 or BC5, in a quarter of the video memory,
  and small upscaled textures too. `MOCKTAIL_ETC2_BC_TRANSCODE=0` keeps them decoded.
- Small textures are upscaled on the graphics card with a smoothing filter that keeps hard two-colour
  edges sharp, instead of nearest-neighbour on the CPU.

## [0.4.0] - 2026-10-18

### Added

- Added a settings window. Open it from the app menu (right-click Nightcap, then Settings) or with
  `mocktail --settings`. It changes `config.yaml` and `fflags.json` for you, keeps your comments, and does
  not save a value Nightcap could not start with. Changes apply the next time Nightcap starts. If Nightcap
  is open when you change something, the window offers to restart it for you, with the same options and
  environment it was started with. It opens on the main menu. Every `config.yaml` setting has a row,
  including the device, the window size, the proxy, Fleasion and the update source. A setting you changed
  from its default gets a button that resets it.
- An asset overlay folder, `~/.local/share/mocktail/asset_overlay`. A file there replaces the file Roblox
  ships at the same path, such as a cursor, a font or a sky texture, and it stays in place when Roblox
  updates. The layout matches Sober's `asset_overlay` folder. `MOCKTAIL_ASSET_OVERLAY_DIR` moves it. Thanks to
  @dim-ghub.
- The asset overlay folder now also replaces files Roblox downloads and no longer ships, such as the shift
  lock and text cursors. Put the file under `content/`, at the path Roblox uses. Thanks to @dim-ghub for the
  idea.
- `input.system_cursor` in `config.yaml`, or System cursor theme in the settings window, shows your
  desktop theme's arrow, hand and text cursors instead of Roblox's.
- `input.hardware_cursor`, or Hardware cursor in the settings window, has the desktop draw the cursor
  instead of Roblox drawing it into the game, so cursor tools such as Hypr Dynamic Cursors work with it.
  It keeps Roblox's pictures, a game's own cursor included. With both on, the theme covers the stock
  cursors and a game's own cursor is still drawn by the desktop. The shift lock cursor stays Roblox's. If a
  Roblox update changes the code these rely on, Nightcap logs that and Roblox keeps drawing its cursors.
  `MOCKTAIL_CURSOR_TRACE=1` prints each cursor Roblox draws. On Wayland, a desktop-drawn cursor also stays
  at the press point while you hold the right mouse button to turn the camera, and sits at the center of the
  window in first person and shift lock, where Roblox draws its cursor.
- `MOCKTAIL_ASSET_TRACE=1` now also prints the files Roblox asks for and does not get, as `[asset] miss`.
- Arch Linux: `nightcap-git` is on the AUR and builds the current `main` branch. Thanks to @dim-ghub, who
  maintains it.
- The nightly `continuous` prerelease now has a `.deb`, `.rpm` and pacman package next to the AppImage,
  each with a `.sha256`. They are versioned after the last release, such as `0.3.4+r29.g112f5ab`, so they
  upgrade over it and over each other. The notes list the commits since the last nightly.
- The log now names each Roblox Java call that Nightcap has no handler for, once per call, as
  `[JNI] no handler for <class>.<method><signature>`. This shows what a new Roblox version needs.
- `--profile` traces now show how much GPU memory decoded ETC2 textures take, as the `emulated texture (MB)`
  and `compressed texture (MB)` counters. `scripts/summarize_profile_trace.py` prints the peak of each.
- A fixed proxy can be SOCKS5 and can have a login: `network.proxy_scheme`, `network.proxy_username` and
  `network.proxy_password` in `config.yaml`, or the Proxy rows in the settings window, where the password
  is hidden as you type. It is saved as plain text in `config.yaml`. From upstream mocktail.

### Fixed

- Joining a game from a website link while already in a game no longer freezes Nightcap. It leaves the
  current game and joins the new one, and leaving that game returns to the home screen.
- Game links opened from a browser are no longer rejected when they ask for any public server, or when
  they carry an unencoded `referralPage` address.
- The loading window no longer gets a dock icon of its own. It now shows under Nightcap's icon.
- The browser tracker ID no longer fails with HTTP 500 on almost every launch. Nightcap reuses the ID
  Roblox already saved, asks for one only when there is none, and keeps its cookie out of your sign-in file.
- After you alt-tab away, moving the mouse over the game no longer turns the camera. Click in the game or
  press a key to take control again.
- The `.deb` package now finds its sign-in, update and error helpers, which Debian installs under
  `/usr/lib/x86_64-linux-gnu`. From upstream mocktail.
- Nightcap no longer crashes at first launch when it cannot read the sign-in helper's path. Thanks to
  @justacalico.
- In the Flatpak, the sign-in window now registers on the desktop's accessibility bus. Thanks to @reqhald.

### Changed

- The home screen and avatar editor have fewer long frames. PC profiles compile the app's menu scripts to
  native code and turn this off while you are in a game, so games run as before. Set
  `FFlagDebugLuauCodegenAll` in `fflags.json` to choose it yourself.
- Nightcap answers Roblox's Java calls with less work per call, and the audio thread no longer allocates
  memory for each block of sound it plays.
- Less hitching while textures load in. Freeing GPU memory no longer waits for texture decodes that do not
  read it. In Blade Ball the worst `vkFreeMemory` fell from 31ms to 3ms.
- Nightcap starts about 0.2s faster. It no longer re-hashes an unchanged Roblox library twice on every
  launch, and the browser tracker no longer waits on a web request.

## [0.3.4] - 2026-10-11

### Added

- The AppImage build now makes a `.zsync` file, so tools like `appimageupdatetool` can update the AppImage.
- The desktop side panel menu now shows by default on PC profiles. Set `FIntFlyoutTestVariantRollout12` in
  `fflags.json` to change it.

### Changed

- The Flatpak build asks for the X11 socket only when there is no Wayland session, as before 0.3.3.
- Nightcap now uses Wayland on a Wayland session with NVIDIA drivers too, however it is started. It used to
  fall back to X11 there when started from a terminal.
- Building Nightcap no longer needs libexecinfo.

### Fixed

- Fixed Roblox 2.742 being rejected as an update, which left the game on the older version. Nightcap now
  accepts Roblox versions that change the order of their start-up steps.
- Fixed Nightcap reporting Mocktail's version number, 1.0.4, in session logs and support bundles. It now
  reports its own version.
- Fixed the old Roblox home screen showing instead of the new one with the Me tab and the "For you" and
  "Charts" tabs. Nightcap now runs the Roblox start-up step that loads the new interface for your account.
- Fixed anti-aliasing being forced off at every graphics quality level. Roblox now picks it, and a
  `FIntDebugForceMSAASamples` value in `fflags.json` still applies. Thanks to @qutad.
- Fixed the voice chat microphone only working on Roblox 2.738. It no longer depends on one Roblox version.
- Fixed other players not hearing you in voice chat after a Roblox update. The updater now rebuilds the
  audio device data when the previous version's profile has none.
- Fixed frame hitches while textures load on graphics cards that need ETC2 textures decoded on the CPU.
- Fixed problems on Chimera Linux and other musl systems, including error messages that differed from the
  ones Roblox expects.
- Fixed the nightly AppImage build failing when a bundled GTK 4 was older than the one libadwaita needs.
- Fixed a random crash on Wayland with NVIDIA drivers, where the game window closed after you let go of the
  camera or left mouse lock. The log showed `wp_linux_drm_syncobj_surface_v1` error 3.
- Fixed some menu actions being ignored. The Chat button on a profile now opens the Party tab with that
  player, and clicking a search suggestion now runs the search instead of closing the search menu.
- Fixed possible crashes when the game calls an Android function Nightcap did not provide, or releases the
  same reference twice.
- Fixed a possible hang on close when a web view process stayed open. Nightcap now stops waiting for the log
  writer after 2 seconds.
- Fixed a possible missing picture on graphics drivers that reject the low-latency present mode. Nightcap now
  retries with the game's own mode.
- Fixed a possible Wayland crash when the window first appears on the OpenGL graphics path.
- Fixed a missing picture on machines where the graphics card runs a different driver than the one Nightcap
  picked, such as an NVIDIA card on nouveau or a laptop with an Intel chip and an older second card. Nightcap
  now picks the Vulkan driver that matches the running kernel driver, and says so when no card can run Vulkan.

## [0.3.3] - 2026-10-04

### Added

- Joining a friend from a Roblox "follow user" link now works. Nightcap looks up which game they are in and
  joins it.
- Nix users get a desktop entry for the app.

### Changed

- Roblox textures now decode about 3 times faster, which shortens the stalls when a place loads and when new
  parts of the map stream in. Thanks to @glook9001.

### Fixed

- `make update-roblox` no longer deletes your previous Roblox install when an import fails partway through. If
  a step fails, the old files are put back, or kept in a folder whose path is printed. Thanks to @kimlherme.
- A failed Roblox update now restores your previous version's records instead of leaving them half switched. A
  damaged version file is rejected instead of being half read, and a temporary failure to start the startup
  check no longer marks a working Roblox build as broken for good. Thanks to @kimlherme.
- Fixed a possible crash when Roblox clears its audio queue while sound is still playing, and a memory leak in
  lists of objects passed to Roblox's Java code. Thanks to @kimlherme.
- Fixed a possible crash when the text overlay shuts down, a freeze when Roblox releases the mouse lock while
  another release is already in progress, and network lookups that misread Roblox's options. Thanks to
  @kimlherme.
- Fixed the sky sometimes rendering with the wrong textures in the Vulkan renderer.
- Clicking while a Roblox text box has focus no longer closes it. Only Roblox decides when a click ends
  typing.
- Going to another page in the web view, such as Friends from a profile, no longer closes the web view. It
  crashed on every page change on Wayland systems whose GL setup GTK could not use.
- Fixed a crash when Roblox's saved dark or light theme setting was missing or damaged.

## [0.3.2] - 2026-09-29

### Added

- Performance recordings made with `--profile` now show how busy the CPU was during each frame, which makes it
  easier to tell what caused a stutter.
- Experimental support for ARM64 (`aarch64`) Linux, running Roblox's Android `arm64-v8a` client.

### Fixed

- When Roblox asks for a Google Play device check during login, Nightcap now answers right away that it isn't
  supported. Before, the "Verifying you're not a bot" screen waited about a minute for an answer and then sent
  you back to the login screen.
- Solving a login challenge now closes its window. Before, Roblox's "challenge completed" signal was ignored
  and the window stayed open. Thanks to @glook9001.
- Login no longer stalls after you pass a verification check. Before, Roblox only saw the page's "challenge
  completed" signal after a one-minute timeout, so it gave up and the login failed. On a Roblox build Nightcap
  can't handle this way, the log says "WebViewProtocol getter not recognized" and web pages behave as before.
- Login check windows and other web pages get 5 seconds to start instead of 3, so a slow start no longer
  closes them. The log says how long each one took to open.
- Less stutter while joining a game and while textures load in. In Blade Ball, loading hitches dropped by
  about half, and the worst one went from 0.8 seconds to 0.55 seconds.
- Clicking Play on a private server now joins right away. Before, nothing happened until you clicked a sidebar
  tab like Home.
- Middle click now reaches Roblox as the middle mouse button.
- Scrolling follows your system's natural scrolling setting.
- Fixed a crash on some x86_64 hosts when Roblox asked about a thread other than the one making the call.
- Roblox updates no longer mix up the ARM64 and x86_64 compatibility profiles for the same version.
- A partial or out-of-range user id in `MOCKTAIL_ROBLOX_USER_ID` or `MOCKTAIL_LOCAL_STORAGE_CURRENT_USER_ID`
  is now ignored instead of being partly read.

### Changed

- `make flatpak` and `scripts/build_flatpak.sh` now build with 2 jobs by default instead of 4, so a local
  Flatpak build is less likely to run out of memory. Set `FLATPAK_JOBS` or `MOCKTAIL_FLATPAK_JOBS` to use
  more. Thanks to @glook9001.

## [0.3.1] - 2026-09-20

### Added

- Each release carries a DEB, an RPM, and a pacman package beside the AppImage, with a `.sha256` for every
  file. They install `/usr/bin/mocktail` and conflict with each other and with upstream's `mocktail` package.
  Nightcap is in no distribution repository or the AUR; the recipes under `packaging/aur/nightcap{,-bin,-git}`
  build from this repository locally.
- The AppStream metadata lists the Nightcap releases, so software centres show the release history instead of
  upstream's.

### Fixed

- ETC2 texture decoding no longer blocks the thread that submits the frame. Uploads decode on a background
  dispatcher and the submit carries a timeline semaphore wait, so the GPU waits for the texture instead of the
  render thread. Reading the application's compressed bytes moves to that dispatcher too whenever they are
  already mapped. In Blade Ball the worst `vkQueueSubmit` fell from 140ms to 1ms, its p99 from 4.9ms to
  0.05ms, and the total time spent in submits from 2534ms to 309ms. Frame interval is unchanged: the remaining
  tail is the engine's own main-thread step. A device without the `timelineSemaphore` feature decodes inline
  as before, as does a submit whose `pNext` already sizes arrays by its semaphore counts.
- The mouse pointer jumped when a right-click camera drag ended, and drifted to the viewport edge during one.
  The input router inferred pointer capture from SDL reporting zero absolute coordinates, which holds on X11
  but not on Wayland, where the reported position accumulates the relative deltas and leaves the window.
  Motion, button, and wheel events now carry the window's relative mouse mode. The right-click camera fallback
  also only captures inside an experience, so the cursor stays free over the app surface.
- Shutting the window down while an experience was starting or leaving could free the pointer capture owner
  underneath the thread using it. Callers off the SDL thread now hold a reference for the length of their
  call.
- The TextBox caret did not move when a trailing space was typed in a single-line field. The final cluster
  rect SDL_ttf reports stops at the last inked glyph, so the laid-out width is used there instead. Wrapped
  layouts keep the clamp, because their width is the widest line rather than the last.
- Every TextBox keystroke re-ran `TTF_Init`, a fontconfig sort, six `TTF_OpenFont` calls, and `TTF_Quit` on
  the render thread. The overlay holds its opened fonts and reopens them only when the Roblox font or the
  point size changes, keeping the current set when a reopen fails. Measured over 100 rasters, 1.59ms to 0.12ms
  per keystroke.
- Staged ETC2 copies now carry an explicit host-write barrier. A queue submission only makes host writes from
  before it visible to the device, and an asynchronous decode writes after the submit, so without the barrier
  those texels were not guaranteed to reach the copy that reads them.

### Changed

- `docs/PERFORMANCE.md` documents the GameMode settings to run with. A `desiredgov=powersave` in
  `~/.config/gamemode.ini` overrides the packaged `performance` default and clocks the CPU down for the whole
  session while still reporting an active performance request.

## [0.3.0] - 2026-09-18

### Added

- Custom Discord presence. `integrations.discord_rpc.text.title` replaces the name in "Playing Nightcap", and
  `integrations.discord_rpc.images` sets the large and small icons and their hover text. Text and image fields
  accept `{place_name}`, `{place_icon}`, and `{creator_name}`. A field that expands an empty placeholder is
  left out, and `text.state: ""` hides the state line.
- The default Discord card now matches Sober: the title is "Roblox", the details line is "Playing
  <experience>", the state is "by <creator>", and the Roblox badge sits inside the experience icon. The
  creator comes from the same Roblox games request that already fetches the experience name. With
  `show_place_name: false` the creator is hidden too, so the card shows only the title and timer.
- `--profile <file>` writes a Chrome-format trace of the Vulkan adapter's work: presents, GPU waits, submits,
  ETC2 texture decode, and pipeline and shader creation. Open it in ui.perfetto.dev. `docs/BENCHMARKING.md`
  describes the benchmark runs, and `scripts/summarize_profile_trace.py` turns traces into a before and after
  table.

### Fixed

- Texture uploads stalled the frame they were submitted on. ETC2 batches decoded on worker threads but were
  then resampled one upload at a time on the submitting thread, which at the default 4x upscale is sixteen
  times the decoded texels. Resampling now runs on the same worker pool, in two passes so mip level 0 still
  resolves before the mips built from it. In a 70k-part place the worst `vkQueueSubmit` fell from 167ms to
  92ms.
- Most ETC2 batches decoded on a single thread. Workers were created and joined per batch, so the parallel
  floor had to cover that cost and sat at a 1024x1024 image; batches under it ran on the caller, which was 54%
  of all decode time in a 70k-part place. Decode workers are now a persistent pool, and the floor drops to a
  128x128 image. Mean decode time in that place fell from 8.2ms to 4.7ms, and 11.8ms before both texture
  fixes.
- On CPUs with a wide core-speed spread, Feral GameMode pinned the process to the few fastest cores,
  concentrating Roblox's worker pool onto them instead of spreading it. With `performance.gamemode: auto`, a
  request that narrows the CPU affinity is now released again, which releases the pin. Set `pin_cores=no` in
  `~/.config/gamemode.ini` to keep GameMode without the pinning, or `performance.gamemode: on` to accept it.
  See `docs/PERFORMANCE.md`.
- With `show_place_name: false`, Discord no longer shows the experience thumbnail or its name on hover.
- The host main thread no longer spins on the engine's message pump, which cost about half a core and 146,000
  empty calls a second. It now rests 100 µs between polls: sleeping when the governor is `performance`,
  parking the core with TPAUSE on CPUs that have it, spinning otherwise. With GameMode setting the governor,
  process CPU in a 60k-part place fell from 216% to 107% at the same 120 fps. `MOCKTAIL_ENGINE_PUMP_REST`
  forces a mode; see `docs/PERFORMANCE.md`.
- ETC2 uploads with a `bufferRowLength` or `bufferImageHeight` larger than the image read the padded rows as
  packed data and decoded garbage.
- ETC2 resampling created and joined worker threads on every batch. It now runs on the persistent decode pool.
- A failed trace write left the profile writer thread waking every 250 ms for the rest of the session. It now
  exits.
- The `--profile` trace gains a `pump` slice for the engine's main-thread step.

## [0.2.0] - 2026-09-16

### Added

- FAQ covering Discord RPC, log locations, and VR, adapted from upstream.
- `input.raw_mouse` in `config.yaml` sends mouse look deltas in the host's own units. Turn it on when
  first-person aiming feels too slow or too fast under fractional display scaling. Off by default.
- Nightly AppImage builds, published as the `continuous` prerelease. They are untested; use a tagged release
  for normal play.

### Changed

- Values in `fflags.json` now win over the performance preset instead of stopping the launch. Startup prints
  each key it kept.
- Discord Rich Presence uses Nightcap's own Discord application, so it shows as "Playing Nightcap". Terms and
  privacy pages for it live on the project site.
- Discord Rich Presence is on by default. Set `integrations.discord_rpc.enabled: false` to turn it off.
- The AppImage build verifies its reduced dependency packages against digests taken at build time, rather than
  a digest list kept in the repository.

### Fixed

- Mouse look keeps responding after a long turn. The tracked pointer position could drift past the edge of the
  view, which swallowed every reversal until the mouse travelled all the way back.
- Nightcap no longer refuses to start with `cannot lock appStorage`. It created its own credential directory
  using the process umask, then rejected that directory for being group writable. Systems with a permissive
  umask, such as `002`, failed on first run.

## [0.1.0] - 2026-09-15

First Nightcap release, forked from komaruworld/mocktail.

### Added

- ETC2 and EAC texture emulation on Vulkan, so games that use compressed mobile textures render correctly on
  desktop GPUs. Decoding runs on worker threads to avoid stutter.
- PNG texture overrides in `~/.config/mocktail/textures`, keyed by upload hash, with a dump mode to find the
  texture you want to replace.
- Small textures are upscaled 4x by default. Set `MOCKTAIL_SMALL_TEXTURE_UPSCALE=1` to turn it off.
- AppImage and signed Flatpak builds on every release tag. The Flatpak repo is published at
  https://coderdayton.github.io/nightcap/.
- Roadmap in `docs/ROADMAP.md`.

### Changed

- Frames are presented as soon as they are ready instead of waiting in a queue, which lowers input latency.
- Roblox now sees the real host RAM and display size instead of a low-end phone profile.
- App ID is `io.github.CoderDayton.nightcap`, so Nightcap installs next to upstream Mocktail. The binary and
  config paths keep the `mocktail` name.
- New name, icon, and colors.
- The desktop entry launches on Wayland and points at the installed binary.

### Fixed

- Servers no longer kick with error 319 shortly after joining.
- The FMOD output-device bridge is optional, so the updater no longer fails without it.
- The AppImage build no longer fails on stale checksums for the slimmed Arch packages.

[Unreleased]: https://github.com/CoderDayton/nightcap/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/CoderDayton/nightcap/releases/tag/v0.4.0
[0.3.4]: https://github.com/CoderDayton/nightcap/releases/tag/v0.3.4
[0.3.3]: https://github.com/CoderDayton/nightcap/releases/tag/v0.3.3
[0.3.2]: https://github.com/CoderDayton/nightcap/releases/tag/v0.3.2
[0.3.1]: https://github.com/CoderDayton/nightcap/releases/tag/v0.3.1
[0.3.0]: https://github.com/CoderDayton/nightcap/releases/tag/v0.3.0
[0.2.0]: https://github.com/CoderDayton/nightcap/releases/tag/v0.2.0
[0.1.0]: https://github.com/CoderDayton/nightcap/releases/tag/v0.1.0
