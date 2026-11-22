#!/usr/bin/env python3
"""Write complete EV_KEY reports to an existing 64-bit Linux evdev device."""
import argparse
import os
import struct
import time

EVENT = struct.Struct("@llHHi")
KEYS = {"right": 106, "tab": 15, "enter": 28}


def send(device, key, count, pause, hold=0.06):
    key = KEYS[key]
    with open(device, "wb", buffering=0) as keyboard:
        def event(value):
            report = EVENT.pack(0, 0, 1, key, value) + EVENT.pack(0, 0, 0, 0, 0)
            if os.write(keyboard.fileno(), report) != len(report):
                raise OSError("short evdev write")

        for _ in range(count):
            event(1)
            try:
                time.sleep(hold)
            finally:
                event(0)
            time.sleep(hold)
            time.sleep(pause)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device")
    parser.add_argument("key", choices=KEYS)
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--pause", type=float, default=0)
    parser.add_argument("--hold", type=float, default=0.06)
    args = parser.parse_args()
    if args.count < 1 or min(args.pause, args.hold) < 0:
        parser.error("count must be positive and timings must be nonnegative")
    send(args.device, args.key, args.count, args.pause, args.hold)


if __name__ == "__main__":
    main()
