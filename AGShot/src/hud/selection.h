#pragma once

#include <windows.h>

#include <array>

namespace agshot
{
    // Which part of a selection the pointer is on: the body, one of the eight
    // handles, or nothing at all.
    enum class SelectionPart
    {
        None,
        Body,
        Left, Right, Top, Bottom,
        TopLeft, TopRight, BottomLeft, BottomRight,
    };

    inline constexpr int kSelectionHandles = 8;

    // The eight handle squares, in a fixed order so that drawing and hit testing
    // cannot drift apart.
    std::array<RECT, kSelectionHandles> HandleRects(const RECT& selection, int handleSize) noexcept;

    // "handleSize" is what gets drawn; the area that answers to the pointer is
    // grown around it, because a handle that is comfortable to grab would look
    // clumsy to draw.
    SelectionPart HitTestSelection(const RECT& selection, POINT point, int handleSize) noexcept;

    // Where the selection ends up when "part" is dragged from "origin" to
    // "current". It is returned rather than written in place so that the drag can
    // always start again from the rectangle it began with, which is what stops a
    // slow drag from accumulating rounding drift.
    RECT DragSelection(const RECT& start, SelectionPart part, POINT origin, POINT current,
                       const RECT& bounds) noexcept;

    // True when the rectangle has no area, which is what a click without a drag
    // leaves behind.
    bool IsEmptyRect(const RECT& rect) noexcept;

    // The pointer shape that belongs to a part, so that a handle looks grabbable.
    LPCWSTR CursorForPart(SelectionPart part) noexcept;
}
