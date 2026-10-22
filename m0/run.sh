#!/usr/bin/env bash
exec qemu-system-x86_64 -enable-kvm -machine q35 -cpu host -smp 4 -m 6G \
  -object memory-backend-memfd,id=mem1,size=6G -machine memory-backend=mem1 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=disk.qcow2,if=virtio -drive file=seed.img,if=virtio,format=raw \
  -device virtio-vga-gl,blob=on,venus=on,hostmem=4G \
  -display egl-headless -serial file:serial.log \
  -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22 \
  -daemonize -pidfile qemu.pid
