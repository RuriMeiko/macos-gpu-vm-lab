#!/usr/bin/env python3
"""Decode fifoB (device->driver) header+dumps from the stub trace log."""
import re, struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else '/home/rurimeiko/vm/m1/applegfx-trace.log'
dumps, cur = [], None
for line in open(path, errors='replace'):
    m = re.match(r'\s+d(\d+) (fifo[AB])\.(data|header) gpa=(0x[0-9a-f]+) len=(0x[0-9a-f]+) (\w+)', line)
    if m:
        cur = [m.group(2), m.group(3), int(m.group(1)), bytearray()]
        dumps.append(cur)
        continue
    m = re.match(r'\s+([0-9a-f]{3}):((?: [0-9a-f]{2}){16})', line)
    if m and cur is not None:
        cur[3] += bytes.fromhex(m.group(2))
    elif line.strip() and not line.startswith('    '):
        cur = None

for tag, kind, n, buf in dumps:
    if tag != 'fifoB' or not buf:
        continue
    print(f'--- doorbell #{n} fifoB.{kind}')
    if kind == 'header':
        vals = struct.unpack_from('<16I', buf, 0)
        print('  u32 words:', ' '.join(f'{v:#x}' for v in vals))
    else:
        for off in range(0, min(0x100, len(buf)), 16):
            row = buf[off:off+16]
            if any(row):
                print(f'  +{off:03x}: ' + ' '.join(f'{b:02x}' for b in row))
