// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <bit>
#include <chrono>
#include <optional>

#include "common/types.h"

/// Where the player looks, for foveated rendering: the headset's OpenXR host writes it, the
/// renderer reads it. Directions are tangents in the head's frame (+x right, +y up), so they
/// compare directly with the field of view the title draws (Fov in vr_runtime.h).
namespace Core::Vr::EyeGaze {

struct Direction {
    float tan_x{};
    float tan_y{};
};

namespace Detail {
inline std::atomic<u64> direction{};
inline std::atomic<s64> direction_time{}; ///< steady_clock nanoseconds, 0 for none
inline std::atomic<u64> title_tangents{};

inline u64 Pack(float a, float b) {
    return (u64{std::bit_cast<u32>(a)} << 32) | std::bit_cast<u32>(b);
}
inline float First(u64 packed) {
    return std::bit_cast<float>(static_cast<u32>(packed >> 32));
}
inline float Second(u64 packed) {
    return std::bit_cast<float>(static_cast<u32>(packed));
}
inline s64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace Detail

/// The host saw the eyes look this way just now.
inline void Set(float tan_x, float tan_y) {
    Detail::direction.store(Detail::Pack(tan_x, tan_y), std::memory_order_relaxed);
    Detail::direction_time.store(Detail::Now(), std::memory_order_release);
}

/// Where the eyes looked, if that was seen within `max_age`.
inline std::optional<Direction> Get(std::chrono::milliseconds max_age) {
    const s64 time = Detail::direction_time.load(std::memory_order_acquire);
    if (time == 0 || Detail::Now() - time > std::chrono::nanoseconds{max_age}.count()) {
        return std::nullopt;
    }
    const u64 packed = Detail::direction.load(std::memory_order_relaxed);
    return Direction{Detail::First(packed), Detail::Second(packed)};
}

/// The half widths of what the title draws, as tangents (its field of view is symmetric).
inline void SetTitleTangents(float half_x, float half_y) {
    Detail::title_tangents.store(Detail::Pack(half_x, half_y), std::memory_order_relaxed);
}

inline std::optional<Direction> TitleTangents() {
    const u64 packed = Detail::title_tangents.load(std::memory_order_relaxed);
    if (packed == 0) {
        return std::nullopt;
    }
    return Direction{Detail::First(packed), Detail::Second(packed)};
}

} // namespace Core::Vr::EyeGaze
