#!/usr/bin/env python3
"""Tiny mip sizes (1x1 ... 7x7, 1xN, Nx1): decode with the shipped decoder under ASan and compare with the
top-left crop of the same blocks decoded as a full 4-aligned texture. Catches out-of-bounds writes/reads and
wrong edge handling in the last mips of every compressed texture (GTA IV/V have 1x1 mips)."""
import os, random, subprocess, sys
CLI = sys.argv[1]; W = sys.argv[2]; os.makedirs(W, exist_ok=True)
FMT = {'BC1': (1,0,8), 'BC1x': (1,1,8), 'BC2': (2,0,16), 'BC3': (3,0,16), 'BC4u': (4,1,8), 'BC4s': (4,0,8),
       'BC5u': (5,1,16), 'BC5s': (5,0,16), 'BC6Hu': (6,1,16), 'BC6Hs': (6,0,16), 'BC7': (7,0,16)}
TEX = {1:4, 2:4, 3:4, 4:1, 5:2, 6:8, 7:4}
rng = random.Random(7); fails = 0; n = 0
def block(name, size):
    while True:
        b = bytearray(rng.getrandbits(8) for _ in range(size))
        if name == 'BC7' and b[0] == 0: continue
        if name.startswith('BC6H') and (b[0]&3) >= 2 and (b[0]&0x1f) in (0x13,0x17,0x1b,0x1f): continue
        return bytes(b)
def run(bcN, na, w, h, data, tag):
    src = os.path.join(W, tag+'.bin'); dst = src+'.out'; open(src,'wb').write(data)
    r = subprocess.run([CLI, str(bcN), str(na), str(w), str(h), '0', src, dst], capture_output=True, text=True)
    return r, (open(dst,'rb').read() if r.returncode == 0 else None)
sizes = [(w,h) for w in (1,2,3,4,5,7) for h in (1,2,3,4,5,7)]
for name,(bcN,na,bs) in FMT.items():
    for (w,h) in sizes:
        bw,bh = (w+3)//4,(h+3)//4
        data = b''.join(block(name,bs) for _ in range(bw*bh)); n += 1
        r, small = run(bcN,na,w,h,data,f'{name}_{w}x{h}')
        if r.returncode != 0:
            print(f'FAIL {name} {w}x{h}: exit {r.returncode} {r.stderr.splitlines()[0] if r.stderr else ""}'); fails += 1; continue
        r2, full = run(bcN,na,bw*4,bh*4,data,f'{name}_{w}x{h}_full')
        t = TEX[bcN]; exp = b''.join(full[(y*bw*4)*t:(y*bw*4+w)*t] for y in range(h))
        if small != exp: print(f'FAIL {name} {w}x{h}: texels differ from the cropped full decode'); fails += 1
print(f'{n-fails}/{n} small-size cases OK'); sys.exit(1 if fails else 0)
