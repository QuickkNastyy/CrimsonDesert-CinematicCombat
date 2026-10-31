"""Convert a 32-bit top-down BMP written by `ui snapshot` into a PNG (stdlib only): python tools/bmp2png.py in.bmp [out.png]"""
import os
import struct
import sys
import zlib


def convert(bmp, png):
    d = open(bmp, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    if bpp != 32:
        raise SystemExit('expected a 32-bit BMP')
    top_down = h < 0
    h = abs(h)
    rows = []
    for y in range(h):
        sy = y if top_down else h - 1 - y
        row = d[off + sy * w * 4: off + (sy + 1) * w * 4]
        out = bytearray(b'\x00')
        for x in range(w):
            b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            out += bytes((r, g, b))
        rows.append(bytes(out))
    raw = b''.join(rows)

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(png, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + '.png'
    convert(src, dst)
    print(dst)
