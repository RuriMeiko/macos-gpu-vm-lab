# BLOCKED-P3-2 — P3 (GPU stamps / submit) — trạng thái sau M7kv..M7m8 (2026-10-26)

## Tóm tắt
P1 (wire decode) ĐẠT, P2 (handshake — Accelerator registered) ĐẠT.
P3 DỪNG theo guardrail: >6 thí nghiệm khác nhau không tạo hành vi mới.
Guest P2-ok boot ổn định; CommandQueue user client tồn tại (WindowServer)
nhưng không có submit nào; driver checkGPUProgress chờ stamp 4
(gpu_stamp=0) → timeout → GPURestart → idle.

## Đã decode (mới, chắc chắn — symtab đầy đủ 2 kext)
- Symtab carve: pvg 1022 syms @symtab 0x158a0e60/stroff 0x159f97c0;
  IAF2 4707 syms @0x1590df80. __ZTV20IOAccelEventMachine2 @0x14bd6aa0
  (IAF2): +0x138 waitForStamp, +0x140 setStampBaseAddress (0x14b778be),
  +0x148 getStampOffset (0x14b778b0 = lea eax,[rsi*4]).
- Vtable thật AppleParavirtEventMachine = 0x148b1c48 (từ constructor C2
  @0x14899b36): +0x118 pvg init (0x14899b66), +0x130 setStampBaseAddress
  (0x14b778be), +0x138 getStampOffset. (0x148b1f58 KHÔNG phải vtable EM.)
- Accelerator init (0x1489aa13..): alloc IOBufferMemoryDescriptor 64KB
  (0x1489a96a: size 0x10000); [accel+0xe00]=map VA; [accel+0xe10]=stamp
  base VA (vtbl+0x118); bzero 4KB đầu; [em+0x30]=PFN (getPhysicalSegment
  >>12 — thunk 0x10246); accel+0xe18 = PA>>12 - 0x1000.
- waitForStamp (M7az): stamp ptr = [em+0x28][idx]; entry =
  getStampOffset(idx) + [em+0x20] → stamp dword = stampBase + idx*4.
- writeStamp (addSignal 0x1489fd9a): [cmdRec+0x400]+8 = stamp value —
  signal command ghi vào command buffer (không phải descriptor).
- op30 sender (0x148a9b9f): payload = 4 bytes [cmdq+0x110]; sau gửi set
  [cmdq+0x118]=1 (không chờ reply).
- canSubmitCommandBuffer = return 1 (không gate).
- fifoB root block @+0x400: 5 channel entries = PFN bảng trang command
  buffer (segdesc pages, 16 PFN/page) — KHÔNG phải stamp pages.

## Đã thử và THẤT (P3 candidates)
1. Bump +0x10 trên op38 pfn pages (M7jl/jm) — op38 pfn pages là
   signal-command pages có header driver {pfn+1, 3, ?, idx} — value
   +0x10 tăng 9252+ nhưng gpu_stamp vẫn 0 → driver không poll đây.
2. Bump op33 area 0x30000000 32 dwords (M7ju) — sai trang.
3. Bump PA 0x1010 (broken-map giả thiết descriptor PA 0x1000, M7kx) —
   sai (driver không poll 0x1000).
4. Scan 8GB cho vptr EM (0xffffff7f95597c48/58) = 0 HITS (M7ls/lt/lu).
5. Scan 8GB cho waitForStamp prologue bytes = 0 HITS (M7lz — runtime
   fixup đổi bytes code).
6. gdb hbreak waitForStamp entry — hit không ổn định (restart cycle
   timing); KPTI chặn SW-break + monitor x/gva2gpa khi idle.
7. op30 reply N dwords 0x40 (M7jv/jw/jx) — vẫn không có submit.
8. system_profiler (guest) 180s: KHÔNG MMIO mới — submit fail trước
   khi chạm doorbell (software-side).

## Kết luận
- Root cause giờ nằm ở tầng KHÔNG thấy được bằng stub log: nếu có
  submit, signal commands sẽ xuất hiện trong command buffer pages của
  channel (segdesc PFN lists — có thể dump). Không submit → không có
  gì viết stamps → heartbeat timeout → restart → idle.
- Để tìm EM/descriptor cần GDB hit ổn định (KVM debug trên tất cả
  vCPU — hiện chỉ attach được 1) hoặc bubble client submit (cần decode
  op30 reply format đúng / externalMethod dispatch của CommandQueue).

## Hướng tiếp (khi mở lại)
1. GDB: gdb -tui với 'set detach-on-fork off'? — KHÔNG. Cần: qemu
   gdbstub KVM debug set trên mọi vCPU (patch qemu: KVM_GUEST_DEBUG
   apply all cpus) → hbreak waitForStamp → read [em+0x20]/[em+0x30] →
   bump đúng descriptor phys.
2. Decode externalMethod dispatch của AppleParavirtCommandQueue user
   client để trả đúng reply làm WindowServer submit thật.
3. Signal-command replay: stub tự viết record hoàn thành vào command
   buffer page (records stride 0xc00 @fifoB+0x3000 boot này, format
   {0xf802e0d0,1,0xc,0x10003,ts}) + bump stamp.

## Bằng chứng nhanh
- applegfx-trace.log boot: tick bump pfn=0x200a2c/0x1dc682/0x1d19be →
  25640+ ticks, gpu_stamp vẫn 0 (kernel log buffer @0x2119a000 t=1970s
  stale sau đó — restart idle).
- 64KB page @0x200a2c000: +0x00=0x200a2d (pfn+1), +0x04=3, +0x10=9252
  (my bump), +0x18=4, +0x4000=ASCII (kext LINKEDIT data).
- /tmp/m5log-m7kv.txt .. /tmp/m5log-m7m7.txt; decode files
  /tmp/m7az-decode.md, /tmp/m7lf-decode.md, /tmp/m7li-decode.md,
  /tmp/m7lj-decode.md, /tmp/m7lq-decode.md, /tmp/m7lr-decode.md,
  /tmp/m7m0-decode.md, /tmp/m7m4-decode.md.
