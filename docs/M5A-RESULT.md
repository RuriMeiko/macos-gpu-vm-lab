# M5a result: stock AppleParavirtGPU binds to a QEMU stub and starts talking

Date 2026-10-23. Device: `applegfx-trace` (m5/applegfx-trace.c), PCI 106b:eeee, class 0x0380, BAR0 256 KB MMIO, MSI x1.
Guest: Tahoe 26.7.1 on virtio-vga desktop. The guest booted normally and SSH came up; the driver touched BAR0 immediately.

Raw trace: docs/M5a-first-trace.txt (24 accesses, then silence - the driver waits on a status register we answer with 0).

Sequence seen (all 32-bit):
```
R 0x122c               probe (returned 0)
W 0x1200 = 0
W 0x1034 = 6   ; R 0x1034 -> 6      register reads back what was written
W 0x101c = 0x11e4d                 (looks like a guest page-frame number: 0x11e4d000)
W 0x1030 = 0x12104                 (second PFN: 0x12104000)
W 0x1004 = 0x10000
W 0x1010 = 0x1000
W 0x1000 = 1                       enable
loop: R 0x100c (0)  ; W 0x1008 = 0x1c, 0x34, 0x44, 0x54, 0x64, 0x74, 0x8c   steps of 0x10-0x18
```
Reading (HYPOTHESIS, not verified): 0x101c/0x1030 are the guest addresses of two shared pages (ring / buffers),
0x1008 is a doorbell/producer offset advancing by one message per write (16-24 bytes each), 0x100c is a
consumer/status the host must advance. Next step (M5b): on each doorbell, dump the guest pages at those
addresses to see the message format.

## M5b: where are the messages? (2026-10-23, inconclusive)

Extended the stub to read guest memory on every doorbell (write to 0x1008) and probe each value the driver
programmed (0x1004, 0x101c, 0x1030) as an address under shifts of 12, 8, 4, 0 bits (m5/applegfx-trace.c).
- Guest DMA from the device works (pci_dma_read, 0 failures).
- Shift 12 (page numbers): all zeros. Shift 8: data present but only 0xFF fill (looks like an "empty slot" pattern).
- Compared every probed address across doorbells: **none changes**. So the messages are not at any of those
  addresses; the "two page numbers + ring" hypothesis above is NOT supported. Register meanings remain unknown.
- The values differ each boot (0x11e4d, 0x17c4a ...), consistent with guest-allocated memory addresses.

## What QEMU's own apple-gfx source tells us (hw/display/apple-gfx*.m, QEMU 10.2, macOS-only build)

The macOS-host device is a thin shim: PCI vendor/device from Apple's header, class DISPLAY_OTHER, one MMIO BAR,
MSI (64-bit); every guest MMIO access is forwarded to `-[PGDevice mmioReadAtOffset:]` / `mmioWriteAtOffset:value:`
(on a worker thread, because writes can block). The framework asks the host for only these services:
- `createTask(vmSize)` reserve a host address range; `destroyTask`
- `mapMemory(task, ranges[{guest phys addr, len}], virtual_offset)` / `unmapMemory`: guest RAM mapped into a task
- `readMemory(guest phys addr, len)`: framework reads guest RAM by physical address
- `raiseInterrupt(vector)` -> MSI
- a `PGDisplay`: modes, new-frame events, cursor glyph
All command decoding and GPU execution happens inside the closed framework using the host's real MTLDevice.
So on Linux the whole command interpreter is the missing part, not just the PCI shell.

## Next options
1. Make the stub "progress": answer 0x100c as consumer == last doorbell value and see which register the driver touches next (cheap loop, ~4 min per try).
2. Read the AppleParavirtGPU binary (inside the guest kernel collection) to get the register map statically.
3. Mirror the PG* callbacks in the stub (task/map/readMemory) once the registers are known.
