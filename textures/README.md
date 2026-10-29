# Texture overrides

Optional PNGs you can drop into your own override directory. Nothing here is
installed or loaded automatically.

## Using one

Copy the file into the override directory, keeping its name:

```sh
mkdir -p ~/.config/mocktail/textures
cp textures/75dcd1166a6cbaa9_32x32.png ~/.config/mocktail/textures/
```

Restart the game. The log line `[vulkan] texture override loaded: <path>`
confirms it was picked up.

To remove it, delete the file again.

`MOCKTAIL_TEXTURE_OVERRIDE_DIR` points the lookup somewhere else.

## What is here

| File | Replaces |
| --- | --- |
| `75dcd1166a6cbaa9_32x32.png` | The in-game crosshair, as a thin dot-and-ticks reticle (128x128). |

## Naming

The file name is `<hash>_<width>x<height>.png`. The hash is the 16 hex digit
FNV-1a hash of the texture's level-0 compressed ETC2 bytes, which is what the
override lookup keys on. The size is the original texture's, not the
replacement's: only textures of that size skip GPU decoding and BC
compression so they can be checked against overrides. A file named just
`<hash>.png` is skipped, with a `texture override skipped` log line. A texture
Roblox re-encodes gets a new hash, so an override can stop matching after a
client update.

To find the hash of a texture you want to replace, set
`MOCKTAIL_TEXTURE_DUMP_DIR` to an empty directory and play until the texture
appears. Every uploaded texture is written there once as
`<hash>_<width>x<height>.png`. Edit the one you want, keep its name, and put
it in the override directory.

Overrides are resampled to the size the game asked for, so the replacement does
not have to match the original's dimensions.

## Files that ship with Roblox

A texture override only reaches ETC2 textures. To replace a file Roblox ships
with, such as a cursor, a font or a sky texture, use the asset overlay folder
instead. It needs no hash: the file goes at the same path Roblox uses.

```sh
dir=~/.local/share/mocktail/asset_overlay/content/textures/Cursors/KeyboardMouse
mkdir -p "$dir"
cp my-cursor.png "$dir/ArrowCursor.png"
```

Restart the game. The layout matches Sober's `asset_overlay` folder, so a file
set made for Sober can be copied in as it is. The Flatpak build keeps the
folder at `~/.var/app/io.github.CoderDayton.nightcap/data/mocktail/asset_overlay`.

Roblox downloads some of these files, such as the shift lock and text cursors,
and reads no local file for them. A file under `content/` in the overlay folder
still replaces one: at start, Nightcap changes that file's name inside the
loaded Roblox library, in memory only, so Roblox reads the local file. Nothing
is changed when the overlay folder holds no file. Roblox does not allow
modified clients, so use this at your own risk.

```sh
dir=~/.local/share/mocktail/asset_overlay/content/textures
mkdir -p "$dir/Cursors/KeyboardMouse"
cp my-shift-lock.png "$dir/MouseLockedCursor.png"
cp my-text-cursor.png "$dir/Cursors/KeyboardMouse/IBeamCursor.png"
```

Set `MOCKTAIL_ASSET_TRACE=1` to print every file Roblox opens as
`[asset] open <name> -> <path>`, and every file it asks for and does not get as
`[asset] miss <name>`. A line that ends in `[overlay]` came from the overlay
folder. `MOCKTAIL_ASSET_OVERLAY_DIR` points the lookup somewhere else.

With `input.system_cursor` on, Roblox's arrow, hand and text cursors are
replaced by the desktop theme's, so overlay files for `ArrowCursor.png`,
`ArrowFarCursor.png` and `IBeamCursor.png` are not used. The shift lock
cursor still comes from the overlay. With only `input.hardware_cursor` on,
the overlay files are used and the desktop draws them.

An overlay file changes the texture's bytes, so a texture override keyed on the
original file's hash stops matching it. Use one of the two for a given texture.
