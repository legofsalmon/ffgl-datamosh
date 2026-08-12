#!/usr/bin/env python3
"""Compare two PNG snapshots and report how different they are.

The point of this tool is that "does it look moshed" is not a test. Resolume can
capture a render target as a PNG over its REST API, so an agent driving the
plugin can capture before and after and get a number instead of an opinion:

    GET /api/v1/composition/monitors/{id}/snapshot.png

Two comparisons matter, and they are opposites:

  Mosh Amount 0 vs the effect bypassed   ->  must be IDENTICAL. Any difference is
                                             a geometry (MaxUV) or alpha bug.
  Mosh Amount 1 vs the live frame        ->  must DIVERGE. If it does not, the
                                             plugin is inert — and an inert
                                             plugin emits a pixel-exact copy of
                                             its input, which is exactly what a
                                             correctly-bypassing effect looks
                                             like.

Usage:
    python3 snapshot-diff.py a.png b.png
    python3 snapshot-diff.py a.png b.png --json
    python3 snapshot-diff.py a.png b.png --expect identical
    python3 snapshot-diff.py a.png b.png --expect different --min-changed 2.0

Exit codes: 0 the expectation held (or none was given), 1 it did not, 2 the
images could not be compared at all.

Pure standard library — zlib and struct are enough to read a PNG, so there is
nothing to install on the machine running Resolume.

Handles 8- and 16-bit greyscale, truecolour, and both with alpha. Palette
(colour type 3) and interlaced PNGs are rejected rather than guessed at;
Resolume emits neither.
"""

import argparse
import json
import struct
import sys
import zlib


class PngError(Exception):
    pass


CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}   # greyscale, RGB, palette, grey+A, RGBA


def read_png(path):
    """Return (width, height, channels, bytes_per_sample, pixels as a bytearray).

    Samples are returned at their native depth, big-endian for 16-bit, exactly as
    the PNG stores them.
    """
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise PngError(f"{path}: not a PNG")

    idat = bytearray()
    width = height = depth = colour = None
    offset = 8
    while offset < len(data):
        (length,) = struct.unpack(">I", data[offset:offset + 4])
        tag = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        offset += 12 + length          # length + tag + body + crc

        if tag == b"IHDR":
            width, height, depth, colour, comp, filt, interlace = struct.unpack(">IIBBBBB", body)
            if interlace:
                raise PngError(f"{path}: interlaced PNGs are not supported")
            if colour == 3:
                raise PngError(f"{path}: palette PNGs are not supported")
            if colour not in CHANNELS:
                raise PngError(f"{path}: unknown colour type {colour}")
            if depth not in (8, 16):
                raise PngError(f"{path}: unsupported bit depth {depth}")
            if comp != 0 or filt != 0:
                raise PngError(f"{path}: unsupported compression or filter method")
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break

    if width is None:
        raise PngError(f"{path}: no IHDR")
    if not idat:
        raise PngError(f"{path}: no image data")

    channels = CHANNELS[colour]
    sample_bytes = depth // 8
    stride = width * channels * sample_bytes
    step = channels * sample_bytes          # distance to the pixel on the left

    raw = zlib.decompress(bytes(idat))
    expected = (stride + 1) * height
    if len(raw) < expected:
        raise PngError(f"{path}: truncated image data ({len(raw)} of {expected} bytes)")

    out = bytearray(stride * height)
    prior = bytearray(stride)
    pos = 0
    for row in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride

        # The five PNG filters, reversed in place. Each works on bytes, not on
        # samples, which is why `step` is in bytes rather than channels.
        if filter_type == 1:            # Sub
            for i in range(step, stride):
                line[i] = (line[i] + line[i - step]) & 0xFF
        elif filter_type == 2:          # Up
            for i in range(stride):
                line[i] = (line[i] + prior[i]) & 0xFF
        elif filter_type == 3:          # Average
            for i in range(stride):
                left = line[i - step] if i >= step else 0
                line[i] = (line[i] + ((left + prior[i]) >> 1)) & 0xFF
        elif filter_type == 4:          # Paeth
            for i in range(stride):
                left = line[i - step] if i >= step else 0
                up = prior[i]
                upleft = prior[i - step] if i >= step else 0
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                if pa <= pb and pa <= pc:
                    pred = left
                elif pb <= pc:
                    pred = up
                else:
                    pred = upleft
                line[i] = (line[i] + pred) & 0xFF
        elif filter_type != 0:
            raise PngError(f"{path}: unknown row filter {filter_type} on row {row}")

        out[row * stride:(row + 1) * stride] = line
        prior = line

    return width, height, channels, sample_bytes, out


def samples(buffer, sample_bytes):
    """Iterate samples as ints, whatever the bit depth."""
    if sample_bytes == 1:
        return buffer
    return [int.from_bytes(buffer[i:i + 2], "big") for i in range(0, len(buffer), 2)]


def compare(path_a, path_b, threshold):
    wa, ha, ca, sa, pa = read_png(path_a)
    wb, hb, cb, sb, pb = read_png(path_b)

    if (wa, ha) != (wb, hb):
        raise PngError(f"different dimensions: {wa}x{ha} vs {wb}x{hb}")
    if ca != cb or sa != sb:
        raise PngError(f"different pixel formats: {ca}ch/{sa*8}bit vs {cb}ch/{sb*8}bit")

    a = samples(pa, sa)
    b = samples(pb, sb)
    maximum = 255 if sa == 1 else 65535

    total = 0
    peak = 0
    changed_pixels = 0
    pixel_count = wa * ha

    for pixel in range(pixel_count):
        base = pixel * ca
        worst = 0
        for c in range(ca):
            d = a[base + c] - b[base + c]
            if d < 0:
                d = -d
            total += d
            if d > worst:
                worst = d
        if worst > peak:
            peak = worst
        # Scaled to 0..255 so the threshold means the same thing at either depth.
        if worst * 255 // maximum > threshold:
            changed_pixels += 1

    mean = total / (pixel_count * ca)
    return {
        "width": wa,
        "height": ha,
        "channels": ca,
        "bit_depth": sa * 8,
        "identical": total == 0,
        "mean_abs_diff": round(mean * 255 / maximum, 4),
        "peak_abs_diff": round(peak * 255 / maximum, 4),
        "changed_pixels": changed_pixels,
        "changed_percent": round(100.0 * changed_pixels / pixel_count, 4),
        "threshold": threshold,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--threshold", type=int, default=1, metavar="N",
                    help="a pixel counts as changed when a channel differs by more "
                         "than N on a 0-255 scale (default 1, which absorbs codec "
                         "and rounding noise without hiding a real change)")
    ap.add_argument("--expect", choices=("identical", "different"),
                    help="assert the relationship and set the exit code")
    ap.add_argument("--min-changed", type=float, default=1.0, metavar="PCT",
                    help="with --expect different, the percentage of pixels that "
                         "must have changed for it to count (default 1.0)")
    args = ap.parse_args()

    try:
        result = compare(args.a, args.b, args.threshold)
    except (PngError, OSError, zlib.error) as exc:
        print(f"cannot compare: {exc}", file=sys.stderr)
        return 2

    verdict = None
    if args.expect == "identical":
        verdict = result["identical"]
        result["expected"] = "identical"
    elif args.expect == "different":
        verdict = result["changed_percent"] >= args.min_changed
        result["expected"] = f"different (>= {args.min_changed}% of pixels)"
    if verdict is not None:
        result["verdict"] = "PASS" if verdict else "FAIL"

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"  {result['width']}x{result['height']}  {result['channels']}ch  {result['bit_depth']}-bit")
        print(f"  identical        {result['identical']}")
        print(f"  mean abs diff    {result['mean_abs_diff']}   (0-255 scale)")
        print(f"  peak abs diff    {result['peak_abs_diff']}")
        print(f"  changed pixels   {result['changed_pixels']}  ({result['changed_percent']}%)")
        if verdict is not None:
            print(f"  expected         {result['expected']}")
            print(f"  VERDICT          {result['verdict']}")

    if verdict is None:
        return 0
    return 0 if verdict else 1


if __name__ == "__main__":
    sys.exit(main())
