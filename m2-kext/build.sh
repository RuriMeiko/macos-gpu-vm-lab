#!/bin/bash
# Build kext trong guest (macOS) — chạy qua SSH.
set -e
cd "$(dirname "$0")"
clang++ -arch x86_64 \
  -mmacosx-version-min=26.0 \
  -mkernel -fno-builtin -fno-rtti -fno-exceptions -fasm-blocks \
  -nostdlib -nostdinc++ \
  -I"$(xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/Kernel.framework/Headers" \
  -DKEXT_COMPILE \
  -c src.cpp -o VirtIOGPUProbe.o
ld -arch x86_64 -static -preload -preload_all -no_png \
  -o VirtIOGPUProbe VirtIOGPUProbe.o \
  $(xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/Kernel.framework/Kernels/kernel_link \
  -lIOKit -lc++
mkdir -p Kexts/VirtIOGPUProbe.kext/Contents/MacOS
cp VirtIOGPUProbe Kexts/VirtIOGPUProbe.kext/Contents/MacOS/
codesign --force --sign - Kexts/VirtIOGPUProbe.kext
