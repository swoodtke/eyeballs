#!/usr/bin/env python3
"""Generate pre-rendered blob eye animation frames.

Outputs main/blob_frames.bin with 6-bit RLE compression:
  [16-byte header][64×3 palette][frame_offsets][compressed frames]

Encoding: 6-bit bitstream
  - Non-zero 6 bits: literal pixel (color 1-63)
  - Zero 6 bits: RLE marker → next 6 bits = color, next 6 bits = count (1-63)
"""

import struct
import math
import numpy as np
from pathlib import Path

W, H = 250, 200
NUM_FRAMES = 120
PALETTE_SIZE = 64


def build_palette():
    """Build a 64-color gradient palette for the glowing blob."""
    # First build a 256-color palette, then sample 64 from it
    pal256 = np.zeros((256, 3), dtype=np.uint8)
    stops = [
        (0,    0,   0,   0),
        (40,   20,  5,   0),
        (80,   120, 30,  0),
        (120,  220, 80,  0),
        (155,  255, 140, 10),
        (180,  240, 60,  5),
        (210,  140, 15,  0),
        (235,  40,  3,   0),
        (255,  0,   0,   0),
    ]
    for i in range(len(stops) - 1):
        i0, r0, g0, b0 = stops[i]
        i1, r1, g1, b1 = stops[i + 1]
        n = i1 - i0
        for j in range(n):
            t = j / n
            idx = i0 + j
            pal256[idx] = [
                int(r0 + (r1 - r0) * t),
                int(g0 + (g1 - g0) * t),
                int(b0 + (b1 - b0) * t),
            ]
    pal256[255] = [0, 0, 0]

    # Sample 64 evenly from 256
    pal64 = np.zeros((PALETTE_SIZE, 3), dtype=np.uint8)
    for i in range(PALETTE_SIZE):
        pal64[i] = pal256[i * 255 // (PALETTE_SIZE - 1)]
    return pal64, pal256


def quantize_frame(frame_256, pal64, pal256):
    """Map 8-bit palette indices to nearest 6-bit palette indices."""
    # Build mapping: for each 256-color index, find closest 64-color index
    mapping = np.zeros(256, dtype=np.uint8)
    pal64_f = pal64.astype(np.float32)
    for i in range(256):
        c = pal256[i].astype(np.float32)
        dists = np.sum((pal64_f - c) ** 2, axis=1)
        mapping[i] = np.argmin(dists)
    return mapping[frame_256]


def encode_rle_6bit(frame):
    """Encode a frame (H×W uint8 with values 0-63) as a 6-bit RLE bitstream."""
    pixels = frame.flatten()
    bits = []

    def emit6(val):
        for bit in range(5, -1, -1):
            bits.append((val >> bit) & 1)

    i = 0
    n = len(pixels)
    while i < n:
        color = int(pixels[i])
        # Count run length
        run = 1
        while i + run < n and pixels[i + run] == color and run < 63:
            run += 1

        if color == 0 or run >= 3:
            # RLE: escape(0) + color(6) + count(6) = 18 bits
            # Worth it for color 0 (always), or runs >= 3 (18 bits vs 3×6=18, break even)
            emit6(0)
            emit6(color)
            emit6(run)
            i += run
        else:
            # Literal
            emit6(color)
            i += 1

    # Pad to byte boundary
    while len(bits) % 8 != 0:
        bits.append(0)

    # Pack bits into bytes
    result = bytearray()
    for j in range(0, len(bits), 8):
        byte = 0
        for k in range(8):
            byte = (byte << 1) | bits[j + k]
        result.append(byte)

    return bytes(result)


def render_frame(frame_idx, total_frames):
    """Render a single frame as a W×H uint8 array of 256-color palette indices."""
    t = frame_idx / total_frames * 2.0 * math.pi

    cx, cy = W / 2, H / 2

    angle = math.sin(t * 0.3) * 0.5
    cos_a, sin_a = math.cos(angle), math.sin(angle)

    b0_r = 35.0 + 8.0 * math.sin(t * 1.1)
    b1_r = 32.0 + 6.0 * math.sin(t * 0.9 + 1.5)

    spacing = (b0_r + b1_r) * 0.5 + 5.0 * math.sin(t * 0.5)

    b0_x = cx + cos_a * spacing * 0.5
    b0_y = cy + sin_a * spacing * 0.5
    b1_x = cx - cos_a * spacing * 0.5
    b1_y = cy - sin_a * spacing * 0.5

    phase3 = (t * 0.4) % (2 * math.pi)
    life3 = max(0, math.sin(phase3))
    orbit_angle = t * 0.3 + 2.0
    orbit_dist = b0_r * 0.4 + 5.0
    b2_x = b0_x + math.cos(orbit_angle) * orbit_dist
    b2_y = b0_y + math.sin(orbit_angle) * orbit_dist
    b2_r = 12.0 * life3 + 15.0

    blobs = [[b0_x, b0_y, b0_r], [b1_x, b1_y, b1_r], [b2_x, b2_y, b2_r]]

    min_bridge_field = 3.0
    pairs = [(0, 1), (0, 2)]
    for i, j in pairs:
        bx_i, by_i, br_i = blobs[i]
        bx_j, by_j, br_j = blobs[j]
        if br_j < 5:
            continue
        mx = (bx_i + bx_j) / 2
        my = (by_i + by_j) / 2
        d2_i = (mx - bx_i) ** 2 + (my - by_i) ** 2
        d2_j = (mx - bx_j) ** 2 + (my - by_j) ** 2
        if d2_i < 1: d2_i = 1
        if d2_j < 1: d2_j = 1
        field_mid = (br_i ** 2) / d2_i + (br_j ** 2) / d2_j
        if field_mid < min_bridge_field:
            boost = math.sqrt(min_bridge_field / max(field_mid, 0.01))
            blobs[i][2] = br_i * boost
            blobs[j][2] = br_j * boost

    blobs = [(b[0], b[1], b[2]) for b in blobs]

    yy, xx = np.mgrid[0:H, 0:W]
    xx = xx.astype(np.float32)
    yy = yy.astype(np.float32)

    field = np.zeros((H, W), dtype=np.float32)
    for bx, by, br in blobs:
        if br < 1:
            continue
        dx = xx - bx
        dy = yy - by
        d2 = dx * dx + dy * dy
        d2 = np.maximum(d2, 1.0)
        field += (br * br) / d2

    frame = np.zeros((H, W), dtype=np.uint8)

    mask_glow = (field >= 0.3) & (field < 1.0)
    t_glow = (field[mask_glow] - 0.3) / 0.7
    frame[mask_glow] = (1 + t_glow * 154).astype(np.uint8)

    mask_rim = (field >= 1.0) & (field < 2.5)
    t_rim = (field[mask_rim] - 1.0) / 1.5
    frame[mask_rim] = (155 + t_rim * 55).astype(np.uint8)

    mask_inner = field >= 2.5
    t_inner = np.clip((field[mask_inner] - 2.5) / 3.0, 0, 1)
    frame[mask_inner] = (210 + t_inner * 45).astype(np.uint8)

    return frame


def main():
    out_path = Path(__file__).parent.parent / "output" / "blob_frames.bin"

    print(f"Generating {NUM_FRAMES} frames at {W}×{H} (6-bit palette + RLE)...")
    pal64, pal256 = build_palette()

    compressed_frames = []
    raw_total = 0
    for i in range(NUM_FRAMES):
        frame_256 = render_frame(i, NUM_FRAMES)
        frame_64 = quantize_frame(frame_256, pal64, pal256)
        compressed = encode_rle_6bit(frame_64)
        compressed_frames.append(compressed)
        raw_total += W * H
        if (i + 1) % 10 == 0:
            print(f"  Frame {i + 1}/{NUM_FRAMES}")

    # Header: 16 bytes
    # [width:u16][height:u16][num_frames:u16][palette_size:u16][bits_per_pixel:u8][reserved:7]
    header = struct.pack("<HHHHbb", W, H, NUM_FRAMES, PALETTE_SIZE, 6, 0)  # playback=0 (ping-pong)
    header += b"\x00" * 6

    # Frame offset table: uint32 per frame (offset from start of file)
    palette_bytes = PALETTE_SIZE * 3
    offset_table_size = NUM_FRAMES * 4
    data_start = 16 + palette_bytes + offset_table_size

    offsets = []
    pos = data_start
    for cf in compressed_frames:
        offsets.append(pos)
        pos += len(cf)

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(pal64.tobytes())
        for off in offsets:
            f.write(struct.pack("<I", off))
        for cf in compressed_frames:
            f.write(cf)

    comp_total = sum(len(cf) for cf in compressed_frames)
    total_size = pos
    print(f"Written {out_path}")
    print(f"  Raw: {raw_total / 1024:.0f} KB, Compressed: {comp_total / 1024:.0f} KB "
          f"({comp_total * 100 / raw_total:.1f}%)")
    print(f"  Total file: {total_size / 1024:.1f} KB")


if __name__ == "__main__":
    main()
