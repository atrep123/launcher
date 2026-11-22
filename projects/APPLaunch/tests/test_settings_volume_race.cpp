/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "hal_lvgl_bsp.h"
#include "launcher_media_controls.h"
#include "settings/settings_volume_page.hpp"

#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

decltype(cp0_signal_audio_api) cp0_signal_audio_api;
decltype(cp0_signal_config_api) cp0_signal_config_api;
decltype(cp0_signal_settings_api) cp0_signal_settings_api;
// The mixer race does not depend on the installed font assets.
Cp0FontService &cp0_fonts()
{
    static Cp0FontService fonts;
    return fonts;
}

Cp0FontService::~Cp0FontService() = default;
lv_font_t *Cp0FontService::get(const char *, uint16_t, lv_freetype_font_style_t,
                              lv_freetype_font_render_mode_t)
{
    return const_cast<lv_font_t *>(LV_FONT_DEFAULT);
}

lv_font_t *Cp0FontService::fallback(uint16_t) const
{
    return const_cast<lv_font_t *>(LV_FONT_DEFAULT);
}

namespace {

using namespace std::chrono_literals;

template <typename Condition>
void wait_for(Condition ready, bool run_ui = false)
{
    const auto timeout = std::chrono::steady_clock::now() + 5s;
    while (!ready()) {
        assert(std::chrono::steady_clock::now() < timeout);
        std::this_thread::sleep_for(2ms);
        if (run_ui) {
            lv_tick_inc(2);
            lv_timer_handler();
        }
    }
}

} // namespace

int main()
{
    int sink = 60;
    std::vector<int> writes;
    bool capture_stale = false;
    bool stale_queued = false;
    bool fresh_delivered = false;
    bool release_stale = false;
    std::mutex mixer;
    std::condition_variable worker;

    cp0_signal_config_api.append([](auto, auto reply) { reply(0, "60"); });
    cp0_signal_audio_api.append([&](auto command, auto reply) {
        std::unique_lock<std::mutex> lock(mixer);
        if (command.front() == "VolumeRead") {
            const int reply_value = sink;
            if (capture_stale) {
                capture_stale = false;
                stale_queued = true;
                worker.notify_all();
                assert(worker.wait_for(lock, 5s, [&] { return release_stale; }));
            }
            reply(0, std::to_string(reply_value));
            if (reply_value == 20) fresh_delivered = true;
        } else if (command.front() == "VolumeWrite") {
            sink = std::stoi(command.back());
            writes.push_back(sink);
            reply(0, std::to_string(sink));
        } else {
            assert(command.front() == "SystemSoundPlay");
            reply(0, "");
        }
    });

    lv_init();
    auto *display = lv_display_create(320, 170);
    assert(display);
    static uint8_t pixels[320 * 20 * 4];
    lv_display_set_buffers(display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, [](lv_display_t *disp, const lv_area_t *, uint8_t *) {
        lv_display_flush_ready(disp);
    });
    Tree settings;
    const auto speaker = settings.set_head(SettingEntry{"Speaker"});
    for (int index = 0; index < LvSettingVolumePage3::metric(
                 LvSettingVolumePage3::LayoutMetric::VolumeOptionCount); ++index) {
        settings.append_child(speaker, SettingEntry{
            std::to_string(LvSettingVolumePage3::volume_for_selection(index)) + "%"});
    }

    using namespace launcher_media_controls;
    start_volume_cache();
    wait_for([] { return cached_volume() == 60; });
    {
        std::lock_guard<std::mutex> lock(mixer);
        sink = 100;
        capture_stale = true;
    }
    refresh_volume_cache();
    {
        std::unique_lock<std::mutex> lock(mixer);
        assert(worker.wait_for(lock, 5s, [&] { return stale_queued; }));
        sink = 20;
    }

    bool left = false;
    int original = -1;
    int selected = -1;
    {
        LvSettingVolumePage3 page(lv_screen_active(), speaker, [&] { left = true; });
        wait_for([&] {
            std::lock_guard<std::mutex> lock(mixer);
            return fresh_delivered;
        }, true);

        // The server queued 100% before Settings read 20%. Deliver the old
        // reply now, while the Settings result is still queued for the UI.
        assert(!page.ready());
        {
            std::lock_guard<std::mutex> lock(mixer);
            release_stale = true;
            worker.notify_all();
        }
        wait_for([] { return cached_volume() == 100; });
        wait_for([&] { return page.ready(); }, true);
        original = page.original_volume();
        selected = page.volume_for_selection(page.selected_index);

        uint32_t down = LV_KEY_DOWN;
        lv_obj_send_event(page.Get(), LV_EVENT_KEY, &down);
        wait_for([&] { return !page.request_pending(); }, true);
        {
            std::lock_guard<std::mutex> lock(mixer);
            assert(writes.size() == 1);
        }

        uint32_t escape = LV_KEY_ESC;
        lv_obj_send_event(page.Get(), LV_EVENT_KEY, &escape);
        wait_for([&] { return left; }, true);
    }
    stop_volume_cache();

    std::printf("Delayed 100%% worker reply, fresh 20%% Settings read: "
                "original=%d%%, selection=%d%%, preview=%d%%, Esc restored=%d%%\n",
                original, selected, writes.front(), sink);
    const bool passed = original == 20 && selected == 20 && sink == 20 &&
                        writes == std::vector<int>{10, 20};
    lv_display_delete(display);
    lv_deinit();
    return passed ? 0 : 1;
}
