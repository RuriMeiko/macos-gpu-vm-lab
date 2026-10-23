#!/usr/bin/env bash
# M1: macOS Tahoe in QEMU/KVM, basic display (no acceleration). Needs no root.
# EFI is served as a virtual FAT folder (fat:rw:) so the OpenCore config can be edited without mounting images.
cd "$(dirname "$0")"
OSX=./OSX-KVM
exec /home/rurimeiko/vm/m1/qemu-system-x86_64-m5a -L /usr/share/qemu -L /usr/share/seabios -enable-kvm -machine q35,kernel-irqchip=split -m 8192 \
  -cpu Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,-tsc-deadline,vmware-cpuid-freq=on,+ssse3,+sse4.2,+popcnt,+avx,+avx2,+aes,+xsave,+xsaveopt,check \
  -smp 4,cores=4,threads=1,sockets=1 \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 \
  -device isa-applesmc,osk="ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc" \
  -drive if=pflash,format=raw,readonly=on,file=$OSX/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=ovmf_vars-m5a.fd \
  -smbios type=2 \
  -device ich9-intel-hda -device hda-duplex \
  -device ich9-ahci,id=sata \
  -drive id=OpenCoreBoot,if=none,format=raw,file=OpenCore.img \
  -device ide-hd,bus=sata.2,drive=OpenCoreBoot \
  -drive id=InstallMedia,if=none,file=tahoe/BaseSystem.img,format=raw \
  -device ide-hd,bus=sata.3,drive=InstallMedia \
  -drive id=MacHDD,if=none,file=m5a.qcow2,format=qcow2 \
  -device nvme,drive=MacHDD,serial=MACDISK1 \
  -netdev user,id=net0,hostfwd=tcp::2223-:22 -device virtio-net-pci,netdev=net0,id=net0,mac=52:54:00:c9:18:27 \
  -serial file:serial-m5a.log -monitor unix:/tmp/m1-mon.sock,server,nowait -display none -vnc 127.0.0.1:5 -device virtio-vga,edid=on -device applegfx-trace,log=/home/rurimeiko/vm/m1/applegfx-trace.log
