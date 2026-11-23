#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
# Linux dependencies: a C++17 compiler, libpulse development headers, pulseaudio and pactl.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temp=$(mktemp -d)
export PULSE_RUNTIME_PATH="$temp/runtime" PULSE_SERVER="unix:$temp/socket"
trap 'pactl exit >/dev/null 2>&1 || true; rm -rf -- "$temp"' EXIT
trap 'exit 1' HUP INT TERM
cat >"$temp/pulse.conf" <<EOF
load-module module-native-protocol-unix socket=$temp/socket auth-anonymous=1
load-module module-null-sink sink_name=volume_step_test channels=2
set-default-sink volume_step_test
EOF

compiler=${CXX:-c++}
"$compiler" -std=c++17 -Wall -Wextra -Werror -I"$root/include" -I"$root/src/cp0" \
    "$root/tests/test_pulse_volume_step.cpp" "$root/src/cp0/cp0_pulse_client.cpp" \
    -Wl,--wrap=dlsym -ldl -lpulse -o "$temp/test"
pulseaudio --daemonize=yes -n --exit-idle-time=-1 \
    --log-target="file:$temp/server.log" -F "$temp/pulse.conf"

for scenario in ack-noop unreadable; do
    pactl set-sink-volume volume_step_test 100%
    pactl set-sink-mute volume_step_test 1
    "$temp/test" "$scenario"
done
pactl set-sink-volume volume_step_test 78643 13107
pactl set-sink-mute volume_step_test 0
"$temp/test" rollback
