#!/usr/bin/env python3
"""Collect CardputerZero carousel and locked-idle measurements on Linux."""
import argparse
from contextlib import contextmanager
import fcntl
import gzip
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import sys
import time

from keygen import EVENT, KEYS

FB_SIZE = 320 * 170 * 2
TRACE_ROOT = Path("/sys/kernel/tracing")
SPI_EVENTS = ("spi/spi_transfer_start", "spi/spi_transfer_stop")
WAKE_EVENTS = ("sched/sched_wakeup", "sched/sched_wakeup_new")
SPI_LINE = re.compile(
    r"\s(\d+\.\d+: spi_transfer_(start|stop): spi\d+\.\d+ \w+ "
    r"len=(\d+)) tx=\[([0-9a-f-]*)\](?: rx=.*)?$"
)


def spi_only(raw):
    # Retain timing, transfer IDs, lengths and CASET/PASET/RAMWR metadata.
    # Strip task information and pixel bytes, which the decoder does not use.
    expecting_coordinates = False
    for line in raw.splitlines():
        match = SPI_LINE.search(line)
        if not match:
            continue
        prefix, event, length, tx = match.groups()
        payload = ""
        if event == "start":
            if length == "1" and tx in ("2a", "2b", "2c"):
                payload = tx
                expecting_coordinates = tx in ("2a", "2b")
            elif expecting_coordinates and length == "4":
                payload = tx
                expecting_coordinates = False
        yield f" {prefix} tx=[{payload}]\n"


@contextmanager
def tracing(events, spi=False):
    instance = TRACE_ROOT / "instances" / f"applaunch-measure-{os.getpid()}"
    instance.mkdir()
    try:
        (instance / "tracing_on").write_text("0")
        (instance / "trace_clock").write_text("mono")
        (instance / "buffer_size_kb").write_text("2048")
        (instance / "trace").write_text("\n")
        for name in events:
            event = instance / "events" / name
            if spi:
                (event / "filter").write_text("bus_num == 0 && chip_select == 0")
            (event / "enable").write_text("1")
        (instance / "tracing_on").write_text("1")
        yield instance
    finally:
        (instance / "tracing_on").write_text("0")
        (instance / "events/enable").write_text("0")
        instance.rmdir()


def finish(instance):
    (instance / "tracing_on").write_text("0")
    stats = [path.read_text() for path in instance.glob("per_cpu/cpu*/stats")]
    if not stats or any(re.search(r"(?:overrun|dropped events):\s*[1-9]", value) for value in stats):
        raise RuntimeError("trace lost events")
    return (instance / "trace").read_text()


def collect_carousel(args):
    dense = args.mode == "dense"
    count = 64 if dense else 24
    presses, samples = [], []
    pending = None
    with open(args.evdev, "rb", buffering=0) as keyboard, open(args.framebuffer, "rb", buffering=0) as fb:
        fd = keyboard.fileno()
        os.set_blocking(fd, False)
        # EVIOCSCLOCKID: the evdev timestamps share ftrace's monotonic clock.
        fcntl.ioctl(fd, 0x400445A0, struct.pack("i", time.CLOCK_MONOTONIC))
        old = os.pread(fb.fileno(), FB_SIZE, 0) if not dense else b""
        with tracing(SPI_EVENTS, spi=True) as instance, subprocess.Popen([
            sys.executable, str(Path(__file__).with_name("keygen.py")), args.evdev, "right",
            "--count", str(count), "--pause", "0" if dense else "0.53",
        ]) as keygen:
            deadline = time.monotonic() + (12 if dense else 20)
            while time.monotonic() < deadline:
                readable, _, _ = select.select([fd], [], [], 0.05 if dense else 0.002)
                if readable:
                    for sec, usec, kind, key, value in EVENT.iter_unpack(os.read(fd, EVENT.size * 64)):
                        if (kind, key, value) == (1, KEYS["right"], 1):
                            stamp = sec * 1_000_000_000 + usec * 1000
                            presses.append(stamp)
                            if not dense:
                                pending = {"t": stamp, "first_any_ms": None, "content_ms": None}
                                samples.append(pending)
                                base = old
                if not dense:
                    pixels = os.pread(fb.fileno(), FB_SIZE, 0)
                    now = time.monotonic_ns()
                    if len(pixels) != FB_SIZE:
                        raise RuntimeError("incomplete framebuffer read")
                    if pending is not None:
                        elapsed = (now - pending["t"]) / 1e6
                        if pixels != base and pending["first_any_ms"] is None:
                            pending["first_any_ms"] = elapsed
                        if pixels[38*640:140*640] != base[38*640:140*640] or elapsed > 400:
                            if elapsed <= 400:
                                pending["content_ms"] = elapsed
                            pending = None
                    old = pixels
                if keygen.poll() is not None:
                    break
            if keygen.poll() is None:
                keygen.terminate()
            if keygen.wait() or len(presses) != count:
                raise RuntimeError(f"expected {count} presses, recorded {len(presses)}")
            time.sleep(0.3)
            raw = finish(instance)
    return {"presses": presses, "samples": samples}, raw


def collect_idle(args):
    def brightness():
        return int(Path(args.brightness).read_text())

    def battery(field):
        return (Path(args.battery) / field).read_text().strip()

    if brightness() != 0 or battery("status") != "Discharging":
        raise RuntimeError("lock the launcher, wait for a dark backlight, and run on battery")
    tids = {int(p.name) for p in (Path("/proc") / str(args.pid) / "task").iterdir()}
    samples = []
    with tracing(WAKE_EVENTS) as instance:
        started = time.monotonic()
        for second in range(1, 26):
            samples.append([int(battery(field)) for field in ("current_now", "voltage_now")])
            time.sleep(max(0, started + second - time.monotonic()))
            if brightness() != 0 or battery("status") != "Discharging":
                raise RuntimeError("brightness or battery state changed")
        raw = finish(instance)
        seconds = time.monotonic() - started
    counts = re.findall(r"sched_wakeup(?:_new)?:.*? pid=(\d+) ", raw)
    return {
        "seconds": seconds,
        "current_voltage": samples,
        "launcher_wakeups_s": sum(int(pid) in tids for pid in counts) / seconds,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("dense", "latency", "idle"))
    parser.add_argument("output", type=Path, help="output filename prefix")
    parser.add_argument("--evdev", help="Cardputer keyboard device used by APPLaunch")
    parser.add_argument("--framebuffer", default="/dev/fb0")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--brightness", default="/sys/class/backlight/backlight/actual_brightness")
    parser.add_argument("--battery", default="/sys/class/power_supply/bq27220-0")
    args = parser.parse_args()
    if args.mode == "idle":
        if not args.pid:
            parser.error("idle requires --pid")
        result = collect_idle(args)
    else:
        if not args.evdev:
            parser.error("dense and latency require --evdev")
        result, raw = collect_carousel(args)
        with gzip.open(str(args.output) + ".trace.gz", "wt") as data:
            data.writelines(spi_only(raw))
    Path(str(args.output) + ".json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
