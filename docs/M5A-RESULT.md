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
