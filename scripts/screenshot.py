#!/usr/bin/env python3
"""Capture the display over USB serial with the firmware's `screenshot` command and save it as a PNG.

The firmware redraws the whole screen and prints it as hex rows ("SS BEGIN w h", "SS <row> <hex>", "SS END"),
one RGB565 pixel per 4 hex digits in the panel's byte order (swapped). The saved PNG is cropped to the round
display, with a transparent background outside the circle.

Pages (optional): rank, graph, prices, watch, leagues (between gameweeks); events, squad, squadend (live).
Without a page it captures whatever is on screen.

Usage:
  pip install pillow pyserial
  python scripts/screenshot.py /dev/cu.usbmodem1101 out.png
  python scripts/screenshot.py /dev/cu.usbmodem1101 graph.png graph
"""

import argparse
import sys
import time

import serial
from PIL import Image, ImageDraw


class CaptureError(Exception):
    pass


SUPERSAMPLE = 4  # for a smooth circle edge


def capture(port: str, page: str | None, timeout_s: float) -> Image.Image:
    with serial.Serial(port, 115200, timeout=0.5) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(f"screenshot {page or ''}".strip().encode() + b"\n")

        width = height = 0
        rows: dict[int, bytes] = {}
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            line = ser.readline().decode("ascii", errors="replace").strip()
            if not line.startswith("SS "):
                continue
            parts = line.split(" ")
            if parts[1] == "BEGIN":
                width, height = int(parts[2]), int(parts[3])
                rows.clear()
            elif parts[1] == "END":
                break
            elif parts[1] == "ERROR":
                raise CaptureError(f"device: {line}")
            elif width and len(parts) == 3 and len(parts[2]) == width * 4:
                rows[int(parts[1])] = bytes.fromhex(parts[2])
        else:
            raise CaptureError("timed out waiting for the screenshot")

    missing = [y for y in range(height) if y not in rows]
    if missing:
        raise CaptureError(f"{len(missing)} rows missing or garbled (first: {missing[0]})")

    img = Image.new("RGB", (width, height))
    pixels = img.load()
    for y in range(height):
        row = rows[y]
        for x in range(width):
            # The hex is the 16-bit value as LVGL stored it: RGB565 with its bytes swapped.
            v = (row[x * 2] << 8) | row[x * 2 + 1]
            v = ((v & 0xFF) << 8) | (v >> 8)
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            pixels[x, y] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    return img


def round_crop(img: Image.Image) -> Image.Image:
    w, h = img.size
    mask = Image.new("L", (w * SUPERSAMPLE, h * SUPERSAMPLE), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, w * SUPERSAMPLE - 1, h * SUPERSAMPLE - 1), fill=255)
    out = img.convert("RGBA")
    out.putalpha(mask.resize((w, h), Image.LANCZOS))
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="serial port, e.g. /dev/cu.usbmodem1101")
    parser.add_argument("out", help="output PNG")
    parser.add_argument("page", nargs="?", help="page to show first (see above)")
    parser.add_argument("--square", action="store_true", help="keep the full square frame")
    parser.add_argument("--timeout", type=float, default=30.0, help="seconds to wait (default 30)")
    parser.add_argument("--retries", type=int, default=3, help="attempts before giving up (default 3)")
    args = parser.parse_args()

    # Log lines from other tasks can land in the middle of a row, so a capture sometimes needs another go.
    for attempt in range(1, args.retries + 1):
        try:
            img = capture(args.port, args.page, args.timeout)
            break
        except CaptureError as e:
            print(f"attempt {attempt}: {e}", file=sys.stderr)
    else:
        sys.exit("giving up")
    (img if args.square else round_crop(img)).save(args.out, optimize=True)
    print(f"saved {args.out}")


if __name__ == "__main__":
    main()
