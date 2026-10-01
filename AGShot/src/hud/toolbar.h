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

    enum class ToolbarHitTarget
    {
        None,
        Panel,
        Handle,
        Copy,
        Cancel,
    };

    // Where the buttons ended up, and which one the pointer is over. Geometry
    // only: drawing them is the overlay's job, because it owns the device
    // context.
    struct Toolbar
    {
        RECT panel{};
        RECT handle{};
        RECT copy{};
        RECT cancel{};
        ToolbarButton hovered = ToolbarButton::None;
    };

    // Tries each side of the selection before using an overlapping fallback, and
    // keeps the toolbar within "bounds". "dpi" scales its controls.
    bool LayoutToolbar(Toolbar& toolbar, const RECT& selection, const RECT& bounds, int dpi) noexcept;

    ToolbarButton HitTestToolbar(const Toolbar& toolbar, POINT point) noexcept;
    ToolbarHitTarget HitTestToolbarTarget(const Toolbar& toolbar, POINT point) noexcept;
    void DragToolbar(Toolbar& toolbar, const Toolbar& start, POINT origin, POINT current,
                     const RECT& bounds) noexcept;

    // What each button's tooltip says. Empty for "None".
    LPCWSTR ToolbarLabel(ToolbarButton button) noexcept;
}
