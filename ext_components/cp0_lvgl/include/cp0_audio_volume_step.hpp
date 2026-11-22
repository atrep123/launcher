/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <functional>

namespace cp0::pulse {

// Synchronous device HAL operation for volume shortcuts. Computes the target
// from the live level and writes it to the same sink on the same connection.
// Calls persist(previous, written) after a successful write and restores the
// previous percentage if persistence fails. Returns the written percentage,
// the previous percentage on write or save failure, or -1 on read failure.
int step_default_sink_volume(const std::function<int(int)> &target,
                             const std::function<bool(int, int)> &persist);

} // namespace cp0::pulse
