# Handoff plan: make macOS Tahoe use Metal through a virtual GPU on Linux/KVM

Goal: macOS guest (no passthrough) gets a Metal GPU backed by the Linux host's Vulkan GPU. Repo is PolyForm
Noncommercial; personal/research use. Everything below is cheap to attempt and reversible. Work in loops: one
hypothesis -> one experiment cycle (~6 min) -> record result -> next.

## Where we are (verified, see docs/M5*.md)
- Tahoe 26.7.1 boots and runs a full 2D desktop on virtio-vga (docs/M7-STATUS.md).
- The guest ships Apple's own paravirtual Metal driver (AppleParavirtGPU, PCI 106b:eeee). Our QEMU stub
  `applegfx-trace` (m5/applegfx-trace.c) makes it bind and start talking.
- Decoded so far (docs/M5C-FIFO-PROTOCOL.md): register block at BAR0+0x1000, write/read pointers, command FIFO in guest
  RAM at (PFN<<12)+0x1000, messages `[opcode][len][seq][payload]`. Driver sends 7 messages then waits for a reply.
- NOT known: where/how the device must reply (probably fifoB at reg +0x1c and an MSI), meaning of opcodes.

## Fixed environment (do not change without reason)
| thing | path / value |
|---|---|
| repo (commit often, push to origin main) | ~/macos-gpu-vm-lab |
| VM dir | ~/vm/m1 (boot script boot-tahoe-m5a.sh, overlay disk m5a.qcow2 -> backing mac_hdd.qcow2) |
| QEMU 10.2.1 source/build | ~/vm/qemu-10.2.1 (symlink /tmp/qemu-10.2.1 is needed by the build tree) |
| stub device source | m5/applegfx-trace.c (copied into the QEMU tree by m5/build.sh) |
| experiment cycle | m5/tools/cycle.sh (build, graceful shutdown, boot, wait SSH, decode). Prints `=== CYCLE DONE ===` or `ABORT: ...`; wait for that marker, not for the process to exit (it was seen to linger) |
| monitor / screenshots | m5/tools/qmon.py ("screendump f.ppm", "sendkey ret", ...) via /tmp/m1-mon.sock |
| trace log | ~/vm/m1/applegfx-trace.log (previous run: applegfx-trace.prev.log) |
| guest SSH | sshpass -p vm123456 ssh -p 2223 long@127.0.0.1 (sudo needs the same password) |
| driver binary RE | ~/vm/m5-re (SystemKernelExtensions.kc copied from guest; python3 m5/re/carve.py; objdump -D) |
| reference | ~/vm/qemu-10.2.1/hw/display/apple-gfx*.m (what the macOS-host device exposes), NullMoth repo (translator/, kexts/NVRM/agdc) |

## Guardrails (hard)
1. Never run sudo / change host settings. If something needs root, stop and write it in the report for the user.
2. Disk: keep >= 12 GB free on `/` (cycle.sh aborts below that). Never delete anything outside ~/vm/m5-re, logs you created, or build outputs. Never touch mac_hdd.qcow2 directly (the overlay protects it); if the overlay breaks, recreate it with qemu-img.
3. Only stop the guest with a graceful `shutdown -h now` (cycle.sh does it). Never `pkill qemu` / `pgrep -x qemu-system-x86` (that also matches the user's Android emulator). Our process is the one whose command line contains `qemu-system-x86_64-m5a`.
4. Never commit Apple binaries, kernel collections, carved code, disassembly, disk images (.gitignore enforces this). Commit only your own code, scripts, traces (small), and notes.
5. Keep upstream notices (NOTICE file) if you copy code from NullMoth (PolyForm NC / LGPL translator) or QEMU (GPL-2.0).
6. One variable per experiment. Record every result, including failures, in docs/ (append-only log docs/M5-LOG.md: date, hypothesis, change, outcome).
7. Stop and report instead of retrying the same failing thing more than 3 times.

## Phases (each has a pass check; do them in order)

### P1. Learn the reply format and get the driver past init
- Find the reply parser in the driver: setupInterrupts handler, the code that reads the second FIFO (reg +0x1c), and
  processSegmentKernelCommand. Regenerate disassembly: `cd ~/vm/m5-re && python3 ~/macos-gpu-vm-lab/m5/re/carve.py && objdump -D -b binary -mi386:x86-64 -Mintel --adjust-vma=$(cat pvg_text.addr) pvg_text.bin > pvg.asm`.
  Useful anchors: strings `written: %d read: %d`, `fifoOffset % 4`, `length < fifoCmdLength`, `cmdOffset < header->length`.
- In the stub: on each doorbell, advance the read pointer (+0xc) to the write pointer, write a reply into fifoB, raise MSI (`msi_notify`), log whether MSI is enabled (`msi_enabled`).
- PASS: driver sends a message with seq > 7, or touches a register not seen before.

### P2. Make the driver finish initialisation
- Keep answering each message; build a table opcode -> reply; log unknown opcodes with full payload.
- PASS: in the guest, `ioreg -l | grep -i AppleParavirt` shows the accelerator/framebuffer nodes, and
  `system_profiler SPDisplaysDataType` lists a second GPU (even with Metal "unsupported" at first).
- If WindowServer panics or the desktop goes black: that is a result, record it, revert the change.

### P3. Display and memory services (mirror QEMU apple-gfx contract)
- Implement what the macOS-host device offers the framework: tasks (create/destroy), mapMemory/unmapMemory of guest
  physical ranges, readMemory, display modes, new-frame events, cursor. Map each to the messages you see.
- PASS: guest shows a display served by the paravirtual device (can stay 2D) without breaking the existing desktop.

### P4. Metal command stream capture
- With P2 done, run a trivial Metal app in the guest (swift/clang one-file: create device, command queue, a compute
  kernel or a clear-color render pass). Capture the command buffers the guest sends (opcodes and payloads, shader blobs = AIR).
- PASS: a documented table of opcodes for device creation, buffer/texture allocation, pipeline creation, dispatch, present.

### P5. Host executor (Linux, Vulkan)
- Start with the simplest end-to-end: a compute dispatch (buffer in, kernel, buffer out) executed on the host GPU via
  Vulkan, results written into guest memory. Shader path: AIR -> SPIR-V (NullMoth translator is LGPL, usable as a library).
- Then clear-color render + present, then triangle, then WindowServer composition.
- PASS (per step): the guest-side test prints correct results / shows the correct picture.

## Stop conditions
- Finished: Metal app renders correctly through the host GPU and `system_profiler` reports Metal supported.
- Blocked: P1 or P2 shows no new behaviour after 6 distinct experiments -> write docs/BLOCKED.md with evidence and stop.
- Do not start P5 before P2 passes. Do not attempt rewriting the guest kext (Apple's driver is the point).

## Report format (end of each session, append to docs/M5-LOG.md and commit)
what changed, what the trace shows, what is now known/unknown, next experiment.
