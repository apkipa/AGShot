#pragma once

#include <windows.h>

namespace agshot
{
    enum class ToolbarButton
    {
        None,
        Copy,
        Cancel,
    };

    // Where the buttons ended up, and which one the pointer is over. Geometry
    // only: drawing them is the overlay's job, because it owns the device
    // context.
    struct Toolbar
    {
        RECT panel{};
        RECT copy{};
        RECT cancel{};
        ToolbarButton hovered = ToolbarButton::None;
    };

    // Puts the panel against the selection: below it, or above it when the
    // bottom of the screen is in the way, and always inside "bounds". "dpi" is
    // the display scaling, so the buttons keep their apparent size on a
    // high-DPI screen.
    void LayoutToolbar(Toolbar& toolbar, const RECT& selection, const RECT& bounds, int dpi) noexcept;

    ToolbarButton HitTestToolbar(const Toolbar& toolbar, POINT point) noexcept;

    // What each button's tooltip says. Empty for "None".
    LPCWSTR ToolbarLabel(ToolbarButton button) noexcept;
}
