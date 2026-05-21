# Surface RT Linux Boot-Shim

Fork of [imbushuo/boot-shim-tegra-3](https://github.com/imbushuo/boot-shim-tegra-3) extended into a complete **Linux boot chain for first-generation Microsoft Surface RT** (Tegra 3, Aug 2013 firmware), bypassing every layer of Microsoft's UEFI Secure Boot enforcement without modifying the device firmware.

## What this does

Boots an unmodified Raspberry Pi OS (`raspios-bullseye-surface-rt-armhf`) ARM Linux kernel on a stock Surface RT 1 via the following chain:

```
Firmware (signed bootmgfw.efi via /efi/boot/bootarm.efi)
   ↓
Windows Boot Manager (loads via Microsoft's signed cert)
   ↓
BCD entry with nointegritychecks=Yes
   ↓
This boot-shim binary (subsystem=16 BOOT_APPLICATION, ARMNT)
   ↓
Manual PE loader (bypasses gBS->LoadImage Secure Boot check)
   ↓
TianoCore EDK II UEFI Shell (Pi OS bootarm.efi)
   ↓
startup.nsh executes Linux kernel command
   ↓
gBS->LoadImage / StartImage hooks redirect to our manual loader
   ↓
gRT->GetVariable hook spoofs SecureBoot=0
   ↓
ARM Linux kernel boots normally, mounts ext4 rootfs, init=/bin/sh
   ↓
Root shell prompt on Surface RT display
```

## Acknowledged base

This repo started as a fork of **imbushuo/boot-shim-tegra-3** which was itself a Tegra 3-specific patch of **imbushuo/boot-shim**. The original boot-shim was a proof-of-concept Windows Boot Manager Application demonstrating context switch from Boot Manager to UEFI on Qualcomm Lumia devices. The Tegra 3 fork added Surface RT screen support but stripped the actual chain-load logic, leaving only a `printf` demo. This fork rebuilds and extends the chain-load logic specifically for booting Linux.

## Verified working on

- **Device:** Microsoft Surface RT (1st generation, 2012), Tegra 3 (Cortex-A9 ARMv7 quad-core), 2GB RAM
- **Firmware:** `OemkS EFI Aug 16 2013 14:55:40, 0x00000000` — early Windows RT 8 firmware that predates most documented community work
- **Distro:** `2023-05-03-raspios-bullseye-surface-rt-armhf` (OpenRT community build)
- **Boot media:** Single USB stick (SanDisk Cruzer 8GB)

## What was bypassed / hooked

| Layer | Block | Bypass |
|---|---|---|
| UEFI Secure Boot policy | Refuses unsigned EFI binaries from firmware boot | Microsoft-signed Windows Boot Manager (already trusted, applied via leaked Golden Keys debug policy) is the firmware-loaded entry point. WBM accepts unsigned children via `nointegritychecks=Yes` BCD flag. |
| WBM image format filter | Rejects EFI_APPLICATION subsystem (10) binaries | Build as WINDOWS_BOOT_APPLICATION (subsystem 16) with ARMNT machine type. |
| Post-UEFI `gBS->LoadImage` Secure Boot check | Refuses unsigned binaries (kernel, shell) launched from inside UEFI after WBM exits | Replace `gBS->LoadImage` and `gBS->StartImage` function pointers with hooks. Hooks fall back to manual PE loader on Security Violation. |
| Microsoft's internal LoadImage validation | Not exposed via standard `EFI_SECURITY_ARCH_PROTOCOL`, has its own inline check | Manual PE loader (parse, allocate, copy sections, apply relocations, install LoadedImage protocol, jump to entry) — never calls firmware's LoadImage. |
| Linux kernel's "Secure Boot = reject dtb=cmdline" policy | Kernel refuses to load DTB from command line when it detects SB enabled | `gRT->GetVariable` hook returns `SecureBoot = 0` for that specific variable. |
| ARM/Thumb mode entry confusion | Default behavior of OR'ing entry address with 1 for Thumb mode | Linux ARM kernel's EFI stub is built as ARM mode (32-bit instructions) despite machine field claiming `IMAGE_FILE_MACHINE_THUMB` (0x1C2). Trust `EntryRVA`'s bit-0 mode encoding instead of forcing Thumb. |

## Architecture of the bypass

### Stage 1 — Firmware load
Surface RT firmware boots `/efi/boot/bootarm.efi` from the FAT32 USB. This file is the standard Microsoft-signed Windows Boot Manager (~770KB, identifiable by Windows-specific strings like `[boot loader]`, `BOOTMGRDEFAULT=`, `bitlocker`). Because it's Microsoft-signed, firmware trusts it unconditionally.

### Stage 2 — Boot Manager BCD
WBM reads `/efi/microsoft/boot/bcd` and finds our custom Boot Application entry:

```
identifier              {default}
device                  boot
path                    \efi\boot\bootshim-test.efi
description             Boot Linux
nointegritychecks       Yes      ← skip signature verify
testsigning             Yes
isolatedcontext         Yes
```

WBM launches the boot-shim binary because the flags tell it to skip integrity checks.

### Stage 3 — Boot-shim context switch
Our boot-shim is built with subsystem = `IMAGE_SUBSYSTEM_WINDOWS_BOOT_APPLICATION` (16) and machine type `IMAGE_FILE_MACHINE_ARMNT` (0x1C4) — the only PE configuration WBM will load via BCD.

`BlApplicationEntry` runs in WBM context. It calls `SwitchToRealModeContext()` (the inherited Tegra 3 code from imbushuo's fork) which manipulates ARM CP15 system registers to switch back from Boot Manager context to a normal UEFI environment, then calls `efi_main`.

### Stage 4 — Install Boot Services hooks
Before doing anything else, `efi_main` saves and replaces:

- `gBS->LoadImage` → `HookedLoadImage`
- `gBS->StartImage` → `HookedStartImage`
- `gRT->GetVariable` → `HookedGetVariable`

Then recomputes the CRC32 of the BootServices and RuntimeServices tables (firmware checks these).

### Stage 5 — Manual load of UEFI shell
The boot-shim reads `shell.efi` (the Pi OS-bundled TianoCore EDK II UEFI shell) from the same USB and runs it through `ManualLoad()`:

1. Parse PE/COFF headers
2. `AllocatePages(EfiLoaderCode, ...)` for `SizeOfImage`
3. Copy each section from the file at `PointerToRawData` to `ImageBase + VirtualAddress`
4. Apply base relocations:
   - `IMAGE_REL_BASED_HIGHLOW` — 32-bit absolute address fixup
   - `IMAGE_REL_BASED_ARM_MOV32T` — Thumb-2 MOVW/MOVT instruction pair fixup (encodes 16-bit immediates split across `i`, `imm3`, `imm4`, `imm8` bitfields)
5. ARM cache coherency:
   - Loop over image, `DCCMVAC` each 32-byte cache line (Cortex-A9 line size) to clean D-cache to memory
   - DSB
   - `ICIALLU` (invalidate entire I-cache) + `BPIALL` (invalidate branch predictor)
   - DSB + ISB
6. Allocate `EFI_LOADED_IMAGE` struct, set `DeviceHandle`, `ImageBase`, `ImageSize`
7. `InstallProtocolInterface` for `EFI_LOADED_IMAGE_PROTOCOL_GUID` on new handle
8. Track `(handle, entry_address)` in internal table for later StartImage lookup
9. Jump to entry — `EntryRVA`'s bit 0 already encodes ARM/Thumb mode, do **not** force Thumb bit

The shell launches and reads startup.nsh from FS0.

### Stage 6 — Shell launches kernel
`startup.nsh` contains:
```
fs0:
zImage dtb=tegra30-microsoft-surface-rt-efi.dtb root=PARTUUID=... rootwait rw console=tty0 earlycon=efi efi=debug loglevel=8 initcall_debug cpuidle.off=1 init=/bin/sh panic=30
reset -s
```

Shell calls `gBS->LoadImage(zImage)`. Our hook:
1. Tries original LoadImage — returns `EFI_SECURITY_VIOLATION`
2. Falls back: reads zImage via SimpleFileSystem, calls our `ManualLoad`
3. Returns the new handle to shell

Shell sets `LoadedImage->LoadOptions = kernel_cmdline` then calls `gBS->StartImage`. Our hook:
1. Sees handle is in our tracked table
2. Calls the kernel entry directly with `(handle, gST)`

### Stage 7 — Linux EFI stub runs
Kernel's ARM EFI stub:
1. Sets up its own environment
2. Calls our hooked `gRT->GetVariable("SecureBoot")` → we return 0
3. Sees Secure Boot is "disabled" → accepts the `dtb=` cmdline arg
4. Reads the DTB file from FAT32 via SimpleFileSystem
5. Allocates real kernel memory, decompresses, sets up DTB
6. Calls `ExitBootServices` — UEFI is gone
7. Jumps to decompressed kernel

### Stage 8 — Linux runs
Kernel boots normally, brings up 4 cores (penguin logos appear), initializes USB stack, mounts ext4 rootfs from sda2, execs `/bin/sh` as init. Root shell prompt.

## File layout on the boot USB

```
/efi/boot/bootarm.efi          ← Microsoft Windows Boot Manager (copied from jailbreak USB)
/efi/boot/bootshim-test.efi    ← Our compiled boot-shim
/efi/boot/shell.efi            ← TianoCore EDK II UEFI Shell (from Pi OS image)
/efi/microsoft/boot/bcd        ← BCD store (modified from jailbreak USB's)
/efi/microsoft/boot/fonts/*    ← Required by WBM
/startup.nsh                   ← Kernel boot command
/zImage                        ← Linux kernel
/tegra30-microsoft-surface-rt-efi.dtb  ← Device tree
... (Pi OS Pi-specific files, unused on Surface RT but harmless)
```

## Prerequisites

### One-time on the Surface RT
- Working firmware (boots into recovery / jailbreak USB)
- Golden Keys debug policy applied to NVRAM (via Tegra Jailbreak USB)
- Boot USB written with bullseye Pi OS image
- Jailbreak USB's WBM + BCD + boot-shim copied onto the boot USB (see [bootloader-setup.md](#))

### On the build machine (Windows)
- Visual Studio 2022 Community
- Individual component: **MSVC v143 - VS 2022 C++ ARM build tools (Latest)**
- Windows SDK 10.0.22621.0 (newer SDKs dropped ARM32 support)
- Git for Windows

## Build

```
git clone --recursive https://github.com/Evan-Haug/surface-rt-linux-bootshim
cd surface-rt-linux-bootshim
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "Boot Shim.sln" /p:Configuration=Debug /p:Platform=ARM /p:WindowsTargetPlatformVersion=10.0.22621.0 /p:PlatformToolset=v143
```

Output: `build\BootShim\ARM\Debug\BootShim.efi` (~46KB)

Copy to USB as `\efi\boot\bootshim-test.efi`, ensure BCD entry points at this path.

## Known limitations / future work

- **No serial console output.** Kernel boots but if anything goes wrong post-`ExitBootServices`, you see a frozen screen. Surface RT has internal UART test pads — tapping them with a USB-UART dongle would unlock real kernel debugging.
- **Touchscreen not yet configured.** Linux detects the I2C touchscreen but X11 input mapping needs `xinput_calibrator` work. Plug in a USB keyboard for now.
- **WiFi/BT** may need firmware blobs the bullseye Pi OS image already includes.
- **No SMP boot verification beyond "4 penguins shown."** Real perf testing not done.
- **Battery / charging path bypassed** — this particular Surface RT has hardware charging issues; the work is reproducible on a Surface RT in better hardware shape with the original Microsoft charger.

## Files of interest in this repo

- `src/EFIApp.c` — main boot-shim source with manual PE loader, hooks, and bypass logic
- `src/Context.c` — Tegra 3 context switch (inherited from upstream)
- `src/BlApplicationEntry.c` — BootApp entry, calls `SwitchToRealModeContext` then `efi_main`
- `contrib/msvc/uefi-simple.vcxproj` — VS2022/SDK22621 build config (patched from upstream)
- `JOURNEY.md` — chronological story of how this was built in one day

## License

GPL-2.0 (inherited from gnu-efi submodule). See `COPYING.md`.

## Credits

- **imbushuo** — original boot-shim and Tegra 3 fork; the screen-output trick is theirs
- **OpenRT community** — Pi OS for Surface RT, Tegra Jailbreak USB, documentation
- **leaked Microsoft Golden Keys debug policy** — without which none of this would work
- The eight-hour debugging session that culminated in 4 Tux penguins on a 13-year-old locked tablet
