#!/usr/bin/env python3
"""Decode fifoA command messages from the stub's trace log: [opcode][total len bytes][seq][payload...] (LE dwords).
Usage: decode_fifo.py [trace.log]   (uses the last fifoA.data dump in the file)"""
import re, struct, sys
path = sys.argv[1] if len(sys.argv) > 1 else '/home/rurimeiko/vm/m1/applegfx-trace.log'
dumps, cur = [], None
for line in open(path, errors='replace'):
    m = re.match(r'\s+d(\d+) (fifo[AB])\.data gpa=(0x[0-9a-f]+) len=(0x[0-9a-f]+) (\w+)', line)
    if m:
        cur = [m.group(2), int(m.group(1)), bytearray()]; dumps.append(cur); continue
    m = re.match(r'\s+[0-9a-f]{3}:((?: [0-9a-f]{2}){16})', line)
    if m and cur is not None: cur[2] += bytes.fromhex(m.group(1))
    elif line.strip() and not line.startswith('    '): cur = cur if line.startswith('  d') else None
a = [d for d in dumps if d[0] == 'fifoA']
if not a: sys.exit('no fifoA.data dump in ' + path)
n, buf = a[-1][1], bytes(a[-1][2])
print(f'fifoA data at doorbell #{n}, {len(buf)} bytes')
off = 0
while off + 12 <= len(buf):
    op, ln, seq = struct.unpack_from('<III', buf, off)
    if ln < 12 or ln % 4 or off + ln > len(buf): print(f'  stop at +0x{off:x} (op=0x{op:x} len={ln})'); break
    pay = struct.unpack_from('<%dI' % ((ln - 12) // 4), buf, off + 12)
    print(f'  +0x{off:03x} op=0x{op:02x} len={ln:3d} seq={seq:3d} payload=' + ' '.join(f'0x{x:x}' for x in pay))
    off += ln
