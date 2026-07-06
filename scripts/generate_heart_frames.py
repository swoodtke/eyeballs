#!/usr/bin/env python3
"""Generate pre-rendered beating heart animation frames.

A glowing red heart beats with a "lub-dub" rhythm: two quick pulses
followed by a rest, one full beat per loop (60 BPM at 30 fps).

Outputs output/heart_frames.bin with 6-bit RLE compression:
  [16-byte header][64x3 palette][frame_offsets][compressed frames]

Encoding: 6-bit bitstream
  - Non-zero 6 bits: literal pixel (color 1-63)
  - Zero 6 bits: RLE marker -> next 6 bits = color, next 6 bits = count (1-63)

Frames are drawn by the firmware at 2x scale, so 208x208 fills the
412px round display.
"""

import struct
import numpy as np
from pathlib import Path
from PIL import Image, ImageFilter

W, H = 208, 208
NUM_FRAMES = 30       # one beat per loop: 30 frames @ 30 fps = 60 BPM
PALETTE_SIZE = 64
SUPERSAMPLE = 2       # render the mask at 2x for antialiased edges

HEART_REST = 26.0     # px per unit of the implicit heart curve, at rest
HEART_PEAK = 63.0     # scale at full beat — fills the display's round crop
GLOW_RADIUS = 12.0    # Gaussian blur radius for the outer glow, px
GLOW_GAIN = 0.55      # glow strength relative to the heart body

# Lub-dub: (time in beat 0-1, amplitude 0-1, rise width, fall width)
# Fast attack, slower relax so the expansion snaps and the shrink eases.
PULSES = [
    (0.15, 1.00, 0.045, 0.110),   # lub — expands to full screen
    (0.45, 0.40, 0.045, 0.110),   # dub — partial swell
]


def build_palette():
    """Black -> deep red -> bright red -> pink ramp; value maps to index."""
    stops = [
        (0,  0,   0,   0),
        (10, 45,  0,   8),
        (28, 150, 10,  25),
        (45, 230, 30,  45),
        (56, 255, 90,  100),
        (63, 255, 190, 195),
    ]
    pal = np.zeros((PALETTE_SIZE, 3), dtype=np.uint8)
    for i in range(len(stops) - 1):
        i0, r0, g0, b0 = stops[i]
        i1, r1, g1, b1 = stops[i + 1]
        for j in range(i1 - i0 + 1):
            t = j / (i1 - i0)
            pal[i0 + j] = [int(r0 + (r1 - r0) * t),
                           int(g0 + (g1 - g0) * t),
                           int(b0 + (b1 - b0) * t)]
    return pal


def beat_envelope(tfrac):
    """Sum of wrapped asymmetric time-gaussians, 0 at rest to 1 at full beat."""
    env = 0.0
    for center, amp, w_rise, w_fall in PULSES:
        for wrap in (-1.0, 0.0, 1.0):   # wrap so the loop is seamless
            dt = tfrac - center + wrap
            width = w_rise if dt < 0 else w_fall
            env += amp * np.exp(-0.5 * (dt / width) ** 2)
    return min(env, 1.0)


def render_frame(frame_idx, total_frames):
    """Render one frame as an HxW uint8 array of palette indices."""
    tfrac = frame_idx / total_frames
    env = beat_envelope(tfrac)
    scale = HEART_REST + (HEART_PEAK - HEART_REST) * env
    brightness = 1.0 + 0.45 * env

    # Antialiased heart mask via supersampled implicit curve:
    # (x^2 + y^2 - 1)^3 - x^2 * y^3 <= 0
    ss = SUPERSAMPLE
    yy, xx = np.mgrid[0:H * ss, 0:W * ss]
    cx, cy = (W * ss - 1) / 2, (H * ss - 1) / 2
    x = (xx - cx) / (scale * ss)
    y = -(yy - cy - 0.12 * scale * ss) / (scale * ss)   # +y up; nudge up a touch
    f = (x * x + y * y - 1) ** 3 - x * x * y ** 3
    mask = (f <= 0).astype(np.float32)
    mask = mask.reshape(H, ss, W, ss).mean(axis=(1, 3))

    # Interior shading: bright core fading toward the edges, with a soft
    # highlight up and left of center
    yy2, xx2 = np.mgrid[0:H, 0:W]
    hx = W / 2 - 0.35 * scale
    hy = H / 2 - 0.40 * scale
    d_hi = np.sqrt((xx2 - hx) ** 2 + (yy2 - hy) ** 2)
    highlight = np.exp(-0.5 * (d_hi / (0.55 * scale)) ** 2)
    inner = mask * (0.50 + 0.42 * highlight)

    # Outer glow: blurred mask, outside the heart only, pulsing with the beat
    mask_img = Image.fromarray((mask * 255).astype(np.uint8))
    glow = np.asarray(mask_img.filter(ImageFilter.GaussianBlur(GLOW_RADIUS)),
                      dtype=np.float32) / 255.0
    glow = glow * (1.0 - mask) * GLOW_GAIN * (0.8 + 0.6 * env)

    v = np.clip((inner + glow) * brightness, 0.0, 1.0)
    return np.rint(v * (PALETTE_SIZE - 1)).astype(np.uint8)


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
    out_path = Path(__file__).parent.parent / "output" / "heart_frames.bin"

    print(f"Generating {NUM_FRAMES} frames at {W}x{H} (6-bit palette + RLE)...")
    pal = build_palette()

    compressed_frames = []
    raw_total = 0
    for i in range(NUM_FRAMES):
        frame = render_frame(i, NUM_FRAMES)
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
