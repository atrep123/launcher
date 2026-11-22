#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
# Usage: sh projects/APPLaunch/tests/run_settings_volume_race.sh CMAKE_EMULATOR_BUILD
set -eu

test_root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_root/../../.." && pwd)
emulator="$repo/projects/CardputerZero-Emulator"
cp0="$repo/ext_components/cp0_lvgl"
ui="$test_root/../main/ui"
lvgl_lib=$(realpath "$1/lvgl/lib")
build=$(mktemp -d)
trap 'rm -rf -- "$build"' EXIT

${CC:-cc} -DLV_CONF_INCLUDE_SIMPLE -I"$emulator" -I"$emulator/lib" \
    -I"$cp0/include" -c "$cp0/src/cp0_lvgl_app_page_assets.c" -o "$build/assets.o"
${CXX:-c++} -std=c++17 -pthread -DHAL_PLATFORM_SDL=1 -DLV_CONF_INCLUDE_SIMPLE \
    -I"$emulator" -I"$emulator/lib" -I"$cp0/include" \
    -I"$repo/SDK/github_source/eventpp/include" -I"$repo/SDK/components/utilities/include" \
    -I"$ui" -I"$test_root/../main/include" -include "$emulator/src/emu_compat.h" \
    "$test_root/test_settings_volume_race.cpp" "$ui/settings/settings_volume_page.cpp" \
    "$ui/launcher_media_controls.cpp" "$ui/model/launcher_media_model.cpp" \
    "$ui/model/setup_value_policy.cpp" "$build/assets.o" \
    -L"$lvgl_lib" -Wl,--start-group -llvgl -llvgl_thorvg -Wl,--end-group \
    $(pkg-config --libs sdl2 freetype2) -o "$build/test"
"$build/test"
