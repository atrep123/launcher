/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_audio_volume_step.hpp"
#include "cp0_pulse_client.hpp"

#include <pulse/pulseaudio.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {
std::string_view scenario;
int reads = 0;
int writes = 0;
int unmutes = 0;
pa_context *initial_context = nullptr;
bool read_on_original = false;

pa_operation *read_sink(pa_context *context, const char *name,
                        pa_sink_info_cb_t callback, void *data)
{
    if (++reads == 1) initial_context = context;
    if (reads == 2 && writes == 1 && unmutes == 0) {
        read_on_original = context == initial_context;
        if (scenario == "unreadable") return nullptr;
    }
    return pa_context_get_sink_info_by_name(context, name, callback, data);
}

pa_operation *write_volume(pa_context *context, uint32_t index, const pa_cvolume *volume,
                           pa_context_success_cb_t callback, void *data)
{
    ++writes;
    if (scenario == "ack-noop") {
        // Acknowledge successfully while leaving the original channel volumes as they were.
        pa_cvolume original;
        pa_cvolume_set(&original, volume->channels, PA_VOLUME_NORM);
        return pa_context_set_sink_volume_by_index(context, index, &original, callback, data);
    }
    if (scenario == "rollback" && writes == 2) {
        assert(volume->channels == 2 && volume->values[0] == 78643 &&
               volume->values[1] == 13107);
    }
    return pa_context_set_sink_volume_by_index(context, index, volume, callback, data);
}

pa_operation *write_mute(pa_context *context, uint32_t index, int mute,
                         pa_context_success_cb_t callback, void *data)
{
    if (!mute) ++unmutes;
    return pa_context_set_sink_mute_by_index(context, index, mute, callback, data);
}
} // namespace

extern "C" void *__real_dlsym(void *, const char *);
extern "C" void *__wrap_dlsym(void *handle, const char *name)
{
    if (std::strcmp(name, "pa_context_get_sink_info_by_name") == 0)
        return reinterpret_cast<void *>(&read_sink);
    if (std::strcmp(name, "pa_context_set_sink_volume_by_index") == 0)
        return reinterpret_cast<void *>(&write_volume);
    if (std::strcmp(name, "pa_context_set_sink_mute_by_index") == 0)
        return reinterpret_cast<void *>(&write_mute);
    return __real_dlsym(handle, name);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    int saves = 0;
    const auto result = cp0::pulse::step_default_sink_volume(
        [](int current) { return current - 10; },
        [&](int, int) { ++saves; return scenario != "rollback"; });
    const int live = cp0::pulse::read_default_sink_volume();
    const int mute = cp0::pulse::read_default_sink_mute();
    std::fprintf(stderr, "%s: level=%d mute=%d writes=%d unmutes=%d saves=%d read_on_original=%d\n",
                 argv[1], live, mute, writes, unmutes, saves, read_on_original);

    if (scenario == "rollback") {
        return !(read_on_original && live == 120 && mute == 0 &&
                 result.percent == 100 && writes == 2 && saves == 1);
    }
    return !(read_on_original && result.muted && unmutes == 0 && saves == 0 &&
             result.percent == 100 && live == 100 && mute == 1 &&
             writes == (scenario == "unreadable" ? 2 : 1));
}
