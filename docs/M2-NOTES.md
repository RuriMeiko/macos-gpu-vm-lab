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

## M2 COMPLETE (2026-10-22 17:10 +07): custom kext matched virtio-gpu, đọc config/features

- Kext `VirtIOGPUProbe` (com.mylab.VirtIOGPUProbe v0.0.1) build trong guest bằng clang 21 (CLT 26.6):
  `clang++ -mkernel -DKERNEL -DKEXT_COMPILE ...` + `ld -kext` → Mach-O kext bundle x86_64.
- Headers: clone Kernel.framework Headers về ~/m2-kext/khdr, XOÁ PCIDriverKit/ để
  __has_include(<PCIDriverKit/IOPCIDevice.h>)=0 (nếu không, class IOPCIDevice kext bị
  branch ra DriverKit variant → lỗi OSClassLoadInformation). KHÔNG dùng -nostdinc? —
  DÙNG -nostdinc -nostdinc++ -Ikhdr -I<sdk>/usr/include (chặn SDK framework paths).
- kmod_info_t kmod_info {next=NULL, ver=1, name, version, ...} + dummy start/stop
  (không có → "NULL module start pointer" 0xdc008007).
- Info.plist: IOPCIPrimaryMatch = **0x10501af4&0xffffffff** (device<<16|vendor! — nhầm
  0x00001050 ban đầu thì match không hit).
- Load flow: kextload → "requires a reboot" → approve trong Privacy&Security (Allow +
  admin password) → reboot → kextload lại (kext aux staged không tự load).
- KẾT QUẢ: kext matched (IOProbeScore=0, active), probe() hit, PCI vendor caps parsed:
  common cfg BAR4 off 0 len 4096; notify BAR4; ISR BAR4 4096; device cfg BAR4 off 8192;
  deviceFeatures=0x0000_0001_0000ffff (VIRTIO_F_VERSION_1 + notify...).
- TODO M3+: mapDeviceMemoryWithRegister BAR index dùng kIOPCIConfigBaseAddress0 + bar*4
  — deviceID đọc ra 0x1 (magic 0x1) → đang map nhầm BAR0 (legacy I/O) thay vì BAR4.
  Dùng pci->getDeviceMemoryWithIndex(bar) hoặc map theo BAR value decode.
- sysctl/log: kext printf qua serial.log (grep "VirtIOGPUProbe:").
