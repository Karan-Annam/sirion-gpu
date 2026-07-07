#!/usr/bin/env python3
"""ppm_tool.py — preview a PPM in the terminal (ANSI truecolor) and/or convert it.

    python scripts/ppm_tool.py build/render.ppm            # ANSI color preview in the terminal
    python scripts/ppm_tool.py build/render.ppm --png out.png   # write a PNG (opens anywhere)
    python scripts/ppm_tool.py build/render.ppm --p3  out.ppm   # write an ASCII (P3) PPM
    python scripts/ppm_tool.py build/render.ppm --ascii    # plain-ASCII luminance (no color)

The GPU renders to a binary (P6) PPM — that's a valid PPM, but some viewers only accept the
ASCII (P3) variant and report a valid P6 file as "not a PPM". Two dependency-free fixes:
  * --png : writes a real PNG (uses Pillow if present, else a built-in stdlib encoder) — PNG
            opens in every OS image viewer (Windows Photos, Preview, browsers, ...).
  * --p3  : rewrites the image as an ASCII PPM for the strict P3-only viewers.
Both need no third-party packages, so they work identically on MSYS2 and WSL.
"""
import argparse
import struct
import sys
import zlib

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] not in (b'P6', b'P3'):
        raise ValueError('not a PPM (P6/P3)')
    binary = data[:2] == b'P6'
    # parse header tokens (skip comments)
    idx = 2
    toks = []
    while len(toks) < 3:
        while idx < len(data) and data[idx:idx+1].isspace():
            idx += 1
        if data[idx:idx+1] == b'#':
            while idx < len(data) and data[idx:idx+1] != b'\n':
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx+1].isspace():
            idx += 1
        toks.append(int(data[start:idx]))
    w, h, maxv = toks
    idx += 1  # single whitespace after maxval
    px = []
    if binary:
        raw = data[idx:idx + w*h*3]
        px = list(raw)
    else:
        nums = data[idx:].split()
        px = [int(n) for n in nums[:w*h*3]]
    return w, h, px


def ansi_preview(w, h, px, cols=72):
    # downsample to cols wide; use the half-block trick (two pixel rows per text row).
    cols = min(cols, w)
    step = max(1, w // cols)
    ow = w // step
    def avg(bx, by):
        r = g = b = n = 0
        for yy in range(by*step, min((by+1)*step, h)):
            for xx in range(bx*step, min((bx+1)*step, w)):
                i = (yy*w+xx)*3; r += px[i]; g += px[i+1]; b += px[i+2]; n += 1
        n = max(1, n)
        return r//n, g//n, b//n
    oh = h // step
    out = []
    for by in range(0, oh, 2):
        line = []
        for bx in range(ow):
            tr, tg, tb = avg(bx, by)
            br, bg, bb = avg(bx, by+1) if by+1 < oh else (tr, tg, tb)
            line.append(f"\x1b[38;2;{tr};{tg};{tb}m\x1b[48;2;{br};{bg};{bb}m▀")
        out.append("".join(line) + "\x1b[0m")
    return "\n".join(out)


def ascii_preview(w, h, px, cols=80):
    ramp = " .:-=+*#%@"
    step = max(1, w // cols)
    lines = []
    for by in range(0, h, step*2):
        row = []
        for bx in range(0, w, step):
            i = (by*w+bx)*3
            lum = (px[i]*3 + px[i+1]*6 + px[i+2]) // 10
            row.append(ramp[min(len(ramp)-1, lum*len(ramp)//256)])
        lines.append("".join(row))
    return "\n".join(lines)


def _png_chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data
            + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))


def encode_png(w, h, px):
    """Encode RGB pixels to PNG bytes using only the stdlib (zlib). No Pillow needed."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0 (None) for this scanline
        raw.extend(px[(y*w)*3:(y*w + w)*3])
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)  # 8-bit, color type 2 (truecolor)
    return (b'\x89PNG\r\n\x1a\n'
            + _png_chunk(b'IHDR', ihdr)
            + _png_chunk(b'IDAT', zlib.compress(bytes(raw), 9))
            + _png_chunk(b'IEND', b''))


def write_png(w, h, px, path):
    """Prefer Pillow (broad format support); fall back to the built-in encoder."""
    try:
        from PIL import Image
        Image.frombytes('RGB', (w, h), bytes(px)).save(path)
    except ImportError:
        with open(path, 'wb') as f:
            f.write(encode_png(w, h, px))


def write_p3(w, h, px, path):
    """Write an ASCII (P3) PPM — for viewers that reject binary (P6) PPMs."""
    with open(path, 'w', newline='\n') as f:  # keep LF endings on Windows too
        f.write(f"P3\n{w} {h}\n255\n")
        for y in range(h):
            row = px[(y*w)*3:(y*w + w)*3]
            f.write(' '.join(str(v) for v in row) + '\n')


def main(argv=None):
    ap = argparse.ArgumentParser(description="PPM previewer / converter (PNG + ASCII-P3)")
    ap.add_argument('ppm')
    ap.add_argument('--png', help='write a PNG (Pillow if present, else a built-in encoder)')
    ap.add_argument('--p3', dest='p3', help='write an ASCII (P3) PPM for strict PPM viewers')
    ap.add_argument('--ascii', action='store_true', help='plain ASCII luminance (no color)')
    ap.add_argument('--no-preview', action='store_true', help='skip the terminal preview')
    ap.add_argument('--cols', type=int, default=72)
    args = ap.parse_args(argv)

    w, h, px = read_ppm(args.ppm)
    if not args.no_preview:
        if args.ascii:
            print(ascii_preview(w, h, px, args.cols))
        else:
            print(ansi_preview(w, h, px, args.cols))
    print(f"[{args.ppm}: {w}x{h}]")

    if args.png:
        write_png(w, h, px, args.png)
        print(f"wrote {args.png}")
    if args.p3:
        write_p3(w, h, px, args.p3)
        print(f"wrote {args.p3}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
