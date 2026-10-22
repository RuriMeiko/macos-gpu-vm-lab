# M2 NOTES — virtio-gpu enumeration (2026-10-22)

## Setup
- Host QEMU 10.2.1 (Ubuntu): KHÔNG có capset venus (không flag venus=), blob=on cần rutabaga/udmabuf -> lỗi.
  => virtio-gpu-pci với flag cơ bản: `-device virtio-gpu-pci,edid=on` (no blob, no venus).
- boot-tahoe.sh: thêm `-device virtio-gpu-pci,edid=on` cạnh vmware-svga.

## Kết quả trong guest (macOS 26.7.1 build 25G241, user long/vm123456, SSH host:2223)
- PCI device virtio-gpu: `pci1af4,1050`, class `038000`, IOName "display", slot S38 (0:7:0).
- **`AppleVirtIOPCITransport` (com.apple.driver.AppleVirtIO) đã MATCH và ATTACH** — macOS có sẵn
  cả họ kext AppleVirtIO (Balloon/Console/Entropy/FS/Input/Network/Sound/... + PCI transport).
- IONDRVFramebuffer đã attach lên device — virtio-gpu đang làm scanout thực sự (VNC hiển thị desktop).

## Ý nghĩa
- M2 (kext guest enumerate virtio-gpu + negotiate) phần lớn ĐÃ CÓ SẴN từ Apple:
  transport generic AppleVirtIO match mọi 1af4 — nghĩa là device đã được kernel claim.
- Câu hỏi M2 còn lại: transport có negotiate virtio-gpu protocol không, hay chỉ claim PCI?
  Kext Apple chỉ có transport chung (virtqueue), KHÔNG có AppleVirtIOGPU — cần viết driver riêng
  nói chuyện với transport hoặc tự quét PCI như transport làm.

## Tooling
- SSH vào guest: `sshpass -p vm123456 ssh -p 2223 long@127.0.0.1` (hostfwd 2223->22).
- Remote Login bật bằng `sudo launchctl load -w /System/Library/LaunchDaemons/ssh.plist`
  (systemsetup bị chặn Full Disk Access; launchctl im lặng = thành công).
- vncdo type: pipe '|' phải gõ bằng `keydown shift + key \ + keyup shift` — type() lỗi pipe.
