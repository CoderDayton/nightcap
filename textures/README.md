# Texture overrides

Optional PNGs you can drop into your own override directory. Nothing here is
installed or loaded automatically.

## Using one

Copy the file into the override directory, keeping its name:

```sh
mkdir -p ~/.config/mocktail/textures
cp textures/75dcd1166a6cbaa9.png ~/.config/mocktail/textures/
```

Restart the game. The log line `[vulkan] texture override loaded: <path>`
confirms it was picked up.

To remove it, delete the file again.

`MOCKTAIL_TEXTURE_OVERRIDE_DIR` points the lookup somewhere else.

## What is here

| File | Replaces |
| --- | --- |
| `75dcd1166a6cbaa9.png` | The in-game crosshair, as a thin dot-and-ticks reticle (128x128). |

## Naming

The file name is the 16 hex digit FNV-1a hash of the texture's level-0
compressed ETC2 bytes, which is what the override lookup keys on. A texture
Roblox re-encodes gets a new hash, so an override can stop matching after a
client update.

To find the hash of a texture you want to replace, set
`MOCKTAIL_TEXTURE_DUMP_DIR` to an empty directory and play until the texture
appears. Every uploaded texture is written there once as
`<hash>_<width>x<height>.png`. Rename the one you want to `<hash>.png` and put
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

Set `MOCKTAIL_ASSET_TRACE=1` to print every file Roblox opens as
`[asset] open <name> -> <path>`. A line that ends in `[overlay]` came from the
overlay folder. `MOCKTAIL_ASSET_OVERLAY_DIR` points the lookup somewhere else.

An overlay file changes the texture's bytes, so a texture override keyed on the
original file's hash stops matching it. Use one of the two for a given texture.
