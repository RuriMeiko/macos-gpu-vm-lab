# M7 — Tổng kết lab macOS GPU VM (2026-10-23)

## Trạng thái đạt được
- **M0-M3**: hoàn tất (Linux guest vkcube, Tahoe boot, kext probe virtio-gpu, fb pipeline, scanout verified).
- **M4' display**: **ĐẠT** — macOS Tahoe desktop đầy đủ (menubar/Dock/Safari/apps) chạy trên **virtio-vga** paravirtualized (KHÔNG passthrough GPU):
  - qemu v2 (class DISPLAY_VGA) + `-device virtio-vga,edid=on` + `-L /usr/share/seabios`
  - vgabios-virtio → OVMF GOP console trên virtio → IOBootNDRV tạo `.Display_boot` (IONDRVFramebuffer legacy) → WindowServer legacy path
  - main display = 1af4:1050, 1280x800, Online. Resolution fixed (EDID 1 mode).
  - Script: `boot-tahoe-vgpu3.sh` (1280x800) / `boot-tahoe-vgpu3-hd.sh` (xres/yres=1920x1080 — OS vẫn chọn 1280x800).
- Kext VirtIOGPUProbe v0.0.39: scanout kext→QEMU verified (v0.0.28); trên vgpu3 KHÔNG attach (IOBootNDRV chiếm IOFramebuffer category) — không ảnh hưởng desktop.

## Đường dead-end đã chốt (không thử lại)
1. IOPresentment/AGDC trên VM: "Unable to get AGDC information 0x10000003" — AGDC info chỉ GPU vật lý trả lời được. v0.0.35-39 (AGDC nub attach OK vẫn không giúp — wrangler agdc 0).
2. Fake IOAccelerator nubs (v0.0.31-34): GPUWrangler bỏ qua.
3. virtio-gpu-pci (không vgabios) bất kể class: không có GOP console trên device → không .Display_boot.
4. CoreDisplay VirtDisplay fallback: offline loop (CGXVFBSelectOnlineState) — cần display pipe.
5. AppleParavirtIOSurface / AppleM2ScalerParavirt: ARM-only.

## Giới hạn còn lại (M5)
- **Metal/3D**: KHÔNG hoạt động — cần IOAccelerator thực. macOS không có driver virgl/blob (AppleParavirtGPU protocol là protocol riêng của Apple, qemu không implement). qemu virgl-enabled build (mặc dù có sẵn virtio-vga-gl) không giúp macOS.

## Cách dùng
1. `~/vm/m1/boot-tahoe-vgpu3.sh` — boot desktop (OC picker: right×2 + Enter; tự boot sau timeout).
2. SSH: port 2223 (user long).
3. Approve kext (khi cần cài lại): System Settings → Login Items & Extensions → Allow (flow trong M4-NOTES.md).

## Files chính
- `vm/m1/qemu-system-x86_64-vgpu2` (class VGA patched — DÙNG CHO vgpu3)
- `vm/m1/qemu-system-x86_64-vgpu` (class OTHERS — chỉ dùng khi kext fb cần)
- `vm/m1/boot-tahoe-vgpu3.sh` — MAIN WORKING BOOT SCRIPT
- `m2-kext/src_v008.cpp` v0.0.39 (đã approve trong guest)
- `docs/M4-NOTES.md` — chi tiết kỹ thuật đầy đủ
