# CardputerZero framebuffer measurement attachments

[Capture script](trace_capture.py), [key generator](keygen.py), [SPI decoder](spi_decode.py), and [measurement inputs](traces/).

Run the scripts with Python 3 on 64-bit Linux. Capture needs tracefs with SPI and scheduler tracepoints, read/write access to the selected keyboard evdev node, and read access to fb0 and the power sensors. The collector creates and removes its own ftrace instance. Use the evdev node already opened by APPLaunch. Run each carousel capture from HOME with several apps available and let the launcher settle between runs.

```sh
sudo python3 trace_capture.py dense traces/new-01-dense --evdev /dev/input/eventN
sudo python3 trace_capture.py latency traces/new-02-latency --evdev /dev/input/eventN
sudo python3 trace_capture.py idle traces/new-03-idle --pid LAUNCHER_PID
```

Repeat dense three times, latency twice, and locked idle three times per build. Use distinct two-digit run numbers. Dense sends 64 right presses. Latency sends 24 right presses per run. Each press has 60 ms down and 60 ms after release, with an extra 530 ms in the latency workload. To lock, hold TAB for just over three seconds, wait for a dark backlight, and run the idle capture on battery. The collector samples battery current and voltage once per second for 25 seconds.

```sh
python3 spi_decode.py shipping patched
python3 spi_decode.py new
```

The decoder pairs SPI start and stop events on `spi0.0` by message ID and length, follows CASET and PASET, and counts a RAMWR window once its full RGB565 payload has transferred. The panel's 170 visible rows start at ST7789 row 35 in the 240-row address space. The decoder adds 35 when comparing framebuffer bounds to PASET and subtracts 35 when reporting framebuffer coordinates. The carousel band is framebuffer y=38–140. The throughput subset also spans x=0–319 and includes all rows y=49–129. Latency is measured from the EV_KEY press to the start of the first pixel transfer whose window overlaps the carousel band, within 400 ms.

The included inputs are from the original measurement. Their SPI traces retain transfer timing, message IDs, lengths, and panel address commands. The workload JSON holds monotonic evdev timestamps, framebuffer-change delays, and locked-idle wakeup and power samples.
