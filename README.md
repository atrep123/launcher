# Measurement artifacts for CardputerZero/launcher display PRs

Supporting data for the pull requests "Flush fbdev updates as contiguous stripes" (`perf/fbdev-pwrite-stripes`)
and "Wake LVGL when the keyboard queue receives input" (`perf/keyboard-queue-wake`) from `atrep123/launcher`.
This branch only holds data and tools; it is not meant to be merged.

## Layout

- `tools/` — capture procedure (`README.md`), ftrace capture script (`trace_capture.py`), evdev key generator
  (`keygen.py`) and the SPI decoder (`spi_decode.py`) that turns ST7789 CASET/PASET/RAMWR traffic into rectangles.
- `final/` — the measurements behind the PR tables. All four builds ran in one CardputerZero boot, on battery,
  `ondemand` governor, APPLaunch restarted before each set:
  - `vyrobce` = shipping `launcher-v0.6.111` binary,
  - `A` = `perf/fbdev-pwrite-stripes` at `b3dea21`,
  - `C` = `perf/keyboard-queue-wake` at `bb3fdfb`,
  - `AC` = their merge `17b9549`.
  Per build: `*-01..03-idle` (locked 25 s windows), `*-04/06/08-dense` (64 right presses),
  `*-05/07-latency` (24 spaced presses each); `*.trace.gz` = raw ftrace, `*.json` = decoded run,
  `*-souhrn.json` = per-build summary, `*-stav.txt` = SHA-256 of the installed binary, boot id and governor.
  `boot.txt` and `pushed-heads.tsv` record the boot and the branch heads used.
- `historical/` — sanitized traces from the first patch iteration (shipping binary vs. the original patched
  build). Kept for reference only; the PR tables come from `final/`.
