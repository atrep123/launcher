/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_enum_cast.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class AppFolder : std::uint8_t {
    System, Tools, Radio, Media, Games, Other, Count,
};

inline constexpr auto kAppFolderCount = CP0_ENUM_CAST_SIZE_T(AppFolder::Count);

struct AppFolderDefinition {
    const char *label;
    std::uint32_t color;
};

const AppFolderDefinition &app_folder_definition(AppFolder folder);
AppFolder app_folder_for_app(std::string_view categories, std::string_view name,
                            std::string_view desktop_file = {});

using AppFolderOverrides = std::unordered_map<std::string, AppFolder>;
AppFolderOverrides parse_app_folder_overrides(std::istream &input,
                                              const std::function<void(std::size_t)> &log_invalid);
AppFolderOverrides load_app_folder_overrides();
AppFolder apply_app_folder_override(const AppFolderOverrides &overrides,
                                   const std::string &id, const std::string &name,
                                   AppFolder default_folder);

class AppFolderModel {
public:
    struct App {
        std::string id;
        AppFolder folder;
    };

    struct Item {
        std::size_t app_index = 0;
        std::optional<AppFolder> folder;
    };

    void rebuild(std::vector<App> apps, bool enabled,
                 std::optional<std::size_t> legacy_builtin_count = std::nullopt);
    bool enabled() const { return enabled_; }
    bool open_folder();
    bool close_folder();
    std::size_t size() const;
    std::size_t index() const;
    std::size_t folder_size(AppFolder folder) const;
    std::optional<Item> item(int offset = 0) const;
    void move(int offset);

private:
    std::vector<App> apps_;
    std::array<std::vector<std::size_t>, kAppFolderCount> members_;
    std::vector<AppFolder> folders_;
    std::optional<AppFolder> open_folder_;
    bool enabled_ = false;
    int flat_index_ = 2;
    std::size_t folder_index_ = 0;
    std::size_t member_index_ = 0;
};
