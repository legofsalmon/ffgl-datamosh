#!/usr/bin/env python3
"""Generates site/og.png, the 1200x630 preview image for shared links.

Pure standard library — zlib and struct are enough to write a PNG, so there is
nothing to install and this runs anywhere Python does.

The image is the effect explaining itself, the same idea as the plugin's own
thumbnails: a clean image above the line, and below it the same image carried
sideways in macroblocks by motion that no longer matches its content. That is
the one picture that says what datamoshing is without a caption.

    python3 site/tools/make-og.py
"""

import math
import pathlib
import struct
import zlib

WIDTH, HEIGHT = 1200, 630
BLOCK = 30
FRONT_Y = 0.46          # fraction of the height where refreshing stops
OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "og.png"

# Matches the site's dark palette, since link previews are usually shown on a
# dark card and the page's own dark theme is the one people see most.
GROUND = (0x12, 0x10, 0x16)
INK = (0xEC, 0xE7, 0xEF)
ACCENT = (0xFF, 0x4D, 0x9D)
GREEN = (0x3F, 0xD2, 0xA4)


def lattice(x: int, y: int, salt: int) -> float:
    """Deterministic value in 0..1. The same hash the test harness uses."""
    h = (x * 374761393 + y * 668265263 + salt * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0xFFFFFF) / 0xFFFFFF


def source_pixel(x: float, y: float) -> tuple:
    """The undamaged frame: hard-edged diagonal bars and a solid disc.

    Everything here is deliberately crisp. A soft gradient hides displacement —
    shifting a blur by thirty pixels still looks like a blur. Hard edges are
    what make a block that has moved obviously a block that has moved, which is
    the entire job of this image.
    """
    # Diagonal bars with hard edges, in screen space so they stay put when the
    # image is sampled from a displaced coordinate.
    phase = (x * 0.62 + y * 0.42) / 92.0
    bar = 1.0 if (phase - math.floor(phase)) < 0.46 else 0.0

    # Thin bright rule inside each bar: fine detail that shears visibly.
    rule = 1.0 if 0.40 <= (phase - math.floor(phase)) < 0.46 else 0.0

    r = GROUND[0] / 255 + bar * 0.10 + rule * 0.34
    g = GROUND[1] / 255 + bar * 0.07 + rule * 0.30
    b = GROUND[2] / 255 + bar * 0.20 + rule * 0.42

    # A solid disc with a defined edge, not a glow.
    dx, dy = (x - WIDTH * 0.29) / 210.0, (y - HEIGHT * 0.40) / 210.0
    d = math.sqrt(dx * dx + dy * dy)
    if d < 1.0:
        edge = min(1.0, (1.0 - d) * 7.0)
        r += edge * (ACCENT[0] / 255) * 0.92
        g += edge * (ACCENT[1] / 255) * 0.62
        b += edge * (ACCENT[2] / 255) * 0.88

    # A second, cooler shape so the frame is not one hue and the tearing has
    # more than one thing to cut across.
    ex, ey = (x - WIDTH * 0.72) / 175.0, (y - HEIGHT * 0.30) / 130.0
    e = math.sqrt(ex * ex + ey * ey)
    if e < 1.0:
        edge = min(1.0, (1.0 - e) * 8.0)
        r += edge * (GREEN[0] / 255) * 0.34
        g += edge * (GREEN[1] / 255) * 0.80
        b += edge * (GREEN[2] / 255) * 0.62

    return (min(1.0, r), min(1.0, g), min(1.0, b))


def render() -> bytearray:
    front_row = int(HEIGHT * FRONT_Y / BLOCK)
    rows = bytearray()

    for y in range(HEIGHT):
        rows.append(0)  # PNG filter type 0 for this scanline
        block_row = y // BLOCK
        for x in range(WIDTH):
            block_col = x // BLOCK

            sample_x, sample_y = float(x), float(y)
            drift = 0.0

            if block_row >= front_row:
                # Displacement grows with distance past the front, and every
                # block gets its own vector — which is what makes it read as
                # blocks rather than as a blur.
                depth = (block_row - front_row) / max(1, (HEIGHT // BLOCK) - front_row)
                strength = 26.0 + depth * 190.0
                sample_x -= strength * (0.35 + lattice(block_col, block_row, 1))
                sample_y -= strength * 0.16 * (lattice(block_col, block_row, 2) - 0.5)
                drift = strength * 0.05

            r, g, b = source_pixel(sample_x, sample_y)

            if drift > 0.0:
                # Channels pulled apart: the fringing of a block whose colour
                # planes disagree about where they came from.
                r = source_pixel(sample_x + drift, sample_y)[0]
                b = source_pixel(sample_x - drift, sample_y)[2]

            # The line where refreshing stops.
            if abs(y - HEIGHT * FRONT_Y) < 1.6:
                r, g, b = (c / 255 for c in ACCENT)

            rows.append(int(r * 255 + 0.5))
            rows.append(int(g * 255 + 0.5))
            rows.append(int(b * 255 + 0.5))

    return rows


def draw_text_block(rows: bytearray) -> None:
    """Lays a solid plate under the lower-left corner.

    No glyph rendering: fonts are the one thing the standard library cannot do,
    and a wordmark baked into a raster would go stale the moment anything is
    renamed. The plate gives the title in the unfurled card somewhere quiet to
    sit against, which is what actually matters.
    """
    plate_h = 64
    for y in range(HEIGHT - plate_h, HEIGHT):
        # Fade in from transparent at the top of the plate to solid at the base.
        t = (y - (HEIGHT - plate_h)) / plate_h
        alpha = min(0.92, t * 1.5)
        base = 1 + y * (WIDTH * 3 + 1)
        for x in range(WIDTH):
            i = base + x * 3
            for c in range(3):
                rows[i + c] = int(rows[i + c] * (1 - alpha) + GROUND[c] * alpha)


def write_png(path: pathlib.Path, rows: bytearray) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)  # 8-bit truecolour
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


if __name__ == "__main__":
    pixels = render()
    draw_text_block(pixels)
    write_png(OUTPUT, pixels)
    print(f"wrote {OUTPUT} ({OUTPUT.stat().st_size / 1024:.0f} KB, {WIDTH}x{HEIGHT})")
