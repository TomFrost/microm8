#!/usr/bin/env python3
"""Saves a screenshot of a Tab5 running microm8, over its USB-C serial port, as a PNG.

Close `pio device monitor` first: only one program can have the port open.

    python3 tools/screenshot.py                  # writes screenshot.png
    python3 tools/screenshot.py landscape.png --port /dev/cu.usbmodem101

Needs only the Python standard library (macOS or Linux). The firmware side is
src/diag/screenshot.cpp, which also describes the format.
"""
import argparse
import base64
import glob
import os
import select
import struct
import sys
import termios
import time
import zlib

HEADER = b"MICROM8-SCREENSHOT "
FOOTER = b"MICROM8-SCREENSHOT-END "
TIMEOUT_S = 20


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("No Tab5 serial port found. Is it plugged in? Pass --port to name one.")
    if len(ports) > 1:
        print(f"Several serial ports; using {ports[0]} (pass --port to choose another)")
    return ports[0]


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag: raw
    attrs[1] = 0  # oflag: raw
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # no HUPCL: closing won't drop DTR
    attrs[3] = 0  # lflag: raw
    attrs[4] = attrs[5] = termios.B115200  # irrelevant over USB, but must be valid
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd


def read_capture(fd):
    """Asks for a screenshot and returns (width, height, rotation, encoded bytes, crc)."""
    os.write(fd, b"S")
    buf = b""
    header = None
    data_lines = []
    deadline = time.time() + TIMEOUT_S
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.5)
        if not ready:
            continue
        buf += os.read(fd, 65536)
        *lines, buf = buf.split(b"\n")
        for raw in lines:
            line = raw.strip()
            if header is None:
                if line.startswith(HEADER):
                    header = line[len(HEADER):].split()
            elif line.startswith(FOOTER):
                total, crc = line[len(FOOTER):].split()
                data = base64.b64decode(b"".join(data_lines))
                if len(data) != int(total):
                    sys.exit(f"Got {len(data)} bytes, expected {int(total)}: try again")
                version, width, height, rotation = (int(v) for v in header)
                if version != 1:
                    sys.exit(f"Unknown screenshot format version {version}")
                return width, height, rotation, data, int(crc, 16)
            elif line:
                data_lines.append(line)
    sys.exit("No screenshot arrived. Is microm8 running, and is the monitor closed?")


def rgb565_table():
    table = []
    for v in range(65536):
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        table.append(bytes(((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6)))
    return table


def decode(width, height, data):
    """Run-length (count, colour) pairs to RGB bytes, in the panel's native orientation."""
    table = rgb565_table()
    parts = []
    pixels = 0
    for count, colour in struct.iter_unpack("<HH", data):
        parts.append(table[colour] * count)
        pixels += count
    if pixels != width * height:
        sys.exit(f"Got {pixels} pixels, expected {width * height}: try again")
    return b"".join(parts)


def rotate(rgb, width, height, rotation):
    """Turns the native portrait image the way the UI is turned, as M5GFX maps it. Returns
    (rgb, width, height) as seen on the screen."""
    if rotation % 4 == 0:
        return rgb, width, height
    px = [rgb[i:i + 3] for i in range(0, len(rgb), 3)]
    if rotation % 4 == 2:
        return b"".join(reversed(px)), width, height
    out = []
    if rotation % 4 == 1:  # screen (x, y) is panel (height' - 1 - y, x)
        for y in range(width):
            nx = width - 1 - y
            out.extend(px[ny * width + nx] for ny in range(height))
    else:  # 3: screen (x, y) is panel (y, width' - 1 - x)
        for y in range(width):
            out.extend(px[ny * width + y] for ny in range(height - 1, -1, -1))
    return b"".join(out), height, width


def write_png(path, rgb, width, height):
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body)))
    stride = width * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("output", nargs="?", default="screenshot.png")
    parser.add_argument("--port", help="serial port (default: the first USB modem port)")
    args = parser.parse_args()

    port = args.port or find_port()
    try:
        fd = open_port(port)
    except OSError as e:
        sys.exit(f"Can't open {port}: {e.strerror}. Close any serial monitor using it.")
    try:
        width, height, rotation, data, crc = read_capture(fd)
    finally:
        os.close(fd)
    if zlib.crc32(data) != crc:
        print("Warning: checksum mismatch; the image may be damaged")
    rgb = decode(width, height, data)
    rgb, width, height = rotate(rgb, width, height, rotation)
    write_png(args.output, rgb, width, height)
    print(f"Saved {args.output} ({width}x{height})")


if __name__ == "__main__":
    main()
