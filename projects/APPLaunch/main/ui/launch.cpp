/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "launch.h"

#include "app_registry.h"
#include "builtin_app_registry.hpp"
#include "desktop_app_loader.hpp"
#include "esc_hold_hint_controller.h"
#include "ui.h"
#include "generated/page_app.h"
#include "ui_launch_page.h"
#include "ui_screensaver.h"
#include "ui_loading.h"
#include "cp0_lvgl_app.h"
#include "sample_log.h"

#include <array>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
constexpr size_t kHomeCarouselSlotCount = 5;
constexpr int kHomeCarouselCenterSlot = 2;
}

// ============================================================
// Launch
// ============================================================
void Launch::bind_ui()
{
    if (bound_) {
        refresh_home_carousel();
        return;
    }
    bound_ = true;

    launcher_app_registry_set_changed_callback(app_registry_changed_cb, this);
    applications_load();
    reload_home_icons();

    app_directory_watcher_.start([this] { applications_reload(); });
    esc_ui_watchdog_.start();
    esc_hold_hint_controller().set_force_home_callback(esc_force_home_cb, this);
}

void Launch::launch_app()
{
    const auto item = carousel_.item();
    if (!item) return;
    if (item->folder) {
        if (carousel_.open_folder()) refresh_home_carousel();
        return;
    }
    const app *selected = ordered_apps_[item->app_index];

    try {
        selected->launch(this);
    } catch (const std::exception &error) {
        SLOGE("Failed to launch app %s: %s", selected->Name.c_str(), error.what());
        abort_page_launch();
    } catch (...) {
        SLOGE("Failed to launch app %s: unknown exception", selected->Name.c_str());
        abort_page_launch();
    }
}

void Launch::lv_go_back_home(void *arg) noexcept
{
    auto *self = static_cast<Launch *>(arg);
    if (!self || !self->page_lifecycle_.complete_home()) return;
    try {
        esc_hold_hint_controller().set_return_home_enabled(false);
        SLOGI("[HOME] lv_go_back_home executing (page=%p)", self->app_Page.get());
        lv_timer_enable(true);
        if (auto page = self->launch_page_.lock()) page->show_home_screen();
        lv_refr_now(nullptr);
        self->app_Page.reset();
        self->esc_ui_watchdog_.disarm();
        SLOGI("[HOME] lv_go_back_home done, on launcher home");
    } catch (...) {
        self->app_Page.reset();
        self->esc_ui_watchdog_.disarm();
    }
}

void Launch::go_back_home()
{
    if (!page_lifecycle_.request_home()) return;
    esc_hold_hint_controller().set_return_home_enabled(false);
    SLOGI("[HOME] go_back_home() requested, scheduling async call (page=%p)", app_Page.get());
    if (lv_async_call(lv_go_back_home, this) != LV_RESULT_OK) {
        page_lifecycle_.cancel_home_request();
        esc_hold_hint_controller().set_return_home_enabled(true);
    }
}

bool Launch::begin_page_launch()
{
    if (!page_lifecycle_.begin_app()) return false;
    esc_hold_hint_controller().set_return_home_enabled(true);
    esc_ui_watchdog_.arm();
    return true;
}

void Launch::abort_page_launch() noexcept
{
    try {
        esc_ui_watchdog_.disarm();
    } catch (...) {
        SLOGW("[HOME] failed to disarm launch watchdog during rollback");
    }
    if (!page_lifecycle_.abort_app()) return;

    try {
        esc_hold_hint_controller().set_return_home_enabled(false);
        if (auto page = launch_page_.lock()) page->show_home_screen();
        app_Page.reset();
    } catch (...) {
        app_Page.reset();
    }

    try {
        ui_loading::hide();
        lv_refr_now(nullptr);
    } catch (...) {
    }
}

void Launch::launch_Exec_in_terminal(const std::string &exec, bool sysplause,
                                     TerminalHelpFactory help_factory)
{
    if (!begin_page_launch()) return;
    SLOGI("Launching terminal app: %s", exec.c_str());
    ui_loading::show("Loading...");
    lv_refr_now(nullptr);
    auto p = std::make_shared<UISTPage>(help_factory);
    if (!p->screen())
        throw std::runtime_error("terminal page creation failed");
    app_Page = p;
    p->navigate_home = std::bind(&Launch::go_back_home, this);
    p->terminal_sysplause = sysplause;
    ui_loading::hide();
    cp0_lvgl_start_app_page(*p);
    p->exec(exec);
}

void Launch::launch_Exec(const std::string &exec, bool keep_root)
{
    SLOGI("Launching external app: %s (keep_root=%d)", exec.c_str(), keep_root);
    ui_loading::show("Loading...");
    lv_disp_t *disp = lv_disp_get_default();
    lv_indev_t *indev = lv_indev_get_next(nullptr);
    ui_screensaver_set_foreground(0);
    LVGL_RUN_FLAGE = 0;
    if (indev) lv_indev_set_group(indev, nullptr);
    lv_refr_now(disp);
    // External apps own the framebuffer exclusively. Keep process supervision
    // active in the backend, but stop every Launcher timer/flush until the
    // child exits; drawing an LVGL toast here would overwrite the child UI.
    lv_timer_enable(false);
    int result = -1;
    try {
        cp0_signal_process_api(
            {"ExecBlocking", exec, keep_root ? "1" : "0"},
            [&](int code, std::string) { result = code; });
    } catch (...) {
        result = -1;
    }
    SLOGI("External app exited with code %d", result);

    lv_timer_enable(true);
    if (indev) lv_indev_set_group(indev, UILaunchPage::home_input_group());
    if (auto page = launch_page_.lock()) page->show_home_screen();
    ui_loading::hide();
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    LVGL_RUN_FLAGE = 1;
    ui_screensaver_set_foreground(1);
}

void Launch::select_next_app()
{
    carousel_.move(1);
}

void Launch::select_previous_app()
{
    carousel_.move(-1);
}

std::size_t Launch::app_count() const
{
    return carousel_.size();
}

std::size_t Launch::current_app_index() const
{
    return carousel_.index();
}

std::optional<HomeCarouselItem> Launch::carousel_slot_item(size_t slot) const
{
    if (slot >= kHomeCarouselSlotCount) return std::nullopt;
    const auto item = carousel_.item(static_cast<int>(slot) - kHomeCarouselCenterSlot);
    if (!item) return std::nullopt;
    if (item->folder) {
        static const std::string no_icon_path;
        return HomeCarouselItem{
            app_folder_definition(*item->folder).label, no_icon_path, item->folder};
    }
    const auto &app = *ordered_apps_[item->app_index];
    return HomeCarouselItem{app.Name.c_str(), app.Icon, std::nullopt};
}

bool Launch::can_switch_carousel_item() const
{
    return !carousel_.enabled() || carousel_.size() > 1;
}

void Launch::close_folder()
{
    if (carousel_.close_folder()) refresh_home_carousel();
}

void Launch::applications_load()
{
    std::list<app> apps;
    launcher_append_enabled_builtin_apps(apps);
    const auto builtin_count = apps.size();
    launcher_append_desktop_apps(apps);
    launcher_sort_app_display_order(apps);

    const bool folders_enabled = launcher_app_folders_enabled();
    const auto overrides = folders_enabled ? load_app_folder_overrides() : AppFolderOverrides{};
    std::vector<const app *> ordered_apps;
    std::vector<AppFolderModel::App> entries;
    ordered_apps.reserve(apps.size());
    entries.reserve(apps.size());
    for (const app &item : apps) {
        ordered_apps.push_back(&item);
        entries.push_back({item.id, apply_app_folder_override(overrides, item.id, item.Name, item.folder)});
    }

    carousel_.rebuild(std::move(entries), folders_enabled, builtin_count);
    app_list.swap(apps);
    ordered_apps_.swap(ordered_apps);
}

void Launch::refresh_home_carousel()
{
    if (auto page = launch_page_.lock()) page->refresh_carousel();
}

void Launch::reload_home_icons()
{
    std::vector<std::string> icon_paths;
    icon_paths.reserve(app_list.size());
    for (const app &item : app_list)
        icon_paths.push_back(item.Icon);

    std::array<std::size_t, kAppFolderCount> folder_sizes{};
    for (std::size_t i = 0; i < folder_sizes.size(); ++i)
        folder_sizes[i] = carousel_.folder_size(static_cast<AppFolder>(i));
    if (auto page = launch_page_.lock()) page->reload_home_icons(icon_paths, folder_sizes);
}

void Launch::applications_reload()
{
    applications_load();
    reload_home_icons();
}

void Launch::esc_force_home_cb(void *user_data) noexcept
{
    auto *self = static_cast<Launch *>(user_data);
    if (!self || !self->app_Page || LVGL_RUN_FLAGE == 0) return;
    try {
        SLOGW("[HOME] ESC held for 3000 ms, forcing built-in page home");
        self->go_back_home();
    } catch (...) {
    }
}


Launch::~Launch()
{
    launcher_app_registry_clear_changed_callback(app_registry_changed_cb, this);
    esc_hold_hint_controller().set_force_home_callback(nullptr, nullptr);
    esc_hold_hint_controller().set_return_home_enabled(false);
    esc_ui_watchdog_.shutdown();
    app_directory_watcher_.stop();
    page_lifecycle_.stop();
    lv_async_call_cancel(lv_go_back_home, this);
}

Launch::Launch() = default;

void Launch::set_launch_page(std::shared_ptr<UILaunchPage> launch_page)
{
    launch_page_ = std::move(launch_page);
}

void Launch::app_registry_changed_cb(void *user_data) noexcept
{
    auto *self = static_cast<Launch *>(user_data);
    if (!self) return;
    try {
        self->applications_reload();
    } catch (...) {
    }
}
