#!/usr/bin/env python3
"""Talks to microm8 over USB passthrough, while the Tab5's USB-C presents the M8.

    python3 tools/passthru.py stats [seconds]   audio counters (see Stats in src/usb/passthru.cpp),
                                                once a second for that long
    python3 tools/passthru.py serial            give USB-C back to the Tab5's serial port until the
                                                M8 reconnects or the Tab5 restarts: flash with
                                                pio run -t upload, or read the log or a screenshot

Uses vendor requests on the device's control endpoint. Needs libusb (brew install libusb).
"""
import ctypes
import ctypes.util
import struct
import sys
import time

FIELDS = ("alt_sets isr_calls isr_failed packets empty_packets frames_sent short_packets "
          "frames_pushed frames_dropped level alt streaming primed").split()
VENDOR_IN, VENDOR_OUT = 0xC0, 0x40
REQ_STATS, REQ_SERIAL = 0x01, 0x02
ESPRESSIF_USJ = (0x303A, 0x1001)


def open_m8():
    lib = None
    for path in (ctypes.util.find_library("usb-1.0"), "/opt/homebrew/lib/libusb-1.0.dylib",
                 "/usr/local/lib/libusb-1.0.dylib"):
        if path:
            try:
                lib = ctypes.CDLL(path)
                break
            except OSError:
                pass
    if not lib:
        sys.exit("libusb not found (brew install libusb)")
    lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
    lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16,
                                                    ctypes.c_uint16]
    lib.libusb_control_transfer.argtypes = [ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8,
                                            ctypes.c_uint16, ctypes.c_uint16, ctypes.c_char_p,
                                            ctypes.c_uint16, ctypes.c_uint]
    lib.libusb_close.argtypes = [ctypes.c_void_p]
    if lib.libusb_init(None) != 0:
        sys.exit("libusb_init failed")
    handle = lib.libusb_open_device_with_vid_pid(None, 0x16C0, 0x048A)
    if not handle:
        sys.exit("no M8 (16C0:048A) on USB; is the M8 connected to the Tab5?")
    return lib, handle


def stats(lib, handle, seconds):
    end = time.time() + seconds
    while True:
        buf = ctypes.create_string_buffer(4 * len(FIELDS))
        n = lib.libusb_control_transfer(handle, VENDOR_IN, REQ_STATS, 0, 0, buf, len(buf), 1000)
        if n < 0:
            sys.exit(f"request failed ({n}); is this a real M8 rather than a Tab5?")
        values = struct.unpack(f"<{n // 4}I", buf.raw[:n])
        print("  ".join(f"{k}={v}" for k, v in zip(FIELDS, values)), flush=True)
        if time.time() >= end:
            return
        time.sleep(1)


def serial(lib, handle):
    n = lib.libusb_control_transfer(handle, VENDOR_OUT, REQ_SERIAL, 0, 0, None, 0, 1000)
    if n < 0:
        sys.exit(f"request failed ({n}); is this a real M8 rather than a Tab5?")
    lib.libusb_close(handle)
    # The M8 leaves USB-C, then the Tab5's Serial/JTAG port appears.
    deadline = time.time() + 10
    while time.time() < deadline:
        time.sleep(0.5)
        usj = lib.libusb_open_device_with_vid_pid(None, *ESPRESSIF_USJ)
        if usj:
            lib.libusb_close(usj)
            print("USB-C is the Tab5's serial port until the M8 reconnects or the Tab5 restarts.")
            print("Flash with: pio run -t upload")
            return
    sys.exit("the Tab5's serial port didn't appear")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd not in ("stats", "serial"):
        sys.exit(__doc__)
    lib, handle = open_m8()
    if cmd == "serial":
        serial(lib, handle)
        return
    try:
        stats(lib, handle, float(sys.argv[2]) if len(sys.argv) > 2 else 0)
    finally:
        lib.libusb_close(handle)


if __name__ == "__main__":
    main()
