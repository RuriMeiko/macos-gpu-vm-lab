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
