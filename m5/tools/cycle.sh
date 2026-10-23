#!/usr/bin/env bash
# One experiment cycle for the applegfx-trace stub:
#   build device -> graceful guest shutdown -> swap QEMU binary -> boot Tahoe on the overlay disk -> wait for SSH -> summary
# Uses ONLY the overlay disk m5a.qcow2 (backing: mac_hdd.qcow2). Never kills anything except our own qemu (m5a binary).
set -u
M1=${M1:-$HOME/vm/m1}; Q=${QEMU_SRC:-$HOME/vm/qemu-10.2.1}
HERE=$(cd "$(dirname "$0")/.." && pwd); TOOLS="$HERE/tools"
SSHG() { sshpass -p vm123456 ssh -o ConnectTimeout=6 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -p 2223 long@127.0.0.1 "$@"; }
OURS() { pgrep -f 'qemu-system-x86_64-m5a' >/dev/null; }

free_gb=$(df --output=avail -BG / | tail -1 | tr -dc 0-9); [ "$free_gb" -ge 12 ] || { echo "ABORT: only ${free_gb}G free on /"; exit 3; }

echo "[1/5] build"; "$HERE/build.sh" >/tmp/cycle-build.log 2>&1 || true   # build.sh's final cp may fail while the VM runs; ninja result checked below
cp "$HERE/applegfx-trace.c" "$Q/hw/misc/applegfx-trace.c"
ninja -C "$Q/build" qemu-system-x86_64 2>&1 | grep -E 'error|FAILED' && { echo "ABORT: build failed"; exit 1; }

echo "[2/5] graceful shutdown"
if OURS; then
  SSHG 'echo vm123456 | sudo -S shutdown -h now' >/dev/null 2>&1
  for i in $(seq 1 200); do OURS || break; sleep 3; done
  OURS && { echo "ABORT: guest did not power off in 600s (do NOT kill; inspect screenshot via tools/qmon.py)"; exit 2; }
fi

echo "[3/5] swap binary"; cp "$Q/build/qemu-system-x86_64" "$M1/qemu-system-x86_64-m5a" || exit 1
[ -f "$M1/applegfx-trace.log" ] && mv "$M1/applegfx-trace.log" "$M1/applegfx-trace.prev.log"
rm -f /tmp/m1-mon.sock

echo "[4/5] boot"; cd "$M1" && setsid nohup ./boot-tahoe-m5a.sh >qemu-m5a.log 2>&1 </dev/null &
sleep 1; cd "$HERE" || exit 1   # qemu is detached (own session), the script must not wait on it
sleep 45; OURS || { echo "ABORT: qemu failed to start"; tail -5 "$M1/qemu-m5a.log"; exit 1; }
python3 "$TOOLS/qmon.py" "sendkey right"; sleep 2; python3 "$TOOLS/qmon.py" "sendkey right"; sleep 2; python3 "$TOOLS/qmon.py" "sendkey ret"

echo "[5/5] wait for guest SSH"
for i in $(seq 1 40); do SSHG true 2>/dev/null && { echo "guest up after ~$((i*10))s"; break; }; sleep 10; done
SSHG true 2>/dev/null || { python3 "$TOOLS/qmon.py" "screendump /tmp/cycle-fail.ppm" >/dev/null 2>&1; echo "WARN: guest SSH not up in 400s (screenshot /tmp/cycle-fail.ppm). May be a hang/panic caused by the last change."; }
sleep 15
echo "--- trace: $(grep -cE '^[0-9]+ t=' "$M1/applegfx-trace.log") MMIO accesses; DMAFAIL=$(grep -c DMAFAIL "$M1/applegfx-trace.log")"
python3 "$TOOLS/decode_fifo.py" "$M1/applegfx-trace.log" 2>&1 | head -30
echo "=== CYCLE DONE ==="
exit 0
