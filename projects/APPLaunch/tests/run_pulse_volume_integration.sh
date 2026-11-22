#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
set -eu

test_root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../../.." && pwd)
cp0="$repo_root/ext_components/cp0_lvgl"
ui="$test_root/../main/ui"
export VOLUME_TEST_DIR
VOLUME_TEST_DIR=$(mktemp -d)
export PULSE_RUNTIME_PATH="$VOLUME_TEST_DIR/runtime"
export PULSE_SERVER="unix:$VOLUME_TEST_DIR/socket"
trap 'timeout 2s pactl exit >/dev/null 2>&1 || :; rm -rf -- "$VOLUME_TEST_DIR"' EXIT
cat > "$VOLUME_TEST_DIR/pulse.conf" <<CONF
load-module module-native-protocol-unix socket=$VOLUME_TEST_DIR/socket auth-anonymous=1
load-module module-null-sink sink_name=launcher_a channels=2 channel_map=front-left,front-right
load-module module-null-sink sink_name=launcher_b
set-default-sink launcher_a
CONF

# Keep the real subscription and delay delivery before the application sees it.
${CXX:-g++} -std=c++17 -Wall -Wextra -Werror \
    -I"$cp0/include" -Dwatch_default_sink_changes=watch_test_server \
    -Dread_default_sink_volume=read_test_server_volume \
    -c "$cp0/src/cp0/cp0_pulse_client.cpp" -o "$VOLUME_TEST_DIR/pulse.o"
${CXX:-g++} -std=c++17 -Wall -Wextra -Werror -pthread -DCONFIG_CP0_LVGL_INIT_AUDIO \
    -I"$cp0/include" -I"$cp0/src/cp0" -I"$ui" \
    -I"$repo_root/SDK/github_source/eventpp/include" \
    "$test_root/test_pulse_volume_integration.cpp" "$VOLUME_TEST_DIR/pulse.o" \
    "$ui/launcher_media_controls.cpp" "$ui/model/launcher_media_model.cpp" \
    "$ui/model/setup_value_policy.cpp" -ldl -o "$VOLUME_TEST_DIR/test"
"$VOLUME_TEST_DIR/test"
