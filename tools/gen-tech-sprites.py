"""Generates the tech-tile chamfer frames and the hatch tile.

Placeholders: real icon art (SVG) replaces the hatch, and the frames stay.
Written by hand rather than with an image library because there isn't one
installed and a PNG writer is thirty lines.
"""
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, 'data', 'ui')

W = H = 54
CHAMFER = 9


def write_png(path, w, h, pixels):
    raw = b''.join(b'\x00' + bytes(v for px in row for v in px) for row in pixels)

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    header = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n'
                + chunk(b'IHDR', header)
                + chunk(b'IDAT', zlib.compress(raw, 9))
                + chunk(b'IEND', b''))


def inside(x, y):
    if not (0 <= x < W and 0 <= y < H):
        return False
    # Cut top-right and bottom-left only -- the two corners the design keeps.
    if (W - 1 - x) + y < CHAMFER:
        return False
    if x + (H - 1 - y) < CHAMFER:
        return False
    return True


def frame(rgb):
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            on_edge = inside(x, y) and not all(
                inside(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            row.append((rgb[0], rgb[1], rgb[2], 255) if on_edge else (0, 0, 0, 0))
        rows.append(row)
    return rows


def hatch():
    # 45 degrees, period 5, two units on -- the design's
    # repeating-linear-gradient(45deg, #ffffff0f 0 2px, transparent 2px 5px).
    #
    # Emitted pre-tiled at tile size rather than as a 5px unit: RmlUi 6 has no
    # repeating image decorator, and `image()` on an already-tiled bitmap is
    # the same picture for none of the trouble.
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            row.append((255, 255, 255, 15) if (x + y) % 5 < 2 else (0, 0, 0, 0))
        rows.append(row)
    return rows


for name, rgb in (('tech-frame-fitted', (0x8a, 0x5f, 0x1c)),
                  ('tech-frame-normal', (0x1d, 0x5f, 0x70)),
                  ('tech-frame-dim', (0x12, 0x37, 0x42))):
    write_png(os.path.join(OUT, name + '.png'), W, H, frame(rgb))
    print('wrote', name)

write_png(os.path.join(OUT, 'tech-hatch.png'), W, H, hatch())
print('wrote tech-hatch')
