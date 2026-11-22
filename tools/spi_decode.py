#!/usr/bin/env python3
"""Decode ST7789 CASET/PASET/RAMWR and summarize the runs."""
from collections import Counter
import gzip
import json
from pathlib import Path
import re
from statistics import median, quantiles

HERE = Path(__file__).resolve().parent / "traces"
# The ST7789 has 240 addressable rows and this panel exposes the centered
# 170-row window. PASET y=35 is fb0 y=0; add 35 to framebuffer coordinates
# before comparing with the decoded wire window, or subtract it for reporting.
PANEL_Y_OFFSET = 35
LINE = re.compile(r"\s(\d+\.\d+): (\w+): (.*)$")
SPI = re.compile(r"spi[0-9]+\.[0-9]+ (\w+) len=(\d+) tx=\[([0-9a-f-]*)\]")


def trace(path):
    pending = None
    axis = None
    x = y = None
    ram = False
    frames = []
    with gzip.open(path, "rt") as data:
        for line in data:
            m = LINE.search(line)
            if not m:
                continue
            t, event, detail = m.groups()
            t = float(t)
            if not event.startswith("spi_transfer_"):
                continue
            m = SPI.fullmatch(detail.split(" rx=")[0])
            if not m:
                raise ValueError(line)
            ident, size, tx = m.groups()
            size = int(size)
            if event.endswith("stop"):
                if pending is None or pending[:2] != (ident, size):
                    raise ValueError(f"Unpaired stop: {line}")
                if pending[2]:
                    f = pending[2]
                    f["bytes"] += size
                    f["wire_ms"] += (t - pending[3]) * 1000
                    f["done"] = t
                    if f["bytes"] == f["expected"]:
                        frames.append(f)
                        ram = False
                        f = None
                    if f and f["bytes"] > f["expected"]:
                        raise ValueError("SPI payload is larger than CASET/PASET window")
                    window = f
                pending = None
                continue
            if pending is not None:
                raise ValueError(f"Unpaired start: {line}")
            f = None
            if ram:
                f = window
                if not f["bytes"]:
                    f["start"] = t
            elif size == 1:
                if tx in ("2a", "2b"):
                    axis = tx
                elif tx == "2c" and x and y:
                    expected = (x[1] - x[0] + 1) * (y[1] - y[0] + 1) * 2
                    ram = True
                    window = dict(x=x, y=y, command=t, bytes=0, wire_ms=0.0, expected=expected)
            elif size == 4 and axis:
                raw = bytes.fromhex(tx.replace("-", ""))
                coords = (int.from_bytes(raw[:2], "big"), int.from_bytes(raw[2:], "big"))
                if axis == "2a":
                    x = coords
                else:
                    y = coords
                axis = None
            pending = (ident, size, f, t)
    return frames


def percentiles(values):
    if not values:
        return None
    return [round(median(values), 3),
            round(quantiles(values, n=20, method="inclusive")[-1], 3) if len(values) >= 2 else values[0]]


def summarize(label, directory):
    result = {}
    collected = []
    wide_collected = []
    for p in sorted(directory.glob(f"{label}-*.json")):
        run = re.fullmatch(r"\d{2}-(\w+)", p.stem[len(label) + 1:])
        if not run:
            continue
        mode = run[1]
        data = json.loads(p.read_text(encoding="utf-8"))
        if mode in ("idle", "lit"):
            result.setdefault(f"{mode}_launcher_wakeups_s", []).append(data["launcher_wakeups_s"])
            power = data["current_voltage"]
            if len(power) == round(data["seconds"]):
                result.setdefault(f"{mode}_battery_mw", []).append(median(abs(i * u) / 1e9 for i, u in power))
        elif mode in ("dense", "latency"):
            frames = trace(p.with_suffix(".trace.gz"))
            presses = [t / 1e9 for t in data["presses"]]
            if mode == "dense":
                selected = [f for f in frames if presses[0] <= f["start"] < presses[-1]]
                carousel = [f for f in selected if f["y"][0] <= PANEL_Y_OFFSET + 140 and f["y"][1] >= PANEL_Y_OFFSET + 38]
                duration = presses[-1] - presses[0]
                result.setdefault("updates_s", []).append(len(selected) / duration)
                result.setdefault("carousel_updates_s", []).append(len(carousel) / duration)
                complete = [f for f in carousel if f["x"] == (0, 319)
                            and f["y"][0] <= PANEL_Y_OFFSET + 49 and f["y"][1] >= PANEL_Y_OFFSET + 129]
                result.setdefault("wide_carousel_updates_s", []).append(len(complete) / duration)
                result.setdefault("wide_carousel_spi_ms_by_run", []).append(percentiles([f["wire_ms"] for f in complete]))
                collected.extend(carousel)
                wide_collected.extend(complete)
            else:
                for key in ("first_any_ms", "content_ms"):
                    result.setdefault(key, []).extend(s[key] for s in data["samples"] if s[key] is not None)
                lat = []
                for press in presses:
                    next_fb = next((f for f in frames if f["start"] >= press
                                    and f["y"][1] >= PANEL_Y_OFFSET + 38 and f["y"][0] <= PANEL_Y_OFFSET + 140), None)
                    if next_fb and next_fb["start"] - press <= 0.4:
                        lat.append((next_fb["start"] - press) * 1000)
                result.setdefault("first_spi_ms", []).extend(lat)
    result["spi_ms"] = percentiles([f["wire_ms"] for f in collected])
    result["wide_spi_ms"] = percentiles([f["wire_ms"] for f in wide_collected])
    result["area_counts"] = Counter(f'{f["x"][0]},{f["y"][0] - PANEL_Y_OFFSET}..{f["x"][1]},{f["y"][1] - PANEL_Y_OFFSET}' for f in collected).most_common(12)
    for key in ("first_any_ms", "content_ms", "first_spi_ms"):
        if key in result:
            result[key] = dict(n=len(result[key]), p50_p95=percentiles(result[key]))
    for key, values in list(result.items()):
        if ((key.startswith(("idle_", "lit_")) and not key.endswith("_states"))
                or key.endswith("_updates_s") or key == "updates_s"):
            result[key] = dict(p50=round(median(values), 3), runs=[round(x, 3) for x in values])
    return result


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("labels", nargs="+")
    parser.add_argument("--directory", type=Path, default=HERE)
    args = parser.parse_args()
    print(json.dumps({label: summarize(label, args.directory) for label in args.labels}, indent=2))
