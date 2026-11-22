/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app_folder_model.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace {

struct FolderMapping {
    AppFolderDefinition definition;
    std::string_view categories;
};

// The first matching folder wins, independent of Categories token order.
// Hardware-specific categories take precedence over generic System or Utility.
constexpr std::array<FolderMapping, kAppFolderCount> kMappings = {{
    {{"System", 0x619dec}, "System;Settings;PackageManager;Monitor;"},
    {{"Tools", 0x52c5a1}, "Utility;Development;Office;Education;Science;Network;Accessories;"},
    {{"Radio", 0xedac4e}, "HamRadio;Radio;HardwareSettings;Electronics;AmateurRadio;X-Radio;X-Hardware;"},
    {{"Media", 0xb28aee}, "AudioVideo;Audio;Video;Graphics;Photography;Music;Player;Recorder;"},
    {{"Games", 0xee7c91}, "Game;"},
    {{"Other", 0x9daaba}, ""},
}};
constexpr AppFolder kCategoryPriority[] = {
    AppFolder::Games, AppFolder::Radio, AppFolder::Media, AppFolder::System, AppFolder::Tools,
};

struct KnownApp {
    std::string_view name;
    AppFolder folder;
};

// Older Cardputer packages omit Categories. These product names also cover
// the original packages that predate the Cap-* naming scheme.
constexpr KnownApp kKnownApps[] = {
    {"CC1101", AppFolder::Radio}, {"LoRa", AppFolder::Radio},
    {"GPS", AppFolder::Radio}, {"NFC", AppFolder::Radio},
    {"SubG", AppFolder::Radio}, {"SubG Chat", AppFolder::Radio},
    {"IR Remote", AppFolder::Radio}, {"IR Chat", AppFolder::Radio},
    {"RF sonda", AppFolder::Radio},
    {"Music", AppFolder::Media}, {"Rec", AppFolder::Media},
    {"Recorder", AppFolder::Media}, {"Piano", AppFolder::Media},
    {"Camera", AppFolder::Media},
    {"Files", AppFolder::Tools}, {"ZClaw", AppFolder::Tools},
    {"Keyboard Guide", AppFolder::Tools}, {"Compass", AppFolder::Tools},
    {"FactoryTest", AppFolder::Tools},
};

std::string_view trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

AppFolder known_app_folder(std::string_view name, std::string_view desktop_file)
{
    const auto radio_prefix = [](std::string_view value) {
        return value.substr(0, 4) == "Cap-" || value.substr(0, 3) == "IR-" ||
               value.substr(0, 4) == "cap-" || value.substr(0, 3) == "ir-";
    };
    if (radio_prefix(name) || radio_prefix(desktop_file)) return AppFolder::Radio;
    for (const auto &app : kKnownApps) {
        if (app.name == name) return app.folder;
    }
    return AppFolder::Other;
}

bool has_category(std::string_view categories, std::string_view token)
{
    while (!categories.empty()) {
        const auto separator = categories.find(';');
        if (categories.substr(0, separator) == token) return true;
        if (separator == std::string_view::npos) break;
        categories.remove_prefix(separator + 1);
    }
    return false;
}

std::size_t wrap(int index, std::size_t count)
{
    if (count == 0) return 0;
    const int size = static_cast<int>(count);
    index %= size;
    return static_cast<std::size_t>(index < 0 ? index + size : index);
}

} // namespace

const AppFolderDefinition &app_folder_definition(AppFolder folder)
{
    return kMappings[CP0_ENUM_CAST_SIZE_T(folder)].definition;
}

AppFolder app_folder_for_app(std::string_view categories, std::string_view name,
                            std::string_view desktop_file)
{
    categories = trim(categories);
    // Legacy Camera advertises only Utility even though it captures media.
    if (categories.empty() || (name == "Camera" && (categories == "Utility;" || categories == "Utility")))
        return known_app_folder(name, desktop_file);

    for (const auto folder : kCategoryPriority) {
        const auto registered = kMappings[CP0_ENUM_CAST_SIZE_T(folder)].categories;
        for (std::size_t start = 0; start < registered.size();) {
            const auto end = registered.find(';', start);
            if (has_category(categories, registered.substr(start, end - start))) return folder;
            start = end + 1;
        }
    }
    return AppFolder::Other;
}

AppFolderOverrides parse_app_folder_overrides(std::istream &input,
                                              const std::function<void(std::size_t)> &log_invalid)
{
    AppFolderOverrides overrides;
    std::string line;
    std::size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        std::string_view raw = line;
        if (number == 1 && raw.substr(0, 3) == "\xef\xbb\xbf") raw.remove_prefix(3);
        const auto entry = trim(raw);
        if (entry.empty() || entry.front() == '#') continue;

        const auto separator = entry.find('=');
        if (separator != std::string_view::npos) {
            const auto app = trim(entry.substr(0, separator));
            const auto label = trim(entry.substr(separator + 1));
            const auto folder = std::find_if(kMappings.begin(), kMappings.end(), [label](const auto &mapping) {
                return label == mapping.definition.label;
            });
            if (!app.empty() && folder != kMappings.end()) {
                overrides[std::string(app)] = static_cast<AppFolder>(folder - kMappings.begin());
                continue;
            }
        }
        log_invalid(number);
    }
    return overrides;
}

AppFolderOverrides load_app_folder_overrides()
{
    namespace fs = std::filesystem;
    const char *xdg_config = std::getenv("XDG_CONFIG_HOME");
    fs::path config = xdg_config ? xdg_config : "";
    if (!config.is_absolute()) {
        const char *home = std::getenv("HOME");
        if (!home || !*home) return {};
        config = fs::path(home) / ".config";
    }
    const auto path = config / "APPLaunch" / "folders.conf";
    std::error_code error;
    const auto status = fs::status(path, error);
    if (status.type() == fs::file_type::not_found) return {};
    if (!fs::is_regular_file(status)) {
        std::fprintf(stderr, "APPLaunch: cannot read folder assignments from %s\n", path.string().c_str());
        return {};
    }
    std::ifstream input(path);
    if (!input) {
        std::fprintf(stderr, "APPLaunch: cannot open %s\n", path.string().c_str());
        return {};
    }
    auto overrides = parse_app_folder_overrides(input, [&path](std::size_t line) {
        std::fprintf(stderr, "APPLaunch: ignoring malformed folder assignment in %s:%zu\n",
                     path.string().c_str(), line);
    });
    if (input.bad())
        std::fprintf(stderr, "APPLaunch: could not finish reading %s\n", path.string().c_str());
    return overrides;
}

AppFolder apply_app_folder_override(const AppFolderOverrides &overrides,
                                   const std::string &id, const std::string &name,
                                   AppFolder default_folder)
{
    const auto by_id = overrides.find(id);
    if (by_id != overrides.end()) return by_id->second;
    const auto by_name = overrides.find(name);
    return by_name == overrides.end() ? default_folder : by_name->second;
}

void AppFolderModel::rebuild(std::vector<App> apps, bool enabled,
                             std::optional<std::size_t> legacy_builtin_count)
{
    const auto previous = item();
    const std::string_view previous_id = previous && open_folder_
        ? std::string_view(apps_[previous->app_index].id) : std::string_view{};
    const auto previous_folder = enabled_ && !folders_.empty()
        ? std::optional<AppFolder>(folders_[folder_index_]) : std::nullopt;

    decltype(members_) members;
    std::vector<AppFolder> folders;
    if (enabled) {
        for (std::size_t i = 0; i < apps.size(); ++i)
            members[CP0_ENUM_CAST_SIZE_T(apps[i].folder)].push_back(i);
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (!members[i].empty()) folders.push_back(static_cast<AppFolder>(i));
        }
    }

    // Keep the flat carousel's existing normalization across registry reloads.
    if (!enabled && !enabled_ && legacy_builtin_count) {
        flat_index_ = *legacy_builtin_count == 0 ? -1
            : static_cast<int>(wrap(flat_index_, *legacy_builtin_count));
    }
    if (!apps.empty()) flat_index_ = static_cast<int>(wrap(flat_index_, apps.size()));

    folder_index_ = 0;
    member_index_ = 0;
    if (previous_folder) {
        const auto found = std::find(folders.begin(), folders.end(), *previous_folder);
        if (found != folders.end()) {
            folder_index_ = static_cast<std::size_t>(found - folders.begin());
        } else {
            open_folder_.reset();
        }
    }
    if (open_folder_) {
        const auto &indices = members[CP0_ENUM_CAST_SIZE_T(*open_folder_)];
        const auto found = std::find_if(indices.begin(), indices.end(), [&](std::size_t i) {
            return apps[i].id == previous_id;
        });
        if (found != indices.end())
            member_index_ = static_cast<std::size_t>(found - indices.begin());
    }

    apps_ = std::move(apps);
    members_ = std::move(members);
    folders_ = std::move(folders);
    enabled_ = enabled;
}

bool AppFolderModel::open_folder()
{
    if (open_folder_ || folders_.empty()) return false;
    open_folder_ = folders_[folder_index_];
    member_index_ = 0;
    return true;
}

bool AppFolderModel::close_folder()
{
    if (!open_folder_) return false;
    open_folder_.reset();
    return true;
}

std::size_t AppFolderModel::size() const
{
    if (!enabled_) return apps_.size();
    return open_folder_ ? folder_size(*open_folder_) : folders_.size();
}

std::size_t AppFolderModel::index() const
{
    if (!enabled_) return apps_.empty() ? 0 : static_cast<std::size_t>(flat_index_);
    return open_folder_ ? member_index_ : folder_index_;
}

std::size_t AppFolderModel::folder_size(AppFolder folder) const
{
    return members_[CP0_ENUM_CAST_SIZE_T(folder)].size();
}

std::optional<AppFolderModel::Item> AppFolderModel::item(int offset) const
{
    const auto count = size();
    if (count == 0 || (enabled_ && count == 1 && offset != 0)) return std::nullopt;
    const auto position = wrap(static_cast<int>(index()) + offset, count);
    if (!enabled_) return Item{position, std::nullopt};
    if (open_folder_)
        return Item{members_[CP0_ENUM_CAST_SIZE_T(*open_folder_)][position], std::nullopt};
    return Item{0, folders_[position]};
}

void AppFolderModel::move(int offset)
{
    const auto position = wrap(static_cast<int>(index()) + offset, size());
    if (!enabled_) flat_index_ = static_cast<int>(position);
    else if (open_folder_) member_index_ = position;
    else folder_index_ = position;
}
