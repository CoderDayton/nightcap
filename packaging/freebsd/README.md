<!-- Modified by vii from komaruworld/mocktail. See the top-level README "About this fork". -->
# FreeBSD Linuxulator

Linuxulator support is experimental. The original setup was tested on FreeBSD
15.1-RELEASE-p2 with Fedora 44 x86-64. Additional workarounds below were tried
on 15.1-RELEASE-p7 with Manjaro, an RX 7600 and KDE X11; they are not guaranteed
to work elsewhere.

## Fix `/proc` filesystem type

FreeBSD reports the wrong filesystem type for Linux `/proc`; Roblox may
therefore disconnect with Error 304. [`linuxulator.patch`](linuxulator.patch)
corrects it.

Use a clean `/usr/src` checkout that exactly matches the running kernel. The
original tested source revision was `aadd58dddcbc78f4d5594827b46b5633552b15ce`.
As root, for an amd64 `GENERIC` kernel:

```sh
fetch -o /root/linuxulator.patch https://raw.githubusercontent.com/CoderDayton/nightcap/main/packaging/freebsd/linuxulator.patch
cd /usr/src
git apply --check /root/linuxulator.patch
git apply /root/linuxulator.patch
make -j"$(sysctl -n hw.ncpu)" kernel-toolchain
make -j"$(sysctl -n hw.ncpu)" buildkernel KERNCONF=GENERIC MODULES_OVERRIDE=linux64
```

Install the module separately so the stock `/boot/kernel/linux64.ko` remains
available. First inspect the destination and back it up if it already exists;
`install` overwrites it. Keep console access and a known-good boot environment.

```sh
ls -l /boot/modules/linux64-mocktail.ko
install -o root -g wheel -m 0444 /usr/obj/usr/src/amd64.amd64/sys/GENERIC/modules/usr/src/sys/modules/linux64/linux64.ko /boot/modules/linux64-mocktail.ko
sysrc -f /boot/loader.conf linux64_load=YES
sysrc -f /boot/loader.conf linux64_name=/boot/modules/linux64-mocktail.ko
reboot
```

After reboot, confirm `kldstat -v` lists the **alternate path** and that Linux
`/proc` reports `9fa0`:

```sh
kldstat -v
chroot /compat/linux stat -f -c %t /proc
```

If loading the alternate fails, the Linux startup service may fall back to the
stock module. To roll back, remove the two loader entries and reboot. Both files
provide the same `linux64elf` identity; do not load them together. Rebuild the
custom module after kernel updates. Live module reloads can disrupt Linux
processes or crash the system; rebooting is safer.

## Run Nightcap

Run the Nightcap AppImage from the
[latest release](https://github.com/CoderDayton/nightcap/releases/latest) using
the Linux userspace.

## Audio

For Fedora, install `alsa-lib` and `alsa-plugins-oss` in the Linux userspace.
Nightcap plays through `/dev/dsp`, FreeBSD's default sound device. To use
another one, find its number in `cat /dev/sndstat` and run
`sysctl hw.snd.default_unit=N`.

## Troubleshooting

These workarounds are experimental. Check whether a patch is already present
before applying it, and keep a recovery path when changing kernel modules.

### Linux Vulkan cannot find the Radeon GPU

If `/sys/class/drm/renderD128` and `/sys/dev/char/226:128` are missing in the
Linux userspace, the upstream [linsysfs render-node patch](https://reviews.freebsd.org/D59190)
may help. On the p7 setup, backporting it and loading a separate `linsysfs`
module made Linux RADV detect the RX 7600. Build it against matching kernel
source, preserve the stock module, and select the alternate at boot:

```conf
linsysfs_load="YES"
linsysfs_name="/boot/modules/linsysfs-mocktail.ko"
```

After reboot, verify the loaded path with `kldstat -v`, inspect the sysfs paths,
and run Linux `vulkaninfo --summary` as the desktop user. To roll back, remove
the loader entries and reboot. GPU detection does not guarantee presentation.

An upstream [DMA-buffer and sync-file ioctl patch](https://reviews.freebsd.org/D58753)
was also backported into the custom `linux64` module after a Vulkan swapchain
failure. Its effect was not established. If trying it, preserve the `/proc`
patch when rebuilding the module.

If Linux Vulkan tries to load FreeBSD ICD libraries, scope the correct **Linux**
ICD to Nightcap. This example is for the tested Manjaro AMD setup:

```sh
VK_DRIVER_FILES=/compat/linux/usr/share/vulkan/icd.d/radeon_icd.json \
VK_ICD_FILENAMES=/compat/linux/usr/share/vulkan/icd.d/radeon_icd.json \
  ./Nightcap-x86_64.AppImage
```

Use the path for your GPU and distribution; do not export it globally.

### Shader pack exists but fails to open

When `/compat/linux/home/USER` exists as its own directory, Linuxulator creates
new files and directories there whenever their parent exists there, even if
they already exist in `/home/USER`. Nightcap's data then splits between the two
trees. Nightcap shows a warning at startup when it finds this.

Mount the real home over the copy so both paths are the same directory:

```sh
# As root. The mount hides what is in the copy; move anything you need out first.
mount -t nullfs /home/USER /compat/linux/home/USER
```

To keep it after a reboot, add this line to `/etc/fstab`:

```text
/home/USER  /compat/linux/home/USER  nullfs  rw,late  0  0
```

To undo it, unmount the target and remove the fstab line. An isolated Xvfb test
cannot verify hardware presentation because it lacks DRI3.
