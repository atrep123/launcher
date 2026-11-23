#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT

set -eu
test_root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../../.." && pwd)
cp0_root="$repo_root/ext_components/cp0_lvgl"
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
trap 'exit 1' HUP INT TERM

${CXX:-g++} -std=c++17 -Wall -Wextra -Werror -pthread \
    -I"$cp0_root/include" \
    -I"$repo_root/SDK/github_source/eventpp/include" \
    "$cp0_root/src/commount.cpp" \
    "$cp0_root/src/cp0_network_api_contract.cpp" \
    "$test_root/test_launcher_media_controls.cpp" \
    "$test_root/../main/ui/launcher_media_controls.cpp" \
    "$test_root/../main/ui/model/launcher_media_model.cpp" \
    "$test_root/../main/ui/model/setup_value_policy.cpp" \
    -o "$build_dir/test_launcher_media_controls"
"$build_dir/test_launcher_media_controls"

${CXX:-g++} -std=c++17 -Wall -Wextra -Werror -pthread \
    -I"$cp0_root/include" "$test_root/test_volume_shortcut_worker.cpp" \
    -o "$build_dir/test_volume_shortcut_worker"
"$build_dir/test_volume_shortcut_worker"
