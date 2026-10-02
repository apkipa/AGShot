#include "pch.h"

#include "hud/toolbar.h"

#include <algorithm>

namespace agshot
{
    namespace
    {
        // Everything here is in DIPs at 96 DPI and scaled on the way out.
        constexpr int kButtonSize = 32;
        constexpr int kHandleWidth = 18;
        constexpr int kPadding = 4;
        constexpr int kGap = 2;
        constexpr int kGroupGap = 6;
        constexpr int kDividerWidth = 1;
        constexpr int kMargin = 8;
    }

    bool LayoutToolbar(Toolbar& toolbar, const RECT& selection, const RECT& bounds, int dpi) noexcept
    {
        const int button = MulDiv(kButtonSize, dpi, 96);
        const int padding = MulDiv(kPadding, dpi, 96);
        const int gap = MulDiv(kGap, dpi, 96);
        const int groupGap = MulDiv(kGroupGap, dpi, 96);
        const int dividerWidth = std::max(1, MulDiv(kDividerWidth, dpi, 96));
        const int margin = MulDiv(kMargin, dpi, 96);

        const int handle = MulDiv(kHandleWidth, dpi, 96);
        const int width = handle + button * 4 + gap + groupGap * 3 + dividerWidth + padding * 2;
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

        const int maxX = std::max(boundsLeft, boundsRight - width);
        const int maxY = std::max(boundsTop, boundsBottom - height);
        const int rightAlignedX = std::clamp(selectionRight - width, boundsLeft, maxX);
        const int centeredY = std::clamp(
            selectionTop + ((selectionBottom - selectionTop) - height) / 2, boundsTop, maxY);
        const RECT candidates[] =
        {
            { rightAlignedX, selectionBottom + margin, rightAlignedX + width, selectionBottom + margin + height },
            { rightAlignedX, selectionTop - margin - height, rightAlignedX + width, selectionTop - margin },
            { selectionRight + margin, centeredY, selectionRight + margin + width, centeredY + height },
            { selectionLeft - margin - width, centeredY, selectionLeft - margin, centeredY + height },
        };

        int x = 0;
        int y = 0;
        bool found = false;
        for (const RECT& candidate : candidates)
        {
            if (candidate.left < boundsLeft || candidate.top < boundsTop
                || candidate.right > boundsRight || candidate.bottom > boundsBottom)
            {
                continue;
            }

            RECT intersection{};
            if (IntersectRect(&intersection, &candidate, &selection))
            {
                continue;
            }

            x = candidate.left;
            y = candidate.top;
            found = true;
            break;
        }
        if (!found)
        {
            x = rightAlignedX;
            y = std::clamp(selectionBottom + margin, boundsTop, maxY);
            if (y + height > boundsBottom)
            {
                y = std::clamp(selectionTop - margin - height, boundsTop, maxY);
            }
        }

        toolbar.panel = RECT{ x, y, x + width, y + height };
        const int handleLeft = x + padding;
        toolbar.handle = RECT{ handleLeft, y + padding,
                               handleLeft + handle, y + padding + button };
        const int modesLeft = handleLeft + handle + groupGap;
        toolbar.editSelection = RECT{ modesLeft, y + padding,
                                      modesLeft + button, y + padding + button };
        toolbar.textRecognition = RECT{ modesLeft + button, y + padding,
                                        modesLeft + button * 2, y + padding + button };
        const int dividerLeft = modesLeft + button * 2 + groupGap;
        toolbar.divider = RECT{ dividerLeft, y + padding + MulDiv(7, dpi, 96),
                                dividerLeft + dividerWidth, y + padding + button - MulDiv(7, dpi, 96) };
        const int actionsLeft = dividerLeft + dividerWidth + groupGap;
        toolbar.cancel = RECT{ actionsLeft, y + padding,
                               actionsLeft + button, y + padding + button };
        toolbar.copy = RECT{ actionsLeft + button + gap, y + padding,
                             actionsLeft + button * 2 + gap, y + padding + button };
        return true;
    }

    ToolbarButton HitTestToolbar(const Toolbar& toolbar, POINT point) noexcept
    {
        if (PtInRect(&toolbar.editSelection, point))
        {
            return ToolbarButton::EditSelection;
        }
        if (PtInRect(&toolbar.textRecognition, point))
        {
            return ToolbarButton::TextRecognition;
        }
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

    ToolbarHitTarget HitTestToolbarTarget(const Toolbar& toolbar, POINT point) noexcept
    {
        switch (HitTestToolbar(toolbar, point))
        {
        case ToolbarButton::EditSelection:
            return ToolbarHitTarget::EditSelection;
        case ToolbarButton::TextRecognition:
            return ToolbarHitTarget::TextRecognition;
        case ToolbarButton::Copy:
            return ToolbarHitTarget::Copy;
        case ToolbarButton::Cancel:
            return ToolbarHitTarget::Cancel;
        default:
            break;
        }

        if (PtInRect(&toolbar.handle, point))
        {
            return ToolbarHitTarget::Handle;
        }
        if (PtInRect(&toolbar.panel, point))
        {
            return ToolbarHitTarget::Panel;
        }
        return ToolbarHitTarget::None;
    }

    void DragToolbar(Toolbar& toolbar, const Toolbar& start, POINT origin, POINT current,
                     const RECT& bounds) noexcept
    {
        const int startX = static_cast<int>(start.panel.left);
        const int startY = static_cast<int>(start.panel.top);
        const int width = static_cast<int>(start.panel.right - start.panel.left);
        const int height = static_cast<int>(start.panel.bottom - start.panel.top);
        const int boundsLeft = static_cast<int>(bounds.left);
        const int boundsTop = static_cast<int>(bounds.top);
        const int boundsRight = static_cast<int>(bounds.right);
        const int boundsBottom = static_cast<int>(bounds.bottom);
        const int maxX = std::max(boundsLeft, boundsRight - width);
        const int maxY = std::max(boundsTop, boundsBottom - height);
        const int dxFromPointer = static_cast<int>(current.x) - static_cast<int>(origin.x);
        const int dyFromPointer = static_cast<int>(current.y) - static_cast<int>(origin.y);
        const int x = std::clamp(startX + dxFromPointer, boundsLeft, maxX);
        const int y = std::clamp(startY + dyFromPointer, boundsTop, maxY);
        const int dx = x - startX;
        const int dy = y - startY;

        toolbar.panel = start.panel;
        toolbar.handle = start.handle;
        toolbar.editSelection = start.editSelection;
        toolbar.textRecognition = start.textRecognition;
        toolbar.divider = start.divider;
        toolbar.copy = start.copy;
        toolbar.cancel = start.cancel;
        OffsetRect(&toolbar.panel, dx, dy);
        OffsetRect(&toolbar.handle, dx, dy);
        OffsetRect(&toolbar.editSelection, dx, dy);
        OffsetRect(&toolbar.textRecognition, dx, dy);
        OffsetRect(&toolbar.divider, dx, dy);
        OffsetRect(&toolbar.copy, dx, dy);
        OffsetRect(&toolbar.cancel, dx, dy);
    }

    LPCWSTR ToolbarLabel(ToolbarButton button) noexcept
    {
        switch (button)
        {
        case ToolbarButton::EditSelection:
            return L"Edit selection";
        case ToolbarButton::TextRecognition:
            return L"Recognize text";
        case ToolbarButton::Copy:
            return L"Copy to clipboard (Ctrl+C)";
        case ToolbarButton::Cancel:
            return L"Cancel";
        default:
            return L"";
        }
    }
}
