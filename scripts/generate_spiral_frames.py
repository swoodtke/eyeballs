#!/usr/bin/env python3
"""Generate pre-rendered spiral-over-rings animation frames.

A black Archimedean spiral winds outward from the center, occluding
brightly colored concentric rings that slowly shrink toward the middle.

Outputs output/spiral_frames.bin with 6-bit RLE compression:
  [16-byte header][64x3 palette][frame_offsets][compressed frames]

Encoding: 6-bit bitstream
  - Non-zero 6 bits: literal pixel (color 1-63)
  - Zero 6 bits: RLE marker -> next 6 bits = color, next 6 bits = count (1-63)

Frames are drawn by the firmware at 2x scale, sized for the larger
1.75" board (233x233 -> 466px). On the 1.46" (412px) the firmware's
centered blit crops the overhanging edges, which is harmless for a
radially-centered animation.
"""

import struct
import numpy as np
from pathlib import Path

W, H = 233, 233
NUM_FRAMES = 360
PALETTE_SIZE = 64

NUM_HUES = 6          # distinct ring colors (rainbow)
NUM_SHADES = 10       # brightness levels per hue (soft edges / ring shading)
RING_PERIOD = 30.0    # radial spacing between ring centers, px
RING_WIDTH = 7.0      # approx visible width (FWHM) of each bright ring, px
RING_CYCLES = 2       # ring color cycles per loop (integer for seamless loop)
SPIRAL_PITCH = 55.0   # radial distance between spiral windings, px
SPIRAL_DUTY = 0.25    # fraction of the pitch occupied by the black band
SPIRAL_TURNS = 1      # full rotations per loop (integer for seamless loop)
CORE_R = 6.0          # black core radius the spiral emerges from
EDGE_PX = 7.0         # softness of the spiral band edge, px

# Bright, fully saturated rainbow bases
HUE_RGB = np.array([
    [255, 40,  40],   # red
    [255, 200, 0],    # yellow
    [40,  255, 60],   # green
    [0,   220, 255],  # cyan
    [60,  80,  255],  # blue
    [230, 40,  255],  # magenta
], dtype=np.float32)


def build_palette():
    """Index 0 = black; then NUM_HUES x NUM_SHADES ramps from dark to full."""
    pal = np.zeros((PALETTE_SIZE, 3), dtype=np.uint8)
    for h in range(NUM_HUES):
        for s in range(NUM_SHADES):
            value = ((s + 1) / NUM_SHADES) ** 0.85
            pal[1 + h * NUM_SHADES + s] = (HUE_RGB[h] * value).astype(np.uint8)
    return pal


def render_frame(frame_idx, total_frames, r, theta):
    """Render one frame as an HxW uint8 array of palette indices."""
    tfrac = frame_idx / total_frames

    # Rings shrink inward; over one loop they shift by an integer number
    # of full color cycles (RING_PERIOD * NUM_HUES px each) so frame N
    # wraps to frame 0.
    shift = tfrac * RING_PERIOD * NUM_HUES * RING_CYCLES
    ring_pos = (r + shift) / RING_PERIOD
    ring_idx = np.floor(ring_pos).astype(np.int32)
    frac = ring_pos - ring_idx
    hue_idx = ring_idx % NUM_HUES

    # Thin Gaussian rings centered in each period, black between them
    sigma = (RING_WIDTH / RING_PERIOD) / 2.355  # FWHM -> sigma, in frac units
    bright = np.exp(-0.5 * ((frac - 0.5) / sigma) ** 2)

    # Archimedean spiral band, rotating so it appears to grow outward
    # from the center. Integer SPIRAL_TURNS keeps the loop seamless.
    sp = (theta / (2 * np.pi) + r / SPIRAL_PITCH - tfrac * SPIRAL_TURNS) % 1.0
    m = np.minimum(sp, SPIRAL_DUTY - sp)          # >0 inside the black band
    edge = EDGE_PX / SPIRAL_PITCH
    occ = np.clip(m / edge + 0.5, 0.0, 1.0)

    # Black core the spiral emerges from
    occ = np.maximum(occ, np.clip((CORE_R - r) / 2.0 + 0.5, 0.0, 1.0))

    v = bright * (1.0 - occ)
    shade = np.rint(v * NUM_SHADES).astype(np.int32)  # 0..NUM_SHADES
    frame = np.where(shade <= 0, 0,
                     1 + hue_idx * NUM_SHADES + (shade - 1)).astype(np.uint8)
    return frame


def encode_rle_6bit(frame):
    """Encode a frame (HxW uint8 with values 0-63) as a 6-bit RLE bitstream."""
    pixels = frame.flatten()
    bits = []

    def emit6(val):
        for bit in range(5, -1, -1):
            bits.append((val >> bit) & 1)

    i = 0
    n = len(pixels)
    while i < n:
        color = int(pixels[i])
        run = 1
        while i + run < n and pixels[i + run] == color and run < 63:
            run += 1

        if color == 0 or run >= 3:
            emit6(0)
            emit6(color)
            emit6(run)
            i += run
        else:
            emit6(color)
            i += 1

    while len(bits) % 8 != 0:
        bits.append(0)

    result = bytearray()
    for j in range(0, len(bits), 8):
        byte = 0
        for k in range(8):
            byte = (byte << 1) | bits[j + k]
        result.append(byte)

    return bytes(result)


def main():
    out_path = Path(__file__).parent.parent / "output" / "spiral_frames.bin"

    print(f"Generating {NUM_FRAMES} frames at {W}x{H} (6-bit palette + RLE)...")
    pal = build_palette()

    cx, cy = (W - 1) / 2, (H - 1) / 2
    yy, xx = np.mgrid[0:H, 0:W]
    dx = xx.astype(np.float32) - cx
    dy = yy.astype(np.float32) - cy
    r = np.sqrt(dx * dx + dy * dy)
    theta = np.arctan2(dy, dx)

    compressed_frames = []
    raw_total = 0
    for i in range(NUM_FRAMES):
        frame = render_frame(i, NUM_FRAMES, r, theta)
        compressed_frames.append(encode_rle_6bit(frame))
        raw_total += W * H
        if (i + 1) % 10 == 0:
            print(f"  Frame {i + 1}/{NUM_FRAMES}")

    # Header: 16 bytes
    # [width:u16][height:u16][num_frames:u16][palette_size:u16][bpp:u8][playback:u8]
    header = struct.pack("<HHHHbb", W, H, NUM_FRAMES, PALETTE_SIZE, 6, 1)  # playback=1 (loop)
    header += b"\x00" * 6

    palette_bytes = PALETTE_SIZE * 3
    offset_table_size = NUM_FRAMES * 4
    data_start = 16 + palette_bytes + offset_table_size

    offsets = []
    pos = data_start
    for cf in compressed_frames:
        offsets.append(pos)
        pos += len(cf)

    out_path.parent.mkdir(exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(pal.tobytes())
        for off in offsets:
            f.write(struct.pack("<I", off))
        for cf in compressed_frames:
            f.write(cf)

    comp_total = sum(len(cf) for cf in compressed_frames)
    print(f"Written {out_path}")
    print(f"  Raw: {raw_total / 1024:.0f} KB, Compressed: {comp_total / 1024:.0f} KB "
          f"({comp_total * 100 / raw_total:.1f}%)")
    print(f"  Total file: {pos / 1024:.1f} KB")


if __name__ == "__main__":
    main()
