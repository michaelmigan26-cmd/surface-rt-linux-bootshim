# Deployment Guide: Surface RT Linux from scratch

End-to-end procedure to take a Surface RT 1st gen with broken Windows RT and boot Linux on it. Assumes you have the build output (`BootShim.efi`) from this repo.

## Hardware needed

- **Surface RT 1st gen**, charged (genuine Microsoft 24W charger preferred)
- **USB stick**, 8GB+, FAT32-capable
- **Powered USB hub** (Surface RT has only one USB port; you need keyboard + storage at the same time)
- **USB keyboard** for navigating the boot menu and Linux shell
- **Windows PC** to build the boot USB

## Files needed

| File | Source |
|---|---|
| `BootShim.efi` | This repo, build output `build\BootShim\ARM\Debug\BootShim.efi` |
| `2023-05-03-raspios-bullseye-surface-rt-armhf.img.xz` | https://files.open-rt.party/Linux/Distro/ (964 MB) |
| Tegra Jailbreak USB v1.61 | https://mega.nz/file/UjkUVZBI#mLH3BSUk18UT6YC3KMhM5ExWug-LaQsy66XnWS6slh4 |
| Surface RT recovery image (optional, if device won't boot) | https://archive.org/details/surface-rt-8.1 |
| Rufus 4.x | https://rufus.ie |

## Procedure

### Step 1 — Apply Golden Keys to Surface RT (one time, persistent)

If the Surface RT has never been jailbroken:

1. Format USB-A flash drive as **FAT32**
2. Extract Tegra Jailbreak USB v1.61 zip contents to USB root
3. Plug hub into Surface RT, plug USB stick + USB keyboard into hub
4. Power off Surface RT (hold power 10s)
5. Hold **Vol-Down**, tap Power, keep holding Vol-Down — boots USB
6. Menu appears. Arrow to **Install Golden Keys**, Enter.
7. Surface reboots, shows Microsoft Secure Boot debug policy prompt — **press Power** to highlight Accept, then tap touchscreen on Accept (or hit Enter on USB keyboard)
8. Device reboots and may sit at black screen. Power off.

Golden Keys is now persistent in NVRAM. Survives power-off, even battery removal.

### Step 2 — Write Pi OS image to boot USB

On Windows PC:

1. Decompress the bullseye image: `xz -dk 2023-05-03-raspios-bullseye-surface-rt-armhf.img.xz` (or use Rufus's built-in `.xz` support in Rufus 4.x)
2. Run **Rufus** as administrator
3. Device dropdown → your boot USB stick (verify size matches!)
4. SELECT → pick the `.img` file
5. If prompted "DD Image" vs "ISO" → choose **DD Image**
6. START — wait for completion (~10 minutes on USB 2.0)

The USB now has:
- Partition 1 (FAT32, "bootfs", 256 MB) — Pi OS boot files including `bootarm.efi` (UEFI shell), `zImage`, DTBs, `startup.nsh`
- Partition 2 (ext4, ~3.7 GB) — Pi OS rootfs

### Step 3 — Overlay WBM + custom BCD + boot-shim

On Windows PC, with the boot USB still inserted (drive letter F: assumed below):

1. **Preserve the Pi OS UEFI shell:**
   ```
   copy F:\efi\boot\bootarm.efi F:\efi\boot\shell.efi
   ```

2. **Replace `bootarm.efi` with Windows Boot Manager** (from the Tegra Jailbreak USB's `\efi\boot\bootarm.efi`):
   ```
   copy <jailbreak-usb>\efi\boot\bootarm.efi F:\efi\boot\bootarm.efi
   ```

3. **Copy Microsoft boot manager support files:**
   ```
   xcopy <jailbreak-usb>\efi\microsoft F:\efi\microsoft /S /I
   ```

4. **Copy this repo's built boot-shim:**
   ```
   copy build\BootShim\ARM\Debug\BootShim.efi F:\efi\boot\bootshim-test.efi
   ```

5. **Modify BCD entry** (requires admin cmd):
   ```
   bcdedit /store F:\efi\microsoft\boot\bcd /set {default} path \efi\boot\bootshim-test.efi
   bcdedit /store F:\efi\microsoft\boot\bcd /set {default} description "Boot Linux"
   bcdedit /store F:\efi\microsoft\boot\bcd /deletevalue {default} loadoptions
   ```

6. **Edit `F:\startup.nsh`** to:
   ```
   fs0:
   zImage dtb=tegra30-microsoft-surface-rt-efi.dtb root=PARTUUID=<your-partition-uuid>-02 rootwait rw console=tty0 cpuidle.off=1 panic=30
   reset -s
   ```
   To find the PARTUUID, run from a Linux machine: `blkid` while the USB is plugged in. Or read it from the existing `cmdline.txt` on the boot partition.

7. Safely eject USB.

### Step 4 — Boot Linux

1. Plug USB hub into Surface RT, plug boot USB + USB keyboard into hub
2. Power off Surface RT
3. Hold **Vol-Down**, tap Power, keep holding Vol-Down
4. Windows Boot Manager menu appears with "Boot Linux" highlighted
5. Press Enter on USB keyboard (or wait 30s for default)
6. Boot-shim diagnostic prints scroll past
7. UEFI shell shows briefly
8. Kernel boot text floods the screen
9. **Root shell prompt `#`**

## Verifying

At the root shell, type (USB keyboard required for input):
- `uname -a` — confirms ARM Linux running on Surface RT
- `cat /proc/cpuinfo` — shows 4 Tegra 3 Cortex-A9 cores
- `free -m` — shows ~2GB RAM
- `lsblk` — shows the USB storage
- `dmesg | grep -i tegra` — shows Tegra-specific driver loads

## If boot fails

The boot-shim prints comprehensive diagnostics. Compare against expected output in `JOURNEY.md`. Likely failure modes:

| Symptom | Cause | Fix |
|---|---|---|
| Surface logo, then off, no text | WBM not finding/launching boot-shim | Verify BCD entry path matches actual file location |
| `Status: 0xc000007b` | Wrong PE subsystem or machine type | Rebuild boot-shim, verify subsystem=16 and machine=0x1C4 |
| `Security Policy Violation` from shell | LoadImage hook didn't install | Check boot-shim built correctly, BCD has `nointegritychecks Yes` |
| Kernel "calling entry" then hang | Wrong ARM/Thumb mode | Verify boot-shim has the EntryRVA bit-0 fix (commit history) |
| Kernel hangs after "Exiting boot services" | DTB rejection from SecureBoot check | Verify `gRT->GetVariable` hook is installed |

## After first boot

Edit `/etc/fstab` to mount rootfs read-write persistently. Configure WiFi via `raspi-config`. Install a desktop environment with `sudo apt install task-lxde-desktop`. Pin to memory: `Welcome to Linux`.
