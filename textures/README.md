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
