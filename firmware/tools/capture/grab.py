#!/usr/bin/env python3
"""Pull glare-free device screenshots over USB-CDC and save them as PNGs.

The capture firmware (env:capture) streams the real RGB565 panel framebuffer for every theme x screen.
This sends 'C' to start the sweep, then decodes each FRAME into a PNG.

Wire protocol (ASCII headers, raw little-endian RGB565 payload):
    BEACONCAP START <n>
    FRAME <theme> <screen> <w> <h>\n  <w*h*2 bytes>  \nENDFRAME\n
    ... (n frames) ...
    BEACONCAP DONE

Usage:
    pip install pyserial pillow numpy
    python3 grab.py --port /dev/cu.usbmodemXXXX --out shots/
    python3 grab.py --port /dev/cu.usbmodemXXXX --out shots/ --montage
"""
import argparse
import os
import re
import sys
import time

import numpy as np
import serial
from PIL import Image


def rgb565_arr_to_image(px, w: int, h: int) -> Image.Image:
    px = px.astype(np.uint32)
    r = ((px >> 11) & 0x1F)
    g = ((px >> 5) & 0x3F)
    b = (px & 0x1F)
    # expand to 8-bit replicating the high bits into the low (standard 5/6-bit upscale)
    r = (r << 3) | (r >> 2)
    g = (g << 2) | (g >> 4)
    b = (b << 3) | (b >> 2)
    rgb = np.stack([r, g, b], axis=-1).astype(np.uint8).reshape(h, w, 3)
    return Image.fromarray(rgb)


def rle_to_image(data: bytes, w: int, h: int) -> Image.Image:
    """Decode firmware RLE16: [uint16 count][uint16 color] runs, little-endian."""
    a = np.frombuffer(data, dtype="<u2")
    counts = a[0::2]
    colors = a[1::2]
    px = np.repeat(colors, counts)
    if px.size != w * h:
        raise ValueError(f"rle expands to {px.size}px, expected {w * h}")
    return rgb565_arr_to_image(px, w, h)


FRAME_RE = re.compile(rb"\nFRAME (\w+) (\w+) (\d+) (\d+) (\d+)\n")


def read_line(ser):
    """Next line without EOL, or None on read timeout. Empty bytes => a blank separator line."""
    line = bytearray()
    while True:
        c = ser.read(1)
        if not c:
            return None
        if c == b"\n":
            return bytes(line)
        if c != b"\r":
            line += c


def read_exact(ser, n: int):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            return None  # timeout
        buf += chunk
    return bytes(buf)


def live_sweep(ser, out, frames):
    """Read each RLE frame and save it. The stream is small (RLE), so no flow control is needed."""
    expected = None
    started = False
    retries = 0
    ser.write(b"C")
    ser.flush()
    while True:
        line = read_line(ser)
        if line is None:
            if not started and retries < 15:
                retries += 1   # 'C' can be dropped during the reset-on-open boot; resend until it answers
                ser.reset_input_buffer()
                ser.write(b"C")
                ser.flush()
                continue
            print("no response from device" if not started else "stalled mid-sweep", file=sys.stderr)
            break
        if line == b"":
            continue
        text = line.decode("ascii", "replace").strip()
        if text.startswith("BEACONCAP START"):
            started = True
            expected = int(text.split()[-1])
            print(f"expecting {expected} frames")
            continue
        if text.startswith("BEACONCAP DONE"):
            break
        if not text.startswith("FRAME "):
            continue
        _, theme, screen, w, h, clen = text.split()
        w, h, clen = int(w), int(h), int(clen)
        payload = read_exact(ser, clen)
        if payload is None:
            print(f"warn: timed out reading {theme}/{screen}", file=sys.stderr)
            break
        end = read_line(ser)
        while end == b"":
            end = read_line(ser)
        if end is None or b"ENDFRAME" not in end:
            print(f"warn: misaligned frame {theme}/{screen}", file=sys.stderr)
        path = os.path.join(out, f"{theme}_{screen}.png")
        img = rle_to_image(payload, w, h)
        img.save(path)
        frames[(theme, screen)] = img
        print(f"saved {path}")
    return expected


def parse_frames(buf: bytes):
    """Yield (theme, screen, PIL.Image) by scanning a recorded buffer for FRAME headers + RLE payloads."""
    pos = 0
    while True:
        m = FRAME_RE.search(buf, pos)
        if not m:
            break
        theme = m.group(1).decode()
        screen = m.group(2).decode()
        w, h, clen = int(m.group(3)), int(m.group(4)), int(m.group(5))
        start = m.end()
        payload = buf[start:start + clen]
        if len(payload) < clen:
            print(f"warn: truncated frame {theme}/{screen} ({len(payload)}/{clen})", file=sys.stderr)
            break
        if b"ENDFRAME" not in buf[start + clen:start + clen + 12]:
            print(f"warn: misaligned frame {theme}/{screen} (no ENDFRAME)", file=sys.stderr)
        yield theme, screen, rle_to_image(payload, w, h)
        pos = start + clen


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port, e.g. /dev/cu.usbmodem1101")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="shots", help="output directory for PNGs")
    ap.add_argument("--montage", action="store_true", help="also write a theme x screen contact sheet")
    ap.add_argument("--from-file", help="decode a previously recorded raw sweep instead of reading serial")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    frames = {}  # (theme, screen) -> Image
    expected = None
    if args.from_file:
        buf = open(args.from_file, "rb").read()
        print(f"decoding {len(buf)} bytes from {args.from_file}")
        m = re.search(rb"BEACONCAP START (\d+)", buf)
        if m:
            expected = int(m.group(1))
            print(f"expecting {expected} frames")
        for theme, screen, img in parse_frames(buf):
            path = os.path.join(args.out, f"{theme}_{screen}.png")
            img.save(path)
            frames[(theme, screen)] = img
            print(f"saved {path}")
    else:
        if not args.port:
            print("need --port or --from-file", file=sys.stderr)
            return 2
        ser = serial.Serial(args.port, args.baud, timeout=6)   # generous: device may render ~400ms+ between frames
        time.sleep(1.0)                # let a reset-on-open boot settle
        ser.reset_input_buffer()
        print("sweep requested...")
        expected = live_sweep(ser, args.out, frames)
        ser.close()

    if args.montage and frames:
        themes = sorted({t for t, _ in frames})
        screens = list(dict.fromkeys(s for _, s in frames))   # capture order (carousel): HOME, MARKETS, LIMITS, CLAUDE, SETTINGS
        w, h = next(iter(frames.values())).size
        pad = 8
        sheet = Image.new("RGB", (len(screens) * (w + pad) + pad, len(themes) * (h + pad) + pad), "black")
        for ti, t in enumerate(themes):
            for si, s in enumerate(screens):
                im = frames.get((t, s))
                if im:
                    sheet.paste(im, (pad + si * (w + pad), pad + ti * (h + pad)))
        mpath = os.path.join(args.out, "montage.png")
        sheet.save(mpath)
        print(f"saved {mpath}")

    print(f"done: {len(frames)} frames" + (f" (expected {expected})" if expected else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
