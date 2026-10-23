# Prompt for Hermes (copy everything inside the fence)

```
Bạn đang tiếp tục dự án nghiên cứu "macOS Tahoe dùng Metal qua GPU ảo trên Linux/KVM" của tôi. Làm việc tự chủ theo
vòng lặp cho đến khi XONG hoặc BỊ CHẶN thật sự. Trả lời bằng tiếng Việt, ngắn gọn.

BƯỚC ĐẦU (bắt buộc, trước khi làm gì khác):
1. Đọc ~/macos-gpu-vm-lab/docs/HANDOFF-PLAN.md (kế hoạch, môi trường, guardrails, các phase P1..P5, điều kiện dừng).
2. Đọc docs/M5C-FIFO-PROTOCOL.md và docs/M5-PVG-FINDINGS.md (những gì đã giải mã) và 20 dòng cuối docs/M5-LOG.md.
3. `git -C ~/macos-gpu-vm-lab pull --ff-only` rồi `git log --oneline | head`.

VÒNG LẶP (lặp lại, không hỏi tôi chọn hướng):
 a. Chọn thí nghiệm tiếp theo nhỏ nhất, đổi ĐÚNG MỘT biến, nêu giả thuyết trong 1 dòng.
 b. Sửa m5/applegfx-trace.c (hoặc công cụ), chạy `m5/tools/cycle.sh` (build -> tắt guest êm -> boot -> thu trace).
 c. Đọc kết quả (`m5/tools/decode_fifo.py`, trace log, ảnh chụp màn hình qua m5/tools/qmon.py nếu cần).
 d. Ghi 1 dòng vào docs/M5-LOG.md (giả thuyết, thay đổi, kết quả), commit và push (`git push origin main`).
 e. Đi tiếp phase hiện tại. Khi đạt PASS của phase thì sang phase kế, cập nhật HANDOFF-PLAN.md.
 Muốn biết driver Apple mong đợi gì thì ĐỌC mã của nó (m5/re/carve.py + objdump) trước khi đoán thêm.

GUARDRAILS (vi phạm là dừng ngay):
 - KHÔNG dùng sudo, KHÔNG đổi cấu hình máy host. Nếu cần quyền root: dừng và ghi rõ lệnh cần chạy vào báo cáo.
 - Giữ >= 12GB trống ở `/`. Không xóa file ngoài ~/vm/m5-re, log do bạn tạo, và output build.
 - Không động vào ~/vm/m1/mac_hdd.qcow2 (chỉ dùng overlay m5a.qcow2). Chỉ tắt guest bằng shutdown êm (cycle.sh làm sẵn).
   TUYỆT ĐỐI không `pkill qemu` hay `pgrep -x qemu-system-x86` (sẽ giết cả Android emulator của tôi).
   Tiến trình của dự án chỉ là tiến trình có `qemu-system-x86_64-m5a` trong dòng lệnh.
 - Không commit binary của Apple (kernel collection, code đã tách, disassembly, ảnh đĩa). .gitignore đã chặn, đừng ép add.
 - Giữ nguyên thông báo bản quyền khi chép code từ NullMoth/QEMU (xem NOTICE).
 - Một thí nghiệm hỏng 3 lần liên tiếp theo cùng một cách thì đổi cách tiếp cận; 6 thí nghiệm khác nhau không ra hành vi mới
   ở P1 hoặc P2 thì viết docs/BLOCKED.md kèm bằng chứng rồi DỪNG.

KHI NÀO DỪNG VÀ BÁO CÁO CHO TÔI:
 - XONG: một app Metal trong guest render đúng qua GPU host và `system_profiler SPDisplaysDataType` báo Metal supported.
 - BỊ CHẶN: theo điều kiện trên, hoặc cần quyền root, hoặc ổ đĩa < 12GB, hoặc guest hỏng không tự phục hồi được.
 - Mỗi lần dừng: tóm tắt đã giải mã được gì, chưa biết gì, bước kế tiếp đề xuất. Không dừng chỉ để "chờ tôi chọn hướng".
```
