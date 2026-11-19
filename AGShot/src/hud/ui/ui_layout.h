#pragma once

#include <windows.h>

#include <algorithm>

namespace agshot::ui
{
    // Host layout context passed top-down on layout passes.
    struct LayoutEnv
    {
        RECT viewport{};
        int dpi{ 96 };
    };

    // Anchoring spec for absolute-positioned floating popups and menus.
    struct AnchorSpec
    {
        RECT anchor{};
        RECT avoidBand{};
        int gapDip{ 7 };
    };

    struct PopoverPlacement
    {
        RECT panel{};
        int anchorCenterX{};
        bool placedAbove{};
        bool drawCaret{};
    };

    inline RECT PadRect(const RECT& r, int pad) noexcept
    {
        return RECT{ r.left + pad, r.top + pad, r.right - pad, r.bottom - pad };
    }

    inline RECT CenterVertically(const RECT& slot, int height) noexcept
    {
        const int top = slot.top + ((slot.bottom - slot.top) - height) / 2;
        return RECT{ slot.left, top, slot.right, top + height };
    }

    struct HLayout
    {
        RECT cursor{};
        int gap{};

        RECT TakeLeft(int width) noexcept
        {
            const int w = std::max(0, width);
            const RECT out{ cursor.left, cursor.top,
                            std::min(cursor.right, cursor.left + w), cursor.bottom };
            cursor.left = std::min(cursor.right, out.right + gap);
            return out;
        }

        RECT TakeRight(int width) noexcept
        {
            const int w = std::max(0, width);
            const int left = std::max(cursor.left, cursor.right - w);
            const RECT out{ left, cursor.top, cursor.right, cursor.bottom };
            cursor.right = std::max(cursor.left, out.left - gap);
            return out;
        }

        RECT TakeRemaining() noexcept
        {
            const RECT out = cursor;
            cursor.left = cursor.right;
            return out;
        }
    };

    struct VLayout
    {
        RECT cursor{};
        int gap{};

        RECT TakeTop(int height) noexcept
        {
            const int h = std::max(0, height);
            const RECT out{ cursor.left, cursor.top,
                            cursor.right, std::min(cursor.bottom, cursor.top + h) };
            cursor.top = std::min(cursor.bottom, out.bottom + gap);
            return out;
        }

        RECT TakeBottom(int height) noexcept
        {
            const int h = std::max(0, height);
            const int top = std::max(cursor.top, cursor.bottom - h);
            const RECT out{ cursor.left, top, cursor.right, cursor.bottom };
            cursor.bottom = std::max(cursor.top, out.top - gap);
            return out;
        }

        RECT TakeRemaining() noexcept
        {
            const RECT out = cursor;
            cursor.top = cursor.bottom;
            return out;
        }
    };

    // Places an anchored popover inside env.viewport while avoiding spec.avoidBand.
    inline PopoverPlacement PlaceAnchoredPopover(
        SIZE panelSize, const AnchorSpec& spec, const LayoutEnv& env,
        int caretPadDip = 14) noexcept
    {
        const int dpi = env.dpi > 0 ? env.dpi : 96;
        const int panelW = static_cast<int>(panelSize.cx);
        const int panelH = static_cast<int>(panelSize.cy);
        const int anchorGap = MulDiv(spec.gapDip, dpi, 96);

        const int boundsL = static_cast<int>(env.viewport.left);
        const int boundsT = static_cast<int>(env.viewport.top);
        const int boundsR = static_cast<int>(env.viewport.right);
        const int boundsB = static_cast<int>(env.viewport.bottom);

        const int anchorCx = (static_cast<int>(spec.anchor.left) + static_cast<int>(spec.anchor.right)) / 2;
        const int maxX = std::max(boundsL, boundsR - panelW);
        const int x = std::clamp(anchorCx - panelW / 2, boundsL, maxX);

        const bool hasAvoid = !IsRectEmpty(&spec.avoidBand);
        const auto hitsAvoid = [&](int cy) noexcept -> bool
        {
            if (!hasAvoid)
            {
                return false;
            }
            const RECT cand{ x, cy, x + panelW, cy + panelH };
            RECT hit{};
            return IntersectRect(&hit, &cand, &spec.avoidBand) != 0;
        };

        const int subPad = MulDiv(4, dpi, 96);
        const int assemblyTop = hasAvoid
            ? std::min(static_cast<int>(spec.anchor.top) - subPad, static_cast<int>(spec.avoidBand.top))
            : (static_cast<int>(spec.anchor.top) - subPad);
        const int assemblyBottom = hasAvoid
            ? std::max(static_cast<int>(spec.anchor.bottom) + subPad, static_cast<int>(spec.avoidBand.bottom))
            : (static_cast<int>(spec.anchor.bottom) + subPad);

        int y = static_cast<int>(spec.anchor.bottom) + anchorGap;
        bool placedAbove = false;
        bool drawCaret = true;

        if (y + panelH <= boundsB && !hitsAvoid(y))
        {
            placedAbove = false;
            drawCaret = true;
        }
        else if (const int aboveY = static_cast<int>(spec.anchor.top) - anchorGap - panelH;
                 aboveY >= boundsT && !hitsAvoid(aboveY))
        {
            y = aboveY;
            placedAbove = true;
            drawCaret = true;
        }
        else if (hasAvoid && (assemblyBottom + anchorGap + panelH <= boundsB))
        {
            y = assemblyBottom + anchorGap;
            placedAbove = false;
            drawCaret = false;
        }
        else if (hasAvoid && (assemblyTop - anchorGap - panelH >= boundsT))
        {
            y = assemblyTop - anchorGap - panelH;
            placedAbove = true;
            drawCaret = false;
        }
        else
        {
            y = std::clamp(y, boundsT, std::max(boundsT, boundsB - panelH));
            drawCaret = false;
        }

        const int caretPad = MulDiv(caretPadDip, dpi, 96);
        PopoverPlacement out{};
        out.panel = RECT{ x, y, x + panelW, y + panelH };
        out.anchorCenterX = std::clamp(anchorCx, x + caretPad, x + panelW - caretPad);
        out.placedAbove = placedAbove;
        out.drawCaret = drawCaret;
        return out;
    }

    // Places a compact dropdown menu above (default) or below anchor inside viewport.
    inline RECT PlaceAnchoredMenu(
        SIZE menuSize, const RECT& anchor, const RECT& viewport,
        int gapPx, int leftOffsetPx = 0, bool preferBelow = false) noexcept
    {
        const int w = static_cast<int>(menuSize.cx);
        const int h = static_cast<int>(menuSize.cy);
        int left = static_cast<int>(anchor.left) + leftOffsetPx;
        int top = preferBelow
            ? (static_cast<int>(anchor.bottom) + gapPx)
            : (static_cast<int>(anchor.top) - gapPx - h);
        if (!IsRectEmpty(&viewport))
        {
            const int minX = static_cast<int>(viewport.left);
            const int maxX = std::max(minX, static_cast<int>(viewport.right) - w);
            left = std::clamp(left, minX, maxX);
            if (preferBelow)
            {
                if (top + h > static_cast<int>(viewport.bottom))
                {
                    top = static_cast<int>(anchor.top) - gapPx - h;
                }
            }
            else if (top < static_cast<int>(viewport.top))
            {
                top = static_cast<int>(anchor.bottom) + gapPx;
            }
            const int minY = static_cast<int>(viewport.top);
            const int maxY = std::max(minY, static_cast<int>(viewport.bottom) - h);
            top = std::clamp(top, minY, maxY);
        }
        return RECT{ left, top, left + w, top + h };
    }
}
