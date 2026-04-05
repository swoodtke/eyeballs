#!/usr/bin/env python3
"""Generate pre-rendered Sauron eye animation frames.

Fiery vertical slit with fractal fire textures using fBm noise.
Same format as blob_frames.bin: 6-bit palette + RLE compression.
"""

import struct
import math
import numpy as np
from pathlib import Path

W, H = 250, 200
NUM_FRAMES = 120
PALETTE_SIZE = 64


# ─── Fractal noise ──────────────────────────────────────────────────────────

def _fade(t):
    """Perlin smoothstep: 6t^5 - 15t^4 + 10t^3"""
    return t * t * t * (t * (t * 6 - 15) + 10)


def _hash_grid(shape, seed=0):
    """Generate a random gradient grid."""
    rng = np.random.RandomState(seed)
    return rng.uniform(-1, 1, shape).astype(np.float32)


class ValueNoise2D:
    """Simple 2D value noise with smooth interpolation."""
    def __init__(self, seed=42, size=256):
        rng = np.random.RandomState(seed)
        self.perm = rng.permutation(size).astype(np.int32)
        self.values = rng.uniform(0, 1, size).astype(np.float32)
        self.size = size

    def _hash(self, ix, iy):
        return self.perm[(self.perm[ix % self.size] + iy) % self.size]

    def sample(self, x, y):
        """Sample noise at (x, y) arrays. Returns 0-1."""
        ix = np.floor(x).astype(np.int32)
        iy = np.floor(y).astype(np.int32)
        fx = x - ix
        fy = y - iy
        u = _fade(fx)
        v = _fade(fy)

        v00 = self.values[self._hash(ix, iy)]
        v10 = self.values[self._hash(ix + 1, iy)]
        v01 = self.values[self._hash(ix, iy + 1)]
        v11 = self.values[self._hash(ix + 1, iy + 1)]

        return (v00 * (1 - u) * (1 - v) +
                v10 * u * (1 - v) +
                v01 * (1 - u) * v +
                v11 * u * v)


def fbm(noise_gen, x, y, octaves=5, lacunarity=2.0, gain=0.5):
    """Fractal Brownian motion — sum of multiple noise octaves."""
    value = np.zeros_like(x)
    amplitude = 1.0
    frequency = 1.0
    max_val = 0.0

    for _ in range(octaves):
        value += amplitude * noise_gen.sample(x * frequency, y * frequency)
        max_val += amplitude
        amplitude *= gain
        frequency *= lacunarity

    return value / max_val  # normalize to 0-1


# ─── Palette ─────────────────────────────────────────────────────────────────

def build_palette():
    """Build a 64-color fire palette."""
    pal256 = np.zeros((256, 3), dtype=np.uint8)
    stops = [
        (0,    0,   0,   0),
        (25,   10,  1,   0),
        (50,   60,  5,   0),
        (80,   160, 25,  0),
        (110,  230, 60,  0),
        (135,  255, 120, 10),
        (155,  255, 180, 50),
        (170,  255, 220, 130),
        (185,  255, 240, 200),   # near-white hot center
        (200,  240, 100, 10),
        (220,  160, 25,  0),
        (240,  50,  5,   0),
        (255,  0,   0,   0),     # black (slit interior)
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

    pal64 = np.zeros((PALETTE_SIZE, 3), dtype=np.uint8)
    for i in range(PALETTE_SIZE):
        pal64[i] = pal256[i * 255 // (PALETTE_SIZE - 1)]
    return pal64, pal256


def quantize_frame(frame_256, pal64, pal256):
    mapping = np.zeros(256, dtype=np.uint8)
    pal64_f = pal64.astype(np.float32)
    for i in range(256):
        c = pal256[i].astype(np.float32)
        dists = np.sum((pal64_f - c) ** 2, axis=1)
        mapping[i] = np.argmin(dists)
    return mapping[frame_256]


def encode_rle_6bit(frame):
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


# ─── Rendering ───────────────────────────────────────────────────────────────

def render_sparse_layer(frame_idx, total_frames, noise1, noise2, noise3, rotation=0.0):
    """Render one sparse fire layer. Returns float array 0-1.
    rotation: rotate the coordinate system to spread seams across layers."""
    t = frame_idx / total_frames * 2.0 * math.pi
    time = frame_idx / total_frames * 10.0

    cx, cy = W / 2, H / 2

    yy, xx = np.mgrid[0:H, 0:W]
    dx = (xx - cx).astype(np.float32)
    dy = (yy - cy).astype(np.float32)

    # Rotate coordinates to spread seams across layers
    cos_r, sin_r = math.cos(rotation), math.sin(rotation)
    rdx = dx * cos_r - dy * sin_r
    rdy = dx * sin_r + dy * cos_r

    dist = np.sqrt(dx * dx + dy * dy)
    angle = np.arctan2(rdy, rdx)

    # --- Elongated oval shape (unrotated — same shape for all layers) ---
    oval_rx = 90.0 + 5.0 * math.sin(t * 0.8)
    oval_ry = 55.0 + 4.0 * math.sin(t * 0.6)
    oval_dist = (dx / oval_rx) ** 2 + (dy / oval_ry) ** 2

    # --- Rough edges using polar coords (seam hidden by rotation) ---
    polar_x = angle / (2 * math.pi) * 8.0
    polar_y = dist * 0.04 - time * 0.6

    edge_noise = fbm(noise1, polar_x * 2.0, np.full_like(polar_x, time * 0.5),
                     octaves=5, lacunarity=2.0, gain=0.5)
    edge_distort = (edge_noise - 0.5) * 1.0
    oval_dist_rough = oval_dist - edge_distort

    # --- Fire texture (polar, seam rotated away) ---
    fire1 = fbm(noise1, polar_x, polar_y, octaves=5, lacunarity=2.2, gain=0.45)
    fire2 = fbm(noise2, polar_x * 1.5 + 3.7, polar_y * 1.3 + time * 0.15,
                octaves=4, lacunarity=2.0, gain=0.5)
    fire3 = fbm(noise3, polar_x * 0.7 + 1.2, polar_y * 0.8 - time * 0.1,
                octaves=3, lacunarity=2.5, gain=0.4)
    fire = fire1 * 0.5 + fire2 * 0.3 + fire3 * 0.2

    # --- Combine ---
    interior = np.clip(1.0 - oval_dist_rough, 0, 1) ** 0.4
    escape = np.clip(edge_noise * 2.0 - 0.7, 0, 1) * np.clip(1.3 - oval_dist, 0, 1)

    # Extra-wispy: high threshold so most pixels are black
    wisp_noise = fbm(noise2, polar_x * 3.0 + 5.0, polar_y * 2.0 + time * 0.3,
                     octaves=4, lacunarity=2.3, gain=0.5)
    edge_factor = np.clip(oval_dist_rough * 0.8, 0, 1)
    wisp_threshold = 0.45 + edge_factor * 0.3  # very sparse — mostly black
    wisp_mask = np.clip((wisp_noise - wisp_threshold) / 0.25, 0, 1)

    combined = (interior + escape * 0.4) * (0.3 + 0.7 * fire) * wisp_mask

    # Outer glow
    glow = np.clip(1.2 - oval_dist_rough, 0, 1) ** 2 * 0.2
    combined = np.maximum(combined, glow)

    # Flickering
    flicker = 0.85 + 0.15 * math.sin(t * 4.7 + rotation * 3.0) * math.sin(t * 7.1 + rotation)
    combined *= flicker

    return np.clip(combined, 0, 1)


def render_sparse_pupil(frame_idx, total_frames, noise1, noise2, rotation=0.0):
    """Render one sparse dark pupil slit layer. Returns float 0-1 (1 = dark)."""
    t = frame_idx / total_frames * 2.0 * math.pi
    time = frame_idx / total_frames * 10.0

    cx, cy = W / 2, H / 2

    yy, xx = np.mgrid[0:H, 0:W]
    dx = (xx - cx).astype(np.float32)
    dy = (yy - cy).astype(np.float32)

    # Rotate for seam spreading
    cos_r, sin_r = math.cos(rotation), math.sin(rotation)
    rdx = dx * cos_r - dy * sin_r
    rdy = dx * sin_r + dy * cos_r
    angle = np.arctan2(rdy, rdx)
    polar_x = angle / (2 * math.pi) * 8.0

    # Vertical slit shape — narrow horizontal, shorter vertical
    slit_half_w = 7.0 + 2.0 * math.sin(t * 1.3)
    slit_half_h = 35.0 + 4.0 * math.sin(t * 0.7)
    slit_dist = (dx / slit_half_w) ** 2 + (dy / slit_half_h) ** 2

    # Solid black core — covers ~90% of the slit interior
    core = np.clip(1.0 - slit_dist * 1.1, 0, 1)  # solid where slit_dist < 0.9
    core = (core > 0.1).astype(np.float32)  # hard edge for the black center

    # Wispy fiery edges — only the outer ~10% ring of the slit
    edge_noise = fbm(noise1, polar_x * 2.5, np.full_like(polar_x, time * 0.4),
                     octaves=4, lacunarity=2.0, gain=0.5)
    # Narrow band around slit_dist ≈ 0.8 to 1.3
    edge_band = np.clip(1.0 - abs(slit_dist - 1.0) / 0.5, 0, 1)
    wisp = np.clip((edge_noise - 0.45) / 0.2, 0, 1) * edge_band

    return np.clip(core + wisp * 0.5, 0, 1)


def combined_to_palette(combined):
    """Map float 0-1 fire intensity to 256-color palette indices."""
    frame = np.zeros((H, W), dtype=np.uint8)

    mask1 = (combined >= 0.02) & (combined < 0.3)
    frame[mask1] = (1 + (combined[mask1] - 0.02) / 0.28 * 99).astype(np.uint8)

    mask2 = (combined >= 0.3) & (combined < 0.6)
    frame[mask2] = (100 + (combined[mask2] - 0.3) / 0.3 * 50).astype(np.uint8)

    mask3 = (combined >= 0.6) & (combined < 0.85)
    frame[mask3] = (150 + (combined[mask3] - 0.6) / 0.25 * 25).astype(np.uint8)

    mask4 = combined >= 0.85
    frame[mask4] = (170 + np.clip((combined[mask4] - 0.85) / 0.15, 0, 1) * 15).astype(np.uint8)

    return frame


def main():
    out_path = Path(__file__).parent.parent / "output" / "sauron_frames.bin"
    out_path.parent.mkdir(exist_ok=True)

    print(f"Generating {NUM_FRAMES} Sauron frames at {W}×{H} (layered sparse fire)...")
    pal64, pal256 = build_palette()

    # Layer config: each sparse fire layer has a different rotation, time offset,
    # and noise seeds. They fade in for FADE frames, burn, fade out for FADE frames.
    NUM_LAYERS = 12
    FADE = 10  # frames to fade in/out
    LAYER_LEN = NUM_FRAMES  # each layer runs the full duration

    # Stagger start times and rotations
    layers = []
    for L in range(NUM_LAYERS):
        offset = L * (NUM_FRAMES // NUM_LAYERS)  # staggered starts
        rotation = L * math.pi / NUM_LAYERS       # spread seams evenly
        seed_base = L * 100
        layers.append({
            'offset': offset,
            'rotation': rotation,
            'noise1': ValueNoise2D(seed=seed_base + 1),
            'noise2': ValueNoise2D(seed=seed_base + 2),
            'noise3': ValueNoise2D(seed=seed_base + 3),
        })

    print(f"  {NUM_LAYERS} fire layers, {FADE}-frame fade, "
          f"staggered by {NUM_FRAMES // NUM_LAYERS} frames")

    raw_frames_256 = []
    for frame_i in range(NUM_FRAMES):
        combined = np.zeros((H, W), dtype=np.float32)

        for layer in layers:
            local_frame = (frame_i - layer['offset']) % NUM_FRAMES

            if local_frame < FADE:
                fade = local_frame / FADE
            elif local_frame >= NUM_FRAMES - FADE:
                fade = (NUM_FRAMES - local_frame) / FADE
            else:
                fade = 1.0

            sparse = render_sparse_layer(
                local_frame, NUM_FRAMES,
                layer['noise1'], layer['noise2'], layer['noise3'],
                rotation=layer['rotation']
            )
            combined += sparse * fade / (NUM_LAYERS * 0.35)

        combined = np.clip(combined, 0, 1)
        raw_frames_256.append(combined_to_palette(combined))

        if (frame_i + 1) % 10 == 0:
            print(f"  Frame {frame_i + 1}/{NUM_FRAMES}")

    compressed_frames = []
    raw_total = 0
    for i in range(NUM_FRAMES):
        frame_64 = quantize_frame(raw_frames_256[i], pal64, pal256)
        compressed = encode_rle_6bit(frame_64)
        compressed_frames.append(compressed)
        raw_total += W * H

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

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(pal64.tobytes())
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
