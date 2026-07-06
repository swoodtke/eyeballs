#!/usr/bin/env python3
"""Preview the blob_frames.bin animation in a window (supports 6-bit RLE format)."""

import struct
import sys
import numpy as np
from pathlib import Path

import tkinter as tk
from PIL import Image, ImageTk


def decode_rle_6bit(data, num_pixels):
    """Decode a 6-bit RLE bitstream into an array of palette indices."""
    pixels = np.zeros(num_pixels, dtype=np.uint8)
    # Unpack all bits
    bits = []
    for byte in data:
        for bit in range(7, -1, -1):
            bits.append((byte >> bit) & 1)

    pos = 0
    px = 0

    def read6():
        nonlocal pos
        val = 0
        for _ in range(6):
            if pos < len(bits):
                val = (val << 1) | bits[pos]
                pos += 1
        return val

    while px < num_pixels:
        v = read6()
        if v != 0:
            pixels[px] = v
            px += 1
        else:
            color = read6()
            count = read6()
            if count == 0:
                count = 1
            end = min(px + count, num_pixels)
            pixels[px:end] = color
            px = end

    return pixels


def load_blob_frames(path):
    with open(path, "rb") as f:
        data = f.read()

    w, h, num_frames, pal_size = struct.unpack_from("<HHHH", data, 0)
    bpp = data[8]
    playback = data[9]  # 0=ping-pong, 1=loop

    pal_offset = 16
    pal = np.frombuffer(data, dtype=np.uint8, count=pal_size * 3, offset=pal_offset).reshape(pal_size, 3)

    offset_table_start = pal_offset + pal_size * 3
    offsets = []
    for i in range(num_frames):
        off = struct.unpack_from("<I", data, offset_table_start + i * 4)[0]
        offsets.append(off)

    num_pixels = w * h
    frames = np.zeros((num_frames, h, w), dtype=np.uint8)

    for i in range(num_frames):
        start = offsets[i]
        end = offsets[i + 1] if i + 1 < num_frames else len(data)
        frame_data = data[start:end]
        pixels = decode_rle_6bit(frame_data, num_pixels)
        frames[i] = pixels.reshape(h, w)

    print(f"Loaded {num_frames} frames at {w}×{h}, {pal_size} colors, {bpp}-bit RLE")
    return w, h, num_frames, pal, frames, playback


def main():
    if len(sys.argv) > 1:
        bin_path = Path(sys.argv[1])
    else:
        bin_path = Path(__file__).parent.parent / "output" / "blob_frames.bin"
    if not bin_path.exists():
        print(f"Not found: {bin_path}")
        print("Usage: preview_blob.py [path_to_frames.bin]")
        sys.exit(1)

    w, h, num_frames, pal, frames, playback = load_blob_frames(bin_path)

    scale = 2
    disp_w, disp_h = w * scale, h * scale

    root = tk.Tk()
    root.title(f"Blob Eye Preview — {num_frames} frames, 6-bit RLE")
    root.resizable(False, False)

    canvas = tk.Canvas(root, width=disp_w, height=disp_h, bg="black", highlightthickness=0)
    canvas.pack()

    frame_idx = [0]
    direction = [1]
    pingpong = [True]  # True=bounce, False=loop
    paused = [False]
    tk_img = [None]

    def update():
        idx = frames[frame_idx[0]]
        rgb = pal[idx]
        img = Image.fromarray(rgb, "RGB").resize((disp_w, disp_h), Image.NEAREST)
        tk_img[0] = ImageTk.PhotoImage(img)
        canvas.create_image(0, 0, anchor=tk.NW, image=tk_img[0])

        if not paused[0]:
            if pingpong[0]:
                frame_idx[0] += direction[0]
                if frame_idx[0] >= num_frames - 1:
                    direction[0] = -1
                elif frame_idx[0] <= 0:
                    direction[0] = 1
            else:
                frame_idx[0] = (frame_idx[0] + 1) % num_frames

        root.after(33, update)

    def on_key(event):
        if event.keysym == "space":
            paused[0] = not paused[0]
        elif event.keysym == "Right":
            frame_idx[0] = min(frame_idx[0] + 1, num_frames - 1)
        elif event.keysym == "Left":
            frame_idx[0] = max(frame_idx[0] - 1, 0)
        elif event.keysym == "l":
            pingpong[0] = not pingpong[0]
            print(f"Playback: {'ping-pong' if pingpong[0] else 'loop'}")
        elif event.keysym in ("q", "Escape"):
            root.destroy()

    root.bind("<Key>", on_key)
    # Playback mode comes from the header (0=ping-pong, 1=loop)
    if playback == 1:
        pingpong[0] = False
    print(f"Controls: Space=pause, Left/Right=step, L=toggle loop/pingpong, Q=quit")
    print(f"Playback: {'ping-pong' if pingpong[0] else 'loop'}")
    update()
    root.mainloop()


if __name__ == "__main__":
    main()
