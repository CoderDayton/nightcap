<!-- Modified by vii from komaruworld/mocktail. See README "About this fork". -->
# FAQ

## How do I turn Discord RPC off?

RPC is on by default. It never signs in to Discord and never reads an
account token. To turn it off, launch Nightcap once to create the config,
then close it. Open `config.yaml`:

- Native / AppImage: `~/.config/mocktail/config.yaml`
- Flatpak: `~/.var/app/io.github.CoderDayton.nightcap/config/mocktail/config.yaml`

For native installs, a custom `$XDG_CONFIG_HOME` replaces `~/.config`.

Set `enabled` to `false` under `integrations.discord_rpc`. If the block is
missing, add it under the existing `integrations` section:

```yaml
integrations:
  discord_rpc:
    enabled: false
```

Save the file and restart Nightcap. Set `enabled` back to `true` to turn
RPC on again.

## Can Discord count Nightcap as Roblox play time or quests?

No. Discord only credits play time and quests to games its own scanner
detects, and Rich Presence never counts. On Linux the scanner only matches
games with a Linux entry in Discord's list. Roblox has none, so no process
name Nightcap could use would be detected. Only Roblox or Discord can change
that.

## Where can I find logs for a bug report?

Nightcap saves logs automatically. Reproduce the issue, close Nightcap, and
attach `latest.log`:

- Native / AppImage: `~/.local/state/mocktail/logs/latest.log`
- Flatpak: `~/.var/app/io.github.CoderDayton.nightcap/.local/state/mocktail/logs/latest.log`

For native installs, a custom `$XDG_STATE_HOME` replaces `~/.local/state`.

## Can I play in VR?

VR is experimental and lives on upstream's `vr` branch, which does not
include the Nightcap changes. Install the
[build dependencies](README.md#building), then switch to that branch and
build:

```bash
git remote add upstream https://github.com/komaruworld/mocktail.git
git fetch upstream
git switch -c vr upstream/vr
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOCKTAIL_ENABLE_VR=ON
cmake --build build -j4
```

Start WiVRn or SteamVR/ALVR, connect your headset, then run:

```bash
./build/mocktail -vr
```
