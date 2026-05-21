# Journey: Surface RT to Linux in one day

This is the chronological log of how this boot-shim came to exist. Started as "can you help me boot a Pi-like image on my Surface RT" and ended at a Linux root shell prompt 12+ hours later.

## Starting state

- Surface RT 1st gen, broken Windows RT install (BSOD: `INACCESSIBLE_BOOT_DEVICE`)
- Goal: "boot a Pi-like image"
- Available: an old 8GB SanDisk Cruzer, a 128GB microSD (claim), a jailbreak USB someone had made years ago
- Knowledge state: zero Linux experience

## Walls hit, in order

### Wall 1 — Windows RT recovery
Surface RT wouldn't boot. Microsoft pulled the official recovery image from their portal. Found archived copy of `SurfaceRT_BMR_20.8.17.0.zip` (3.0GB) on Internet Archive. Wrote to USB, booted Surface, Reset claimed success but Surface still wouldn't boot Windows → **eMMC has bad blocks in boot region** (13-year-old soldered flash dying). Couldn't fix RT side.

### Wall 2 — Boot Linux without working RT
Plan: apply Tegra Jailbreak USB (Golden Keys + Yahallo). Tegra Jailbreak USB worked. Golden Keys applied to NVRAM. Yahallo failed with "Failed to find the device" — firmware version `Aug 16 2013, 0x0` is too old for Yahallo's offset table.

Discovered Golden Keys alone is enough for unsigned EFI boot — Yahallo only matters for Windows RT app sandbox bypass. Pivoted to OpenRT Linux path.

### Wall 3 — 128GB microSD turns out to be 3.8GB
Card claimed 128GB but reported 3.75GB to the OS. Either counterfeit or wrong card. Pivoted to writing Pi OS to the 8GB SanDisk USB.

### Wall 4 — Pi OS bookworm too big for 8GB USB
`raspios-bookworm-full` decompresses to 11.4GB. Won't fit. Pivoted to `raspios-bullseye-armhf` which decompresses to 3.95GB. Fits with room.

### Wall 5 — Write the image
Tried balenaEtcher → `requestMetadata is not a function` bug. Pivoted to Rufus. Rufus initially refused `.img.xz` because file picker had `.iso/.img` filter active. Manually decompressed via Git's bundled `xz`, then Rufus accepted the raw `.img`.

### Wall 6 — Surface RT firmware refuses Pi OS bootarm.efi
Booted USB with Vol-Down + Power. Got Surface logo → 5-15 seconds → off. Pi OS's `bootarm.efi` is a TianoCore UEFI Shell built as `EFI_APPLICATION` (subsystem 10). Surface firmware silently rejects unsigned EFI applications even with Golden Keys policy applied (firmware honors Golden Keys for Microsoft-signed binaries only, not arbitrary unsigned ones).

### Wall 7 — APX mode (Tegra BootROM USB recovery)
Tried Fusée Gelée-style chip-level recovery. Needs USB-A male-male data cable. User had USB-C cable + 2x USB-A adapters which may or may not pass data. Tested across multiple 60–180s watch windows from desktop — no NVIDIA Tegra USB device ever enumerated. Either cable was charge-only or Surface couldn't sustain enough power to enter APX. Couldn't differentiate without testing cable separately with a phone (user opted to skip that test).

### Wall 8 — Apparent end
Reviewed structural difference between jailbreak USB (works) and Pi OS USB (doesn't). Jailbreak USB's `/efi/boot/bootarm.efi` had Windows Boot Manager strings (`[boot loader]`, `BOOTMGRDEFAULT=`, `bitlocker`) — it's Microsoft's signed WBM, not a custom binary. Pi OS's `bootarm.efi` is unsigned TianoCore. Firmware accepts Microsoft-signed, rejects unsigned.

### Insight — WBM + BCD chain
Read jailbreak BCD store with elevated `bcdedit /store /enum all`. Found entries with `nointegritychecks=Yes` `testsigning=Yes` `flightsigning=Yes` launching unsigned community binaries (`Yahallo.efi`, `SecureBootDebug.efi`). **WBM does launch unsigned binaries via BCD when those flags are set.**

### Wall 9 — Make WBM launch our Linux chain
Copied WBM (jailbreak's `bootarm.efi`) + `efi/microsoft/boot/` (BCD + fonts) onto SanDisk. Renamed Pi OS shell as `/efi/boot/shell.efi`. Modified BCD's `{default}` entry to point at `shell.efi` with `description="Boot Linux"`. Booted. WBM came up, menu showed "Boot Linux", selected it, got:

```
File: \efi\boot\shell.efi
Status: 0xc000007b
Info: The application or operating system couldn't be loaded because a required file is missing or contains errors.
```

`STATUS_INVALID_IMAGE_FORMAT`. WBM is stricter than the firmware: it requires PE subsystem 16 (`WINDOWS_BOOT_APPLICATION`), not 10 (`EFI_APPLICATION`).

### Wall 10 — Need a subsystem-16 binary that loads Linux
Tried patching shell.efi's subsystem byte from 10 to 16. WBM still rejected (additional PE format checks beyond subsystem).

Searched OpenRT community resources. Found **`imbushuo/boot-shim-tegra-3`** — a Tegra 3-specific patch of `imbushuo/boot-shim` that is exactly the kind of binary needed: Windows Boot Manager Application that chain-loads EFI code. But: no prebuilt binary exists. Only source.

### Detour — Build it ourselves
Cloned repo + gnu-efi submodule. Required Visual Studio 2017 with ARM tools per README. We had VS2022 — added MSVC v143 ARM toolchain via Installer GUI (~1.5GB).

Initial build errors:
- Wrong SDK version (`10.0.18362.0` not installed). Used `10.0.22621.0`.
- Newer SDK `10.0.26100.0` dropped ARM32 support entirely.
- Wrong platform toolset (`v142` from VS2019). Overrode with `v143`.
- Missing assembly file `src/ARM/ProcessorSupport.asm` (referenced in vcxproj but stripped from Tegra 3 fork). Removed reference + signtool postbuild. Build succeeded.

Tested as-is: Tegra 3 boot-shim's `efi_main` just prints "Fuck NVIDIA" 10 times and waits for keypress (literal source code, the original author's notes mention "it is 5am now"). Surface RT displayed it. **The WBM → Boot App → UEFI context switch worked.**

### Wall 11 — Make boot-shim launch shell.efi
Added chain-load logic to `efi_main`: read shell.efi via SimpleFileSystem, call `gBS->LoadImage(filepath)`. Got `Security Policy Violation`.

Tried `gBS->LoadImage(SourceBuffer)` instead of file-path mode. Still security violation.

Tried hooking `EFI_SECURITY_ARCH_PROTOCOL` and `EFI_SECURITY2_ARCH_PROTOCOL` (the classic Linux shim.efi technique). The first protocol was found and hooked. `LoadImage` still rejected. **Microsoft's UEFI on Surface RT does its own security check inline in LoadImage, not via the standard architectural protocols.**

### Wall 12 — Write a manual PE loader
Implemented in-source PE/COFF parser + loader:
- DOS header / PE signature / file header / optional header parsing
- Section copy by `VirtualAddress`
- Base relocation: `IMAGE_REL_BASED_HIGHLOW` (32-bit) and `IMAGE_REL_BASED_ARM_MOV32T` (Thumb-2 MOVW/MOVT pair). The MOV32T encoding splits a 16-bit immediate across `i`(1), `imm4`(4), `imm3`(3), `imm8`(8) bitfields — fiddly but documented.
- Allocate `EfiLoaderCode` pages, copy in, relocate
- Construct `EFI_LOADED_IMAGE` struct, `InstallProtocolInterface` for new handle
- Jump to entry point

Built. Booted. **The Pi OS UEFI shell launched.** Banner, mapping table, "Press ESC in 1 seconds to skip startup.nsh". `Shell> fs0:`. `FS0:\> zImage dtb=...`. **`Script Error Status: Security Violation (line number 2)`** — shell tried to LoadImage the kernel but firmware refused.

### Wall 13 — Hook UEFI Boot Services
Replaced `gBS->LoadImage` and `gBS->StartImage` function pointers in the Boot Services table before launching shell. Hooks track manually-loaded handles in a small table and route Start/Load through the manual loader on Security Violation. Recomputed BootServices header CRC32 (firmware may verify).

Built. Booted. Shell came up, processed startup.nsh, zImage was loaded via our manual loader (no Security Violation this time), kernel entry was called via our hooked StartImage.

But: kernel entry never returned, screen frozen at "calling entry at 0x...".

### Wall 14 — Add ARM cache coherency
Wrote D-cache clean by MVA (DCCMVAC) over loaded image + ICIALLU + BPIALL + DSB + ISB sequence so the CPU executed our copied bytes from memory, not stale cached data. Built. Booted. Same hang.

### Wall 15 — Diagnose what kernel sees
Added diagnostic Prints in hooks. Booted. Saw:
- Kernel machine = 0x1C2 (claimed Thumb)
- SizeOfImage = 8635392
- EntryRVA = 0x809C — **bit 0 clear**
- ImageBase = 0x0
- DLLCharacteristics = 0x0 (not relocatable)
- No `.reloc` section, all DataDirectory entries empty

Examined the actual bytes at the kernel entry RVA from disk. Disassembled mentally:
```
809C: 24 31 9F E5  F0 40 2D E9  03 30 8F E0  01 40 A0 E1
```
That's `E5 9F 31 24` (ARM LDR), `E9 2D 40 F0` (ARM PUSH) — **4-byte ARM mode instructions, not Thumb.**

The boot-shim was unconditionally OR'ing the entry address with 1 to set the Thumb mode bit (based on machine field). For zImage, EntryRVA's bit 0 is 0, signaling ARM mode. The OR was making the CPU enter Thumb mode when it should be ARM, causing immediate execution of garbage.

**Fix:** trust EntryRVA's bit 0, don't force Thumb. Rebuilt. Booted.

### Breakthrough
```
EFI stub: Booting Linux Kernel...
EFI stub: DEBUG: Free memory starts at 0x82452000, setting kernel_base to 0x82600000
EFI stub: DEBUG: image_addr == 0x82608000, reserve_addr == 0x82603000
EFI stub: Entering in SVC mode with MMU enabled
EFI stub: UEFI Secure Boot is enabled.
EFI stub: ERROR: Ignoring DTB from command line.
EFI stub: Generating empty DTB
EFI stub: Exiting boot services...
```

Linux kernel was actually executing. EFI stub printing. But — `UEFI Secure Boot is enabled. Ignoring DTB from command line.` The kernel was refusing our DTB because of its own Secure Boot policy. Generated an empty DTB and ExitBootServices'd into a hardware-unaware kernel that immediately hung.

### Wall 16 — Spoof SecureBoot variable
Added `gRT->GetVariable` hook. When kernel reads the `SecureBoot` UEFI variable, return value 0 (off). Forward all other variable reads to the original handler. Recomputed RuntimeServices CRC32.

Built. Booted.

### Result
```
[ 0.000000] Booting Linux on physical CPU 0x0
[ ... ]
[ 3.342B01] usb 1-1.4: New USB device strings...
[ ... ]
[ 3.946921] EXT4-fs (sda2): mounted filesystem ... on device 8:2
[ 3.977761] VFS: Mounted root (ext4 filesystem) ...
[ 3.989211] Run /bin/sh as init process
#
```

**Root shell on Surface RT.** 4 penguin logos at the top of the screen (4 CPU cores SMP). USB stack online, DisplayLink and AirPod Case (lol) recognized as HID. ext4 rootfs mounted. `/bin/sh` running as PID 1.

## Total elapsed

About 12 hours from "won't boot" to root shell, including ~3 hours of stuck pivots on cable/charging early in the day.

## Lessons

1. **The right wall to climb is the documented one.** APX mode was a tempting Hail Mary but cable verification took longer than expected; meanwhile the WBM/BCD path was sitting right there in plain sight.

2. **PE machine field is advisory, not authoritative.** Linux kernel labels itself THUMB but the entry point is ARM mode. Honor `EntryRVA & 1` for actual mode.

3. **Boot Services table is mutable post-firmware-handoff.** Replace function pointers freely; recompute the header CRC. Firmware doesn't re-verify.

4. **Multiple Secure Boot enforcement points exist.** Defeating firmware's load check via custom PE loader doesn't help with the kernel's own SB policy, which uses a separate variable lookup.

5. **The "I've never used Linux" user ended the day at a Linux root shell on a device that has no documented Linux path for its firmware revision.** Worth remembering.
