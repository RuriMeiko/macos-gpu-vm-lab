# M1 notes: macOS Tahoe on QEMU/KVM, Ryzen host

Findings so far (2026-10-22)

- OSX-KVM (kholia) OpenCore boots Tahoe's recovery image on a Ryzen 7 5800X host with
  `-cpu Skylake-Client,-hle,-rtm,...`; no AMD kernel patches needed because the guest sees an Intel CPU.
- **SMBIOS must not be iMac19,1**: Tahoe does not support it. Use `MacPro7,1` (edit `SystemProductName`).
- Serving the EFI folder with `-drive file=fat:rw:./esp,format=raw` avoids needing root to rebuild OpenCore.qcow2.
- `fetch-macOS-v2.py` ignores `-o` for some versions and its image check fails without a TTY
  (`os.get_terminal_size`); verify the chunklist yourself (all chunks matched).
- Verbose boot (`-v keepsyms=1 debug=0x100`) reached launchd and recoveryosd, then **hung at
  `KextLog: Loading FileSet KC(s)` with all vCPUs at 100%** while `kvm.ignore_msrs=N`.
  OSX-KVM states `ignore_msrs=1` is required. Next step: set it and retest.

- 2026-10-22 continued: dumped 8.1GB guest RAM during the hang and grepped it: **no panic,
  no fail-safe spin record** — the vCPUs spin in-kernel without panicking. Config diff vs
  upstream OSX-KVM shows only intended changes (SMBIOS, boot-args, WEG disabled). Suspicion
  set: Tahoe recovery + 4-way SMP + virtio? Next: boot with fewer changes (no audio device,
  ehci added like upstream, -smp 4 keep), and try `-cpu` tweak only if needed.

## M1 SOLVED (2026-10-22 12:30 +07)

**Result: Tahoe recoveryOS reaches the full macOS Utilities UI (Reinstall macOS Tahoe /
Disk Utility / Time Machine / Web Browser) on Ryzen 5800X KVM.**

Root causes fixed, in order:

1. **AppleIntelMCEReporter panic (page fault in AppleIntelMCE10Controller::start)** — the real
   kernel crash behind every "silent reboot". Blocked two ways:
   - `Kernel > Block` entry for `com.apple.driver.AppleIntelMCEReporter` (kept as belt-and-braces)
   - the working fix: **AppleMCEReporterDisabler.kext with CORRECT OpenCore paths**. OpenCore
     path convention: `BundlePath` = folder name relative to `EFI/OC/Kexts` root naming, and
     `PlistPath`/`ExecutablePath` are **relative to the kext bundle itself**
     (`Contents/Info.plist`, NOT `Kexts/X.kext/Contents/Info.plist`). Our first "fix" had them
     backwards; also the bundle folder must be named exactly like `BundlePath`
     (renamed `MCEReporterDisabler.kext` -> `AppleMCEReporterDisabler.kext`).
     Info-only kext: no ExecutablePath key.
2. **QEMU vvfat crashes** (`check_directory_consistency` assertion) after minutes of runtime —
   replaced `file=fat:rw:./esp` with a real FAT32 image built by mtools:
   `~/vm/m1/rebuild-esp.sh` rebuilds `OpenCore.img` from the `esp/` folder after any edit.
3. Boot hangs seen on the way (all CPUs idle-spin, serial stops at launchd) were the SAME MCE
   panic in different visibility modes: with `debug=0x100` it halts and prints "Waiting for
   remote debugger", without it the guest reboots (serial shows a fresh BdsDxe pass).

Also noted:
- `ignore_msrs=1` still required (set via pkexec; not persistent).
- kexts Lilu/VirtualSMC/WhateverGreen/VoodooPS2/USBToolBox disabled in this config for now
  (not present on disk in correct form); recovery boots fine without them.
- VM: 8GB RAM, 4 vCPU Skylake-Client,-hle,-rtm, MacPro7,1 SMBIOS, no -no-reboot currently.
- Next (M1 -> install): boot picker -> Reinstall macOS Tahoe needs the full InstallAssistant
  (BaseSystem only has recovery) — download full installer via `macrecovery`/`fetch-macOS-v2`
  full image or use `InstallAssistant.pkg` from swcdn; or boot `Reinstall macOS Tahoe` which
  downloads online via recovery network.

Current state at save time: VM still running with recovery UI up.

## M1 continued: install started (2026-10-22 ~13:20 +07)

- Re-added the target disk. AHCI ports >3 invisible to macOS (tried sata.4/.5/.0 — Disk Utility
  never listed the disk); **moving it to NVMe worked**: `-device nvme,drive=MacHDD,serial=MACDISK1`.
- Disk Utility (via VNC automation) erased the 85.9GB NVMe to APFS/GUID ("Untitled", container disk3).
- Note: HMP mouse_move/mouse_button do NOT work for the macOS GUI (only OpenCore picker); keyboard
  sendkey works in firmware only. Control of the GUI is via VNC: QEMU `-display none -vnc 127.0.0.1:5`
  + vncdotool (installed into ~/.hermes/venvs/cybersec). vncdo key names: `return` not `ret`,
  `super` = Cmd. OpenCore picker: `keydown right/keyup right` then `return`.
- Install flow automated so far: Utilities > Reinstall macOS Tahoe > Continue > Agree x2 >
  select "Untitled" (85.69GB) > Continue. Installer is now copying (progress bar running).
- 3h20m remaining is the initial estimate (emulated NVMe is slow); expect it to drop.

Next: wait for install completion, VM auto-reboots into macOS, remove BaseSystem/OpenCore boot
ordering (OpenCore will boot the new "Macintosh HD" or select via picker), then M2 (virtio-gpu kext).
