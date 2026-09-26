<!-- Modified by vii from komaruworld/mocktail. See "About this fork" below. -->
<div align="center">
  <img src="https://raw.githubusercontent.com/CoderDayton/nightcap/main/packaging/io.github.CoderDayton.nightcap.svg" alt="Nightcap logo" width="128">
  <h1 style="margin-top: 5px;">
    Nightcap
    <br>
    <a href="https://github.com/CoderDayton/nightcap/actions/workflows/ci.yml"><img src="https://github.com/CoderDayton/nightcap/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
    <a href="https://github.com/CoderDayton/nightcap/stargazers"><img src="https://img.shields.io/github/stars/CoderDayton/nightcap?style=flat&logo=github" alt="Stars"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="License"></a>
  </h1>
</div>

Nightcap is Mocktail, tuned for playing on a real PC. It runs the Android
`x86_64` Roblox client on Linux, including a Linux userspace hosted by
FreeBSD's Linuxulator. It provides the Android ABI and JNI pieces the client
expects, then connects them to SDL3 and Vulkan or OpenGL on the Linux side.

Nightcap is an independent community project. It is not affiliated with Roblox
Corporation or VinegarHQ and does not distribute the Roblox client.

## About this fork

Nightcap is a fork of [komaruworld/mocktail](https://github.com/komaruworld/mocktail)
with extra fixes for playing on a PC. The program and config paths keep the
`mocktail` name so upstream fixes still apply cleanly. The app ID is
`io.github.CoderDayton.nightcap`, so Nightcap and upstream can be installed
side by side.

- **Lower input lag.** Frames are shown as soon as they are ready instead of
  waiting in a queue.
- **Sharper textures.** Small textures are upscaled 4x by default, and you
  can replace any texture with your own PNG in `~/.config/mocktail/textures`.
  A ready-made crosshair is in [`textures/`](textures/README.md). Set
  `MOCKTAIL_SMALL_TEXTURE_UPSCALE=1` to turn the upscale off.
- **ETC2 textures on any GPU.** Games that use compressed mobile textures now
  render correctly on desktop GPUs, without stutter.
- **No more error 319 kicks.** Servers no longer drop you shortly after
  joining.
- **Correct device info.** Roblox sees your real RAM and screen size.
- **Better desktop launcher.** Runs on Wayland and always finds the installed
  binary.

Everything else works the same as upstream. See the
[roadmap](docs/ROADMAP.md) for what is coming next, the
[changelog](CHANGELOG.md) for what each release changed,
[performance](docs/PERFORMANCE.md) for CPU load, heat and the knobs that change
them, and the [FAQ](FAQ.md) for Discord RPC, logs, and VR.

## Install with Flatpak

```bash
flatpak install --user https://coderdayton.github.io/nightcap/nightcap.flatpakref
flatpak run io.github.CoderDayton.nightcap
```

Updates arrive through `flatpak update`. Nightcap is not on Flathub yet.

## AppImage

Grab the AppImage from the
[latest release](https://github.com/CoderDayton/nightcap/releases/latest),
make it executable, and run it:

```bash
chmod +x Nightcap-x86_64.AppImage
./Nightcap-x86_64.AppImage
```

## Distribution packages

Each release also carries a DEB, an RPM, and a pacman package, with a
`.sha256` beside every file on the
[releases page](https://github.com/CoderDayton/nightcap/releases/latest):

```bash
sudo apt install ./nightcap_<version>_amd64.deb
sudo dnf install ./nightcap-<version>-1.x86_64.rpm
sudo pacman -U nightcap-<version>-1-x86_64.pkg.tar.zst
```

They install `/usr/bin/mocktail`, so they conflict with upstream's `mocktail`
package. Nightcap is not in any distribution repository or the AUR; the
recipes under `packaging/aur` are kept for local builds only.

Or build and install from source using the steps under
[Building](#building).

## How it works

```
Roblox APK -> signature and ABI checks -> Bionic + JNI -> SDL3 + Vulkan/OpenGL
```

The APK is checked before any native code is loaded. It is downloaded on first
launch and is not bundled with Nightcap. The last working copy is kept in case
an update fails.

<details>
<summary>Screenshots</summary>

![Roblox home in Nightcap](assets/screenshots/flatpak-home.png)

![Roblox gameplay in Nightcap](assets/screenshots/flatpak-gameplay-tower.png)

![Roblox experience in Nightcap](assets/screenshots/flatpak-gameplay-lobby.png)

</details>

## FFlag overrides

Put a JSON object in `$XDG_CONFIG_HOME/mocktail/fflags.json` (usually
`~/.config/mocktail/fflags.json`). Nightcap applies it on the next launch.

PC profiles use the legacy Charts page by default to avoid the blank screen
caused by `Color3` errors in the newer SDUI page. An explicit
`FFlagLuaAppChartsAppPage` override takes precedence over this default.

```json
{
  "FFlagExample": "True",
  "DFIntExample": "120"
}
```
## Settings

Edit `~/.config/mocktail/config.yaml`. Useful options:

- `graphics.frame_rate_limit: display` to unlock FPS to your monitor's rate.
- `graphics.vsync: off` for the lowest lag, at the cost of tearing.
- `graphics.backend: opengl` if Vulkan does not work on your GPU.

## Building

Linux `x86_64` is supported. Experimental FreeBSD 15.1 Linuxulator support has
been tested with an `x86_64` Fedora 44 userspace. On FreeBSD, Nightcap runs
inside Linuxulator; it is not a native FreeBSD binary. Building requires CMake
3.20+, Git, pkg-config, LLD, binutils, a C++17 compiler, SDL 3.4+, SDL3_ttf,
Vulkan, EGL, libplacebo, fontconfig, libcurl, OpenSSL, libelf, libyaml, libpng,
minizip, Capstone 5, utf8proc, nlohmann/json, GTK4, libadwaita 1.6+, and
WebKitGTK 6.0.

See the [FreeBSD Guide](packaging/freebsd/README.md) for setup and launch instructions.

<details>
<summary>Ubuntu 26.04+</summary>

```bash
sudo apt update
sudo apt install build-essential cmake git ninja-build pkg-config lld \
  libsdl3-dev libsdl3-ttf-dev libcurl4-openssl-dev libssl-dev \
  nlohmann-json3-dev libyaml-dev libpng-dev libelf-dev libminizip-dev \
  libcapstone-dev libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev \
  libutf8proc-dev libfontconfig1-dev libegl-dev libvulkan-dev \
  libplacebo-dev zlib1g-dev
```
</details>

<details>
<summary>Arch Linux</summary>

```bash
sudo pacman -S --needed base-devel cmake git ninja pkgconf lld sdl3 sdl3_ttf \
  curl openssl nlohmann-json libyaml libpng libelf minizip capstone gtk4 \
  libadwaita webkitgtk-6.0 libutf8proc fontconfig libglvnd \
  libplacebo vulkan-headers vulkan-icd-loader zlib
```
</details>

<details>
<summary>Fedora 44+</summary>

```bash
sudo dnf install gcc-c++ cmake git ninja-build pkgconf-pkg-config lld \
  SDL3-devel SDL3_ttf-devel libcurl-devel openssl-devel \
  nlohmann-json-devel libyaml-devel libpng-devel elfutils-libelf-devel \
  minizip-ng-compat-devel \
  capstone-devel gtk4-devel libadwaita-devel webkitgtk6.0-devel \
  utf8proc-devel fontconfig-devel libglvnd-devel vulkan-headers \
  vulkan-loader-devel libplacebo-devel zlib-ng-compat-devel
```
</details>

```bash
git clone --recurse-submodules https://github.com/CoderDayton/nightcap.git
cd nightcap
make build
./build/mocktail
```

To install it for your user, with an app menu entry and Roblox link handling:

```bash
make install PREFIX=~/.local
```

Use `sudo make install` instead to install system-wide under `/usr`. To make
Roblox website links open the build tree copy without installing, run
`make register-url-handler`.

To run the same checks as CI before each commit and push, install
[lefthook](https://github.com/evilmartians/lefthook) and run once:

```bash
lefthook install
```

## Known limitations

Nightcap does not currently work with `hardened_malloc`. Using
`hardened_malloc` may cause Nightcap to fail to start or crash during runtime.
Run Nightcap without `hardened_malloc` enabled.

## License

[Apache License 2.0](LICENSE). Third-party components keep their own licenses.

## Support

Support this fork by giving it a star or with cryptocurrency:

- LTC: `LdABR2ELRYrUESWZWrEk1uNx38gHHMuHUU`
- SOL: `tjeDnPoWeyW8zfTf8c5CVAm5QTV8vFPaPouaScAdwfT`

Support the upstream project, which does most of the heavy lifting:

- USDT (TON): `UQCi6Yzcc9cOctoij6n_r1K90-OdVxAT0D_xo2UzGKkQaJDY`
- USDT (TRC20): `TNPMG9Vig2xiuo2r1QqnXRChPH7Vu28Jmx`
