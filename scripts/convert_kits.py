#!/usr/bin/env python3
"""Convert kit PNGs into the raw RGB565 files the firmware loads from LittleFS.

Output format (matches src/main.cpp loadKitImage):
  data/kits/<team_slug>_<type>_110x145.rgb565
  110x145 pixels, 2 bytes per pixel, little-endian RGB565, no header.
  Transparent pixels are filled with the popup background (kColorBgDeep, 0x1A0533).

Source PNGs are named after the team (e.g. "man-united.png"). The file name is
turned into the firmware's kit slug: lower case, non-alphanumerics -> "_", then
KIT_SLUG_ALIASES is applied. Keep the slugs in sync with normalizeKitTeamSlug().

Usage:
  pip install pillow
  python scripts/convert_kits.py 26-27-kits
  python scripts/convert_kits.py 26-27-kits --type gk --preview /tmp/kit_preview
"""

import argparse
import re
import struct
import sys
from pathlib import Path

from PIL import Image

KIT_WIDTH = 110
KIT_HEIGHT = 145
BACKGROUND_RGB = (0x1A, 0x05, 0x33)  # kColorBgDeep in src/main.cpp

# Source file stem (after slugify) -> kit slug used by the firmware.
KIT_SLUG_ALIASES = {
    "man_united": "man_utd",
    "manchester_united": "man_utd",
    "manchester_city": "man_city",
    "spurs": "tottenham",
    "tottenham_hotspur": "tottenham",
    "coventry_city": "coventry",
    "hull_city": "hull",
    "ipswich_town": "ipswich",
    "afc_bournemouth": "bournemouth",
    "brighton_and_hove_albion": "brighton",
    "newcastle_united": "newcastle",
    "nottm_forest": "nottingham_forest",
    "nott_m_forest": "nottingham_forest",
    "west_ham_united": "west_ham",
    "wolverhampton_wanderers": "wolves",
}


def kit_slug(stem: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "_", stem.lower()).strip("_")
    return KIT_SLUG_ALIASES.get(slug, slug)


def render_kit(src: Path) -> Image.Image:
    """Composite onto the background and fit into KIT_WIDTH x KIT_HEIGHT, centered."""
    img = Image.open(src).convert("RGBA")
    flat = Image.new("RGBA", img.size, BACKGROUND_RGB + (255,))
    flat.alpha_composite(img)
    flat = flat.convert("RGB")

    scale = min(KIT_WIDTH / flat.width, KIT_HEIGHT / flat.height)
    size = (max(1, round(flat.width * scale)), max(1, round(flat.height * scale)))
    flat = flat.resize(size, Image.LANCZOS)

    canvas = Image.new("RGB", (KIT_WIDTH, KIT_HEIGHT), BACKGROUND_RGB)
    canvas.paste(flat, ((KIT_WIDTH - size[0]) // 2, (KIT_HEIGHT - size[1]) // 2))
    return canvas


def to_rgb565_le(img: Image.Image) -> bytes:
    raw = img.tobytes()
    out = bytearray()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("src_dir", type=Path, help="folder of team PNGs")
    parser.add_argument("--out", type=Path, default=Path("data/kits"), help="output folder (default: data/kits)")
    parser.add_argument("--type", default="outfield", choices=["outfield", "gk"], help="kit type suffix")
    parser.add_argument("--preview", type=Path, help="also write 110x145 PNG previews here")
    args = parser.parse_args()

    sources = sorted(args.src_dir.glob("*.png"))
    if not sources:
        print(f"no PNGs in {args.src_dir}", file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    if args.preview:
        args.preview.mkdir(parents=True, exist_ok=True)

    expected = KIT_WIDTH * KIT_HEIGHT * 2
    for src in sources:
        slug = kit_slug(src.stem)
        kit = render_kit(src)
        data = to_rgb565_le(kit)
        assert len(data) == expected
        dest = args.out / f"{slug}_{args.type}_{KIT_WIDTH}x{KIT_HEIGHT}.rgb565"
        dest.write_bytes(data)
        if args.preview:
            kit.save(args.preview / f"{dest.stem}.png")
        print(f"{src.name:28} -> {dest}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
