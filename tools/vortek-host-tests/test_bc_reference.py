#!/usr/bin/env python3
"""Compare Vortek's BC decoder (bc_decoder.h, the code shipped in the APK) with an independent
decoder (Pillow's BCn decoder) on random blocks, for BC1-BC7, odd sizes and multi-threaded decodes.

usage: test_bc_reference.py <bc_decode_cli> <work_dir>
Exit code 0 only if every case is within tolerance.
"""
import os, random, struct, subprocess, sys
from array import array
from PIL import Image

CLI, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)

# name: (bcN, isNoAlphaU, dxgi, block bytes, tolerance)
# BC1-BC5 interpolation rounding is implementation-defined (the Vulkan spec allows it), BC7 is exact.
FORMATS = {
    'BC1_RGBA_UNORM': (1, 0, 71, 8, 4),
    'BC2_UNORM':      (2, 0, 74, 16, 4),
    'BC3_UNORM':      (3, 0, 77, 16, 4),
    'BC4_UNORM':      (4, 1, 80, 8, 2),
    'BC5_UNORM':      (5, 1, 83, 16, 2),
    'BC5_SNORM':      (5, 0, 84, 16, 3),
    'BC6H_UFLOAT':    (6, 1, 95, 16, 1),
    'BC6H_SFLOAT':    (6, 0, 96, 16, 1),
    'BC7_UNORM':      (7, 0, 98, 16, 0),
}
SIZES = [(4, 4, 0), (64, 64, 0), (37, 53, 0), (256, 300, 4), (130, 129, 8), (1024, 16, 4)]
BC6H_RESERVED = {0b10011, 0b10111, 0b11011, 0b11111}
# Pillow does not sign-extend delta-decoded (transformed) endpoints of SIGNED BC6H blocks, which
# the BC6H specification requires (bcdec, used by Vortek, does). Signed BC6H is therefore compared
# only on the two non-transformed modes (5-bit mode values 0x03 and 0x1E), where Pillow is correct.
BC6H_UNTRANSFORMED = {0x03, 0x1E}

def random_block(name, size, rng):
    while True:
        b = bytearray(rng.getrandbits(8) for _ in range(size))
        if name.startswith('BC7') and b[0] == 0:
            continue  # mode byte 0 is reserved (decoders legitimately differ)
        if name.startswith('BC6H') and (b[0] & 0b11) >= 2 and (b[0] & 0x1f) in BC6H_RESERVED:
            continue
        if name == 'BC6H_SFLOAT' and (b[0] & 0x1f) not in BC6H_UNTRANSFORMED:
            continue
        return bytes(b)

def write_dds(path, width, height, dxgi, data):
    header = struct.pack('<4sI6I44x', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, height, width, len(data), 0, 1)
    pixel_format = struct.pack('<II4s5I', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
    caps = struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack('<5I', dxgi, 3, 0, 1, 0)
    with open(path, 'wb') as f:
        f.write(header + pixel_format + caps + dx10 + data)

def half_to_float(h):
    return struct.unpack('<e', struct.pack('<H', h))[0]

def ours_to_rgba(name, raw, width, height):
    """Convert Vortek's decoded texels to the channel layout Pillow returns."""
    n = width * height
    if name.startswith(('BC1', 'BC2', 'BC3')):  # B8G8R8A8
        return [(raw[i*4+2], raw[i*4+1], raw[i*4+0], raw[i*4+3]) for i in range(n)], 'RGBA'
    if name == 'BC4_UNORM':
        return [(raw[i],) for i in range(n)], 'L'
    if name == 'BC5_UNORM':
        return [(raw[i*2], raw[i*2+1], 0) for i in range(n)], 'RGB'
    if name == 'BC5_SNORM':  # signed bytes; Pillow maps snorm to unorm as (s + 128)
        sb = lambda v: v - 256 if v > 127 else v
        return [(sb(raw[i*2]) + 128, sb(raw[i*2+1]) + 128, 128) for i in range(n)], 'RGB'
    if name.startswith('BC6H'):  # R16G16B16A16_SFLOAT
        halfs = array('H'); halfs.frombytes(raw)
        out, alpha_ok = [], True
        for i in range(n):
            px = []
            for c in range(3):
                v = half_to_float(halfs[i*4+c])
                px.append(int(max(0.0, min(1.0, v)) * 255 + 0.5))
            alpha_ok &= halfs[i*4+3] == 0x3C00
            out.append(tuple(px))
        if not alpha_ok:
            raise AssertionError('BC6H alpha is not 1.0')
        return out, 'RGB'
    if name == 'BC7_UNORM':  # R8G8B8A8
        return [tuple(raw[i*4:i*4+4]) for i in range(n)], 'RGBA'
    raise ValueError(name)

def main():
    rng = random.Random(0x1A2B3C)
    failures, report = 0, []
    for name, (bcN, noAlpha, dxgi, bsize, tol) in FORMATS.items():
        for (w, h, threads) in SIZES:
            blocks = ((w + 3) // 4) * ((h + 3) // 4)
            data = b''.join(random_block(name, bsize, rng) for _ in range(blocks))
            src = os.path.join(WORK, f'{name}_{w}x{h}.bin'); dst = src + '.out'; dds = src + '.dds'
            open(src, 'wb').write(data)
            write_dds(dds, w, h, dxgi, data)
            subprocess.run([CLI, str(bcN), str(noAlpha), str(w), str(h), str(threads), src, dst], check=True)
            ours, mode = ours_to_rgba(name, open(dst, 'rb').read(), w, h)

            im = Image.open(dds); im.load()
            flat = lambda img: list(img.get_flattened_data()) if hasattr(img, 'get_flattened_data') else list(img.getdata())
            ref = flat(im.convert(mode)) if im.mode != mode else flat(im)
            if mode == 'L':
                ref = [(v,) if isinstance(v, int) else v for v in ref]
            if name.startswith('BC6H'):
                ref = [tuple(p[:3]) for p in ref]
            max_diff = 0
            for a, b in zip(ours, ref):
                for x, y in zip(a, b):
                    max_diff = max(max_diff, abs(int(x) - int(y)))
            ok = max_diff <= tol and len(ours) == len(ref)
            failures += not ok
            report.append(f"{'OK  ' if ok else 'FAIL'} {name:15s} {w:4d}x{h:<4d} threads={threads} max|diff|={max_diff} (tolerance {tol})")
    print('\n'.join(report))
    print(f"\n{len(report) - failures}/{len(report)} cases within tolerance")
    sys.exit(1 if failures else 0)

if __name__ == '__main__':
    main()
