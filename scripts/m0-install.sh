#!/usr/bin/env bash
# M0 step 1: install QEMU + OVMF + virglrenderer (Venus capable). Packages only; no GPU/host config changes.
set -euo pipefail
sudo apt-get update
sudo apt-get install -y qemu-system-x86 qemu-utils ovmf libvirglrenderer1 mesa-vulkan-drivers vulkan-tools cloud-image-utils
qemu-system-x86_64 --version | head -1
qemu-system-x86_64 -device virtio-vga-gl,help 2>&1 | grep -E 'venus|blob|hostmem' || echo "WARN: no venus option reported"
