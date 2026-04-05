#!/usr/bin/env python3
"""Pack multiple animation files into a single eyedata.bin for the flash partition.

Format:
  [4-byte magic: "EYES"]
  [uint16_t num_animations]
  [uint16_t reserved]
  [TOC entries: num_animations × (16-byte name + uint32_t offset + uint32_t size)]
  [animation data...]
"""

import struct
from pathlib import Path


def main():
    output_dir = Path(__file__).parent.parent / "output"
    out_path = output_dir / "eyedata.bin"

    # Find all *_frames.bin files
    anim_files = sorted(output_dir.glob("*_frames.bin"))
    if not anim_files:
        print("No *_frames.bin files found in output/")
        return

    print(f"Packing {len(anim_files)} animations:")

    # Read all animation data
    anims = []
    for f in anim_files:
        name = f.stem.replace("_frames", "")  # "blob_frames" → "blob"
        data = f.read_bytes()
        anims.append((name, data))
        print(f"  {name}: {len(data) / 1024:.1f} KB")

    # Build TOC
    num_anims = len(anims)
    toc_entry_size = 16 + 4 + 4  # name + offset + size
    header_size = 8  # magic + num_anims + reserved
    toc_size = num_anims * toc_entry_size
    data_start = header_size + toc_size

    with open(out_path, "wb") as f:
        # Header
        f.write(b"EYES")
        f.write(struct.pack("<HH", num_anims, 0))

        # TOC
        offset = data_start
        for name, data in anims:
            name_bytes = name.encode("utf-8")[:16].ljust(16, b"\x00")
            f.write(name_bytes)
            f.write(struct.pack("<II", offset, len(data)))
            offset += len(data)

        # Animation data
        for name, data in anims:
            f.write(data)

    total = header_size + toc_size + sum(len(d) for _, d in anims)
    print(f"\nWritten {out_path} ({total / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
