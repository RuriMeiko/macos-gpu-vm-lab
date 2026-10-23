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

### P1. Learn the reply format and get the driver past init — DONE (M5e..M6v, 2026-10-23)
- Reply path decoded: the driver's reader (AppleParavirtVirtualChannel read, 0x148a9e56) is PULL-based: it
  copies `iov.len*4` RAW bytes from the fifoA ring (whole 64KB page, segdesc+0x10 = fifoA PFN) at the consumer
  cursor, spinning on the producer cursor (segdesc+4, device-owned). Reply cursor register = BAR0+0x100c
  (device->driver); doorbell register +0x1008 = driver->driver cursor. Reply content consumed silently; the
  replies stay in the ring (no clearing).
- Reg semantics (final for now): +0x1004 len 0x10000; +0x1008 doorbell (end-offset of driver cmds, cmd area =
  fifoA+0x1000); +0x100c reply cursor (device-owned); +0x1010 0x1000 (cmd area base); +0x101c fifoB PFN (root
  shared block, +0x400 = array of 5 channel segdescs, 20B each {.., idx@+0xc, ptab-pfn@+0x10}); +0x1030 fifoA
  PFN; +0x102c FAULT flag (bit0 = fault -> handleFaultInterrupt PANICS, never set); +0x1014/+0x1018 masks.
- op->reply-length map: op 0x38 {1,0,4,pfn} -> 16B; op 0x33 {0,1,0x100000} -> 4B; op 0x30 {n} -> n*4B;
  op 0x3a {45,0x200,pfn} -> 512B (reply len = payload[1]) — pfn (payload[2]) = device-time page the device
  must fill with clock data (qword0 = virtual ns; [ch+0x270]/[ch+0x274] = timebase used at 0x1489d7d9).
- OUTCOME: kext AppleParavirtGPU 15.0.0 LOADS, AppleParavirtGPUControl registered, AppleParavirtFramebuffer
  matched, 4 VirtualChannels + RootChannel + PageTable + ResourceHeap + Task + EventMachine alive, Metal plugin
  configured (AppleParavirtGPUMetal / AppleParavirtDevice). GPUControl stays busy=1 spinning; Accelerator
  !registered — the handshake reply CONTENT (not length) is still wrong/unknown.

### P2. Make the driver finish initialisation (current phase)
- GPUControl busy=1 accumulates: it is polling/spinning on something the device must provide. Candidate:
  reply CONTENT of the setup msgs (op 0x38 handshake reply 16B and op 0x3a time struct) or an event the
  device must keep asserting (masks +0x14/+0x18 read back once; fault +0x102c stays 0).
- SegmentedCommand (u16 op @+0, size @+4) = driver-side command struct (0x1489fc5e); the fifoA wire msg is
  built elsewhere — find the builder to learn expected reply fields.
- PASS: AppleParavirtAccelerator becomes `registered` in ioreg, GPUControl busy settles to 0.
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
