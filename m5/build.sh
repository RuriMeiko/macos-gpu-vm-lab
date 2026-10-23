#!/usr/bin/env bash
# Build QEMU with the applegfx-trace device. Expects an already-configured QEMU 10.2.1 tree.
set -euo pipefail
Q=${QEMU_SRC:-$HOME/vm/qemu-10.2.1}
# The configured build tree records /tmp/qemu-10.2.1 as an absolute path; keep a symlink for it.
[ -e /tmp/qemu-10.2.1 ] || ln -s "$Q" /tmp/qemu-10.2.1
HERE=$(cd "$(dirname "$0")" && pwd)
cp "$HERE/applegfx-trace.c" "$Q/hw/misc/applegfx-trace.c"
grep -q applegfx-trace "$Q/hw/misc/meson.build" || \
  echo "system_ss.add(when: 'CONFIG_PCI', if_true: files('applegfx-trace.c'))" >> "$Q/hw/misc/meson.build"
ninja -C "$Q/build" qemu-system-x86_64
OUT=${OUT:-$HOME/vm/m1/qemu-system-x86_64-m5a}
cp "$Q/build/qemu-system-x86_64" "$OUT"
"$OUT" -device applegfx-trace,help 2>&1 | head -6
echo "built: $OUT"
