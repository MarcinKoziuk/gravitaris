"""Generates the tech-tile chamfer frames, the hatch tile, and the hover glows.

Placeholders: real icon art (SVG) replaces the hatch, and the frames stay.
Written by hand rather than with an image library because there isn't one
installed here.

TGA rather than PNG because that is what the game's own RmlUi render
interface reads -- see RenderInterfaceGL3::LoadTexture, which handles
24/32-bit uncompressed TGA and nothing else.

The glows are baked here rather than asked of RCSS because this game's render
interface implements neither filters nor render layers (RenderInterfaceGL3 has
no CompileFilter/PushLayer), so `box-shadow` and `filter: drop-shadow` render
as nothing at all. A textured quad is what the renderer can actually draw.
"""
import math
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, 'data', 'ui')

W = H = 54
CHAMFER = 9

# How far past the tile the glow reaches, and the canvas that leaves. The
# markup hangs a halo element this far outside the tile on every side, so the
# sprite lands pixel for pixel rather than stretched.
GLOW_REACH = 10
GLOW_SIZE = W + GLOW_REACH * 2


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


# How far outside the tile a point is, in pixels. The shape is convex -- a
# square with two corners cut -- so the nearest of its edge half-planes is the
# distance to the shape itself, and the two cuts fall out of it as mitres.
def outside_depth(px, py):
    edges = (px, (W - 1) - px, py, (H - 1) - py,
             ((W - 1 - px) + py - CHAMFER) / math.sqrt(2.0),
             (px + (H - 1 - py) - CHAMFER) / math.sqrt(2.0))
    return max(0.0, -min(edges))


# A soft halo hugging the tile's outline, and nothing at all inside it: the
# frame and the code are already drawn there, and a wash over them would cost
# the text its contrast for no gain.
def glow(rgb, peak):
    rows = []
    for y in range(GLOW_SIZE):
        row = []
        for x in range(GLOW_SIZE):
            depth = outside_depth(x - GLOW_REACH, y - GLOW_REACH)
            if depth <= 0.0 or depth >= GLOW_REACH:
                row.append((0, 0, 0, 0))
                continue
            falloff = 1.0 - depth / GLOW_REACH
            row.append((rgb[0], rgb[1], rgb[2], int(round(peak * falloff * falloff))))
        rows.append(row)
    return rows


for name, rgb in (('tech-frame-fitted', (0x8a, 0x5f, 0x1c)),
                  ('tech-frame-normal', (0x1d, 0x5f, 0x70)),
                  ('tech-frame-dim', (0x12, 0x37, 0x42)),
                  # Staged: chosen but not yet paid for. White, and the
                  # brightest frame on the board -- a plan should be the thing
                  # the eye goes to.
                  ('tech-frame-staged', (0xff, 0xff, 0xff)),
                  # Selected: the mount the AVAILABLE SYSTEMS list is for.
                  # Cyan, the board's own "you may act on this" colour -- amber
                  # would say the mount carries something, which is a different
                  # thing that a selected mount may or may not also be.
                  ('tech-frame-selected', (0x56, 0xd9, 0xf2))):
    write_tga(os.path.join(OUT, name + '.tga'), W, H, frame(rgb))
    print('wrote', name)

write_tga(os.path.join(OUT, 'tech-hatch.tga'), W, H, hatch())
print('wrote tech-hatch')

# One glow per thing a tile can be, because an image decorator cannot be
# tinted -- the same reason there is a frame per state. Amber for what is
# already held, cyan for what is on offer, white for what is planned, and a
# barely-there steel for what cannot be taken at all.
for name, rgb, peak in (('tech-glow-cyan', (0x56, 0xd9, 0xf2), 120),
                        ('tech-glow-amber', (0xff, 0xb3, 0x40), 115),
                        ('tech-glow-white', (0xff, 0xff, 0xff), 135),
                        ('tech-glow-dim', (0x4a, 0x7a, 0x86), 70)):
    write_tga(os.path.join(OUT, name + '.tga'), GLOW_SIZE, GLOW_SIZE, glow(rgb, peak))
    print('wrote', name)
