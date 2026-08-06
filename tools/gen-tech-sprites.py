"""Generates the tech-tile chamfer frames and the hatch tile.

Placeholders: real icon art (SVG) replaces the hatch, and the frames stay.
Written by hand rather than with an image library because there isn't one
installed here.

TGA rather than PNG because that is what the game's own RmlUi render
interface reads -- see RenderInterfaceGL3::LoadTexture, which handles
24/32-bit uncompressed TGA and nothing else.
"""
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, 'data', 'ui')

W = H = 54
CHAMFER = 9


def write_tga(path, w, h, pixels):
    # 32-bit uncompressed true-colour, top-left origin, 8 alpha bits.
    header = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x28)
    body = bytearray()
    for row in pixels:
        for r, g, b, a in row:
            body += bytes((b, g, r, a))  # TGA is BGRA
    with open(path, 'wb') as f:
        f.write(header + bytes(body))


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
                  ('tech-frame-dim', (0x12, 0x37, 0x42)),
                  # Staged: chosen but not yet paid for. White, and the
                  # brightest frame on the board -- a plan should be the thing
                  # the eye goes to.
                  ('tech-frame-staged', (0xff, 0xff, 0xff))):
    write_tga(os.path.join(OUT, name + '.tga'), W, H, frame(rgb))
    print('wrote', name)

write_tga(os.path.join(OUT, 'tech-hatch.tga'), W, H, hatch())
print('wrote tech-hatch')
