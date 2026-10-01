#include "pch.h"

#include "hud/toolbar.h"

#include <algorithm>

namespace agshot
{
    namespace
    {
        // Everything here is in DIPs at 96 DPI and scaled on the way out.
        constexpr int kButtonSize = 32;
        constexpr int kPadding = 4;
        constexpr int kGap = 2;
        constexpr int kMargin = 8;
    }

    void LayoutToolbar(Toolbar& toolbar, const RECT& selection, const RECT& bounds, int dpi) noexcept
    {
        const int button = MulDiv(kButtonSize, dpi, 96);
        const int padding = MulDiv(kPadding, dpi, 96);
        const int gap = MulDiv(kGap, dpi, 96);
        const int margin = MulDiv(kMargin, dpi, 96);

        const int width = button * 2 + gap + padding * 2;
        const int height = button + padding * 2;

        // RECT holds LONG, which is not int, and the standard algorithms will not
        // mix the two. Everything below is int.
        const int selectionLeft = static_cast<int>(selection.left);
        const int selectionTop = static_cast<int>(selection.top);
        const int selectionRight = static_cast<int>(selection.right);
        const int selectionBottom = static_cast<int>(selection.bottom);
        const int boundsLeft = static_cast<int>(bounds.left);
        const int boundsTop = static_cast<int>(bounds.top);
        const int boundsRight = static_cast<int>(bounds.right);
        const int boundsBottom = static_cast<int>(bounds.bottom);

        // Centred on the selection, below it by preference.
        int x = selectionLeft + ((selectionRight - selectionLeft) - width) / 2;
        int y = selectionBottom + margin;

        if (y + height > boundsBottom)
        {
            y = selectionTop - margin - height;
        }
        if (y < boundsTop)
        {
            // Neither side has room - the selection fills the screen - so tuck it
            // against the bottom edge and let it overlap rather than vanish.
            y = boundsBottom - height;
        }

        const int maxX = std::max(boundsLeft, boundsRight - width);
        const int maxY = std::max(boundsTop, boundsBottom - height);
        x = std::clamp(x, boundsLeft, maxX);
        y = std::clamp(y, boundsTop, maxY);

        toolbar.panel = RECT{ x, y, x + width, y + height };
        toolbar.copy = RECT{ x + padding, y + padding,
                             x + padding + button, y + padding + button };
        toolbar.cancel = RECT{ x + padding + button + gap, y + padding,
                               x + padding + button * 2 + gap, y + padding + button };
    }

    ToolbarButton HitTestToolbar(const Toolbar& toolbar, POINT point) noexcept
    {
        if (PtInRect(&toolbar.copy, point))
        {
            return ToolbarButton::Copy;
        }
        if (PtInRect(&toolbar.cancel, point))
        {
            return ToolbarButton::Cancel;
        }
        return ToolbarButton::None;
    }

    LPCWSTR ToolbarLabel(ToolbarButton button) noexcept
    {
        switch (button)
        {
        case ToolbarButton::Copy:
            return L"Copy to clipboard";
        case ToolbarButton::Cancel:
            return L"Cancel";
        default:
            return L"";
        }
    }
}
