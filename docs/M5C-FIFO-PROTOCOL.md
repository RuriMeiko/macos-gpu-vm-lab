# M5c: the command FIFO of AppleParavirtGPU (decoded 2026-10-23)

Method: carved `com.apple.driver.AppleParavirtGPU` out of the guest's SystemKernelExtensions.kc
(m5/re/carve.py; the binary itself is NOT committed), read its strings and disassembled `setupFIFO`, then confirmed
with the stub device by dumping guest RAM at every doorbell (m5/applegfx-trace.c). Strings in the driver:
`setupMMIO`, `setupFIFO`, `writeFifo`, `length < fifoCmdLength`, `(fifoOffset % 4) == 0`, `written: %d read: %d`.

## Register block (BAR0 + 0x1000), all 32-bit
| off  | written by driver | meaning (from setupFIFO disassembly) |
|------|-------------------|--------------------------------------|
| +0x00 | 1 | enable (written last) |
| +0x04 | 0x10000 | FIFO buffer length (64 KB) |
| +0x08 | 0x1c,0x34,... | **write pointer** ("written"): byte offset into the command area, advanced after each message |
| +0x0c | (read) | **read pointer** ("read"): the device is expected to advance it |
| +0x10 | 0x1000 | offset of the command area inside the buffer (page 0 = header, zeroed by the driver) |
| +0x1c | PFN | physical page of a second buffer (fifoB), same layout idea |
| +0x30 | PFN | physical page (addr >> 12) of the main command buffer (fifoA) |
| +0x34 | 6 | read back by the driver; unknown (maybe interrupt vector count) |

Earlier probes failed only because they dumped the header page; commands are at (PFN<<12) + 0x1000.

## Message format seen in fifoA (little-endian dwords)
`[opcode][total length in bytes][sequence number][payload...]`, 4-byte aligned. The write pointer moves by one
message per doorbell. First seven messages the driver sends after enable:

| # | doorbell | opcode | len | seq | payload |
|---|----------|--------|-----|-----|---------|
| 1 | 0x1c | 0x38 | 28 | 1 | 1, 0, 4, 0x001f2ee3 |
| 2 | 0x34 | 0x33 | 24 | 2 | 0, 1, 0x00100000 |
| 3 | 0x44 | 0x30 | 16 | 3 | 1 |
| 4 | 0x54 | 0x30 | 16 | 4 | 2 |
| 5 | 0x64 | 0x30 | 16 | 5 | 3 |
| 6 | 0x74 | 0x30 | 16 | 6 | 4 |
| 7 | 0x8c | 0x3a | 24 | 7 | 0x2d, 0x200, 0xbe47 (looks like id, size, page number) |

Raw bytes: docs/M5c-fifo-trace.txt (doorbell #7 dump).

## Not known yet (hypotheses)
- Opcode names. 0x38 first with a 4-field payload looks like a hello/version handshake; 0x30 x4 with 1..4 looks like
  "create/open object n" (queues or contexts).
- Where and in what format the device must answer. fifoB (+0x1c) is probably the reply/event FIFO, and the driver
  then waits on an MSI. After message 7 the driver goes silent: it is waiting for a reply.
- The 4 bytes at message 1 payload[3] (0x1f2ee3) may be a magic/version.

## Next
Find the driver's reply parser (interrupt handler / processSegmentKernelCommand) in the disassembly to learn the
reply header, then make the stub answer message 1 and watch which message comes next.
