# BLOCKED (2026-10-24) — P3.5: IOFramebuffer::open NULL-deref panic

## Hiện tượng
- Boot (mọi binary gần đây: M7bq và M7e2): WindowServer mở framebuffer user
  client → IOFramebuffer::newUserClient → IOFramebuffer::open → page fault
  CR2=0x0 → panic → 'Waiting for remote debugger connection.'
- Panic deterministic trên các boot hiện tại; những boot P2-ok trước đó
  (12:34) đạt tới user clients (IOAccelDevice2...) KHÔNG panic — trạng thái
  NVRAM/guest nào đó đã thay đổi sau chuỗi reset/recovery.

## Backtrace (panic log serial-m5a.log)
  IOFramebuffer::open (IOGraphicsFamily 600.0)
  IOFramebuffer::newUserClient
  IOService::newUserClient
  is_io_service_open_extended (WindowServer)

## Nguyên nhân giả thuyết
1. AppleParavirtFramebuffer nub thiếu property/method mà IOFramebuffer::open
   cần (connection object / IONDRV null) → deref NULL.
2. Race: 2 framebuffer (IOBootNDRV virtio boot fb + AppleParavirtFramebuffer)
   — WindowServer mở node pvg khi display pipe chưa 'connected'.
3. Reply op33 (device-alloc 1MB) sai cshape làm map hỏng — đã revert M7e2,
   panic vẫn còn → KHÔNG phải (3).

## Đề xuất phiên sau
- Decode IOFramebuffer::open trong KC (IOGraphicsFamily trong KC fileset
  list — carve segment, tìm 0x...3ee1 offset trong runtime = có thể tính
  từ KC base + file off).
- Hoặc: cho AppleParavirtFramebuffer KHÔNG publish user client (đổi
  personality) để WindowServer dùng virtio fb — Metal vẫn chạy độc lập
  framebuffer hiển thị.
- Hoặc: cập nhật stub để Device Machine op (op38 chan4/5) trả display-pipe
  state đúng.

## Recovery đã dùng
- QEMU monitor system_reset + OpenCore picker keys (/tmp/m6picker2.py).
- Khi guest stuck shutdown (GTrace loop): monitor 'quit' (user-approved).
