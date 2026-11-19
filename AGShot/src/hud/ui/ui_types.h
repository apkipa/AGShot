#pragma once

#include "hud/text_edit_utils.h"

#include <windows.h>

#include <cstdint>

namespace agshot::ui
{
    enum class Invalidate : std::uint8_t
    {
        None   = 0,
        Layout = 1u << 0,
        Paint  = 1u << 1,
        Cursor = 1u << 2,
        Ime    = 1u << 3,
    };

    inline constexpr Invalidate operator|(Invalidate a, Invalidate b) noexcept
    {
        return static_cast<Invalidate>(
            static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
    }

    inline constexpr Invalidate& operator|=(Invalidate& a, Invalidate b) noexcept
    {
        a = a | b;
        return a;
    }

    inline constexpr bool HasFlag(Invalidate value, Invalidate flag) noexcept
    {
        return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
    }

    enum class PointerButton
    {
        Left,
        Right,
    };

    enum class PointerKind
    {
        Mouse,
        Pen,
        Touch,
    };

    enum class EventOutcome
    {
        Ignored,
        Handled,
        Capture,
    };

    enum class OutsideClickPolicy
    {
        Ignore,
        DismissAndPass,
        DismissAndAbsorb,
    };

    enum class ImePolicy
    {
        Pass,
        Disable,
        Enable,
    };

    struct PointerInput
    {
        POINT point{};
        KeyModifiers mods{};
        int wheelDelta{};
        PointerKind kind{ PointerKind::Mouse };
    };

    // Accumulates fine-grained high-resolution / precision-touchpad wheel deltas
    // and emits discrete integer detent steps once |accum| reaches threshold (WHEEL_DELTA = 120).
    // Resets immediately on direction reversal or target switch.
    struct WheelDetentAccumulator
    {
        int accum{};
        std::uint64_t targetKey{};

        int Feed(int delta, std::uint64_t key = 0, int threshold = WHEEL_DELTA) noexcept
        {
            if (delta == 0 || threshold <= 0)
            {
                return 0;
            }
            if (key != targetKey || (accum != 0 && ((accum > 0) != (delta > 0))))
            {
                accum = 0;
                targetKey = key;
            }
            accum += delta;
            const int steps = accum / threshold;
            accum -= steps * threshold;
            return steps;
        }

        void Reset() noexcept
        {
            accum = 0;
            targetKey = 0;
        }
    };
}
