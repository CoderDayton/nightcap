# FAQ

## How do I enable Discord RPC?

RPC is disabled by default. Launch Mocktail once to create the config, then
close it. Open `config.yaml`:

- Native / AppImage: `~/.config/mocktail/config.yaml`
- Flatpak: `~/.var/app/space.bigrat.mocktail/config/mocktail/config.yaml`

For native installs, a custom `$XDG_CONFIG_HOME` replaces `~/.config`.

Set `enabled` to `true` under `integrations.discord_rpc`. If the block is
missing, add it under the existing `integrations` section:

```yaml
integrations:
  discord_rpc:
    enabled: true
```

Save the file, open Discord Desktop, and restart Mocktail. Set `enabled`
back to `false` to disable RPC.

## Where can I find logs for a bug report?

Mocktail saves logs automatically. Reproduce the issue, close Mocktail, and
attach `latest.log`:

- Native / AppImage: `~/.local/state/mocktail/logs/latest.log`
- Flatpak: `~/.var/app/space.bigrat.mocktail/.local/state/mocktail/logs/latest.log`

For native installs, a custom `$XDG_STATE_HOME` replaces `~/.local/state`.

## Can I play in VR?

VR is experimental. Install the [build dependencies](README.md#building),
then switch to the `vr` branch and build:

```bash
git fetch origin
git switch vr
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOCKTAIL_ENABLE_VR=ON
cmake --build build -j4
```

Start WiVRn or SteamVR/ALVR, connect your headset, then run:

```bash
./build/mocktail -vr
```
