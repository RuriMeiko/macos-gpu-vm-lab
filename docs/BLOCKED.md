# BLOCKED — disk space (P1, 2026-10-23)

## Điều kiện kích hoạt
`/` còn **9.0 GB** (< 12 GB guardrail) và đang giảm dần (~0.5 GB/30 phút, chủ yếu
systemd journal + syslog tăng liên tục).

## Đã thử
- Xóa mọi log/ảnh dump do agent tạo trong /tmp (ppm/png, tarball) — không đủ.
- `journalctl --vacuum-size=100M` → **Permission denied** (cần root).
- Không thể `sudo` (guardrail), không thể đổi cấu hình host (guardrail).

## Cần người dùng chạy (root):
```bash
sudo journalctl --vacuum-size=200M      # giải phóng ~4 GB
sudo rm /var/log/syslog.1 /var/log/syslog.2.gz /var/log/syslog.3.gz  # ~2 GB
```
Sau đó đảm bảo `df -h /` hiển thị >= 12-15 GB trống rồi báo lại để tiếp tục.

## Trạng thái khi dừng
- Guest: khỏe, SSH up (port 2223), không wedge (reply=off).
- QEMU (m5a) vẫn chạy — guest idling, không có tác vụ dang dở trong guest.
- Binary M5j (per-message descriptor reply) ĐÃ BUILD xong ở
  `~/vm/qemu-10.2.1/build/qemu-system-x86_64` (13:57) nhưng CHƯA swap/chạy —
  cycle.sh ABORT ở bước check disk.
- Boot script: `~/vm/m1/boot-tahoe-m5a.sh` đang `reply=on` (chuẩn bị cho M5j).

## Tiến bộ ngay trước khi blocked (M5i/M5j — đã commit tới b3762ef)
- Root channel decoded: block +0x400 = mảng {seq, PFN trang descriptor mỗi msg}.
- Mỗi trang descriptor: +0 consumer(driver), +4 producer(device), +8 flushid,
  +0c id, +0x10 ring PFN — device ghi reply segment (28B: [0x10000][1][12][0]
  [id][ts64]) vào ring tại producer rồi advance producer.
- writeFifo 0x1489ab0c decoded: mirror+0x8 = producer (doorbell arg),
  mirror+0xc = consumer (driver poll), +0x1c = producer mirror.
- Stub M5j đã code: đọc slot[seq] → desc page → ring → ghi reply + MSI.
  Đang chờ chạy khi disk đủ.

## Bước kế tiếp (sau khi disk đủ)
1. Chạy `cycle.sh` (binary M5j đã build sẵn sẽ được swap).
2. Đọc log: `reply: msg seq=.. desc(pfn ..) cons=.. prod=.. id=.. ring_pfn=..`
   + `reply: posted seq=..` để xác nhận driver tiêu thụ reply (cons advances,
   driver gửi tiếp messages → P1 PASS).
3. Nếu driver consume: tăng dần reply scope (op 0x30/0x3a/0x33) theo log,
   hướng tới P2 (accelerator match, framebuffer).
