#include "pch.h"

#include "hud/selection.h"

#include <algorithm>

namespace agshot
{
    namespace
    {
        // A selection always keeps at least this much area. Dragging a handle past
        // the opposite edge stops rather than turning the rectangle inside out,
        // which would swap the handle under the pointer for a different one.
        constexpr int kMinimumSize = 1;

        bool MovesLeftEdge(SelectionPart part) noexcept
        {
            return part == SelectionPart::Left
                || part == SelectionPart::TopLeft
                || part == SelectionPart::BottomLeft;
        }

        bool MovesRightEdge(SelectionPart part) noexcept
        {
            return part == SelectionPart::Right
                || part == SelectionPart::TopRight
                || part == SelectionPart::BottomRight;
        }

        bool MovesTopEdge(SelectionPart part) noexcept
        {
            return part == SelectionPart::Top
                || part == SelectionPart::TopLeft
                || part == SelectionPart::TopRight;
        }

        bool MovesBottomEdge(SelectionPart part) noexcept
        {
            return part == SelectionPart::Bottom
                || part == SelectionPart::BottomLeft
                || part == SelectionPart::BottomRight;
        }
    }

    std::array<RECT, kSelectionHandles> HandleRects(const RECT& selection, int handleSize) noexcept
    {
        const int half = handleSize / 2;
        const int left = selection.left;
        const int right = selection.right;
        const int top = selection.top;
        const int bottom = selection.bottom;
        const int middleX = (left + right) / 2;
        const int middleY = (top + bottom) / 2;

        const POINT centres[kSelectionHandles] =
        {
            { left,     top },      // TopLeft
            { middleX,  top },      // Top
            { right,    top },      // TopRight
            { right,    middleY },  // Right
            { right,    bottom },   // BottomRight
            { middleX,  bottom },   // Bottom
            { left,     bottom },   // BottomLeft
            { left,     middleY },  // Left
        };

        std::array<RECT, kSelectionHandles> handles{};
        for (int i = 0; i < kSelectionHandles; ++i)
        {
            handles[i] = RECT{ centres[i].x - half, centres[i].y - half,
                               centres[i].x + half, centres[i].y + half };
        }
        return handles;
    }

    SelectionPart HitTestSelection(const RECT& selection, POINT point, int handleSize) noexcept
    {
        const auto handles = HandleRects(selection, handleSize);

        // Corners first, then edges: where two handles overlap the corner is the
        // one that resizes in both directions, which is what a pointer there
        // looks like it should do.
        constexpr SelectionPart corners[kSelectionHandles] =
        {
            SelectionPart::TopLeft, SelectionPart::Top,
            SelectionPart::TopRight, SelectionPart::Right,
            SelectionPart::BottomRight, SelectionPart::Bottom,
            SelectionPart::BottomLeft, SelectionPart::Left,
        };

        // The drawn size is small; the area that answers is not.
        const int slack = std::max(handleSize / 2, 4);

        for (int i = 0; i < kSelectionHandles; ++i)
        {
            RECT area = handles[i];
            InflateRect(&area, slack, slack);
            if (PtInRect(&area, point))
            {
                return corners[i];
            }
        }

        return PtInRect(&selection, point) ? SelectionPart::Body : SelectionPart::None;
    }

    RECT DragSelection(const RECT& start, SelectionPart part, POINT origin, POINT current,
                       const RECT& bounds) noexcept
    {
        const int dx = current.x - origin.x;
        const int dy = current.y - origin.y;

        RECT result = start;

        if (part == SelectionPart::Body)
        {
            OffsetRect(&result, dx, dy);
        }
        else
        {
            if (MovesLeftEdge(part)) { result.left += dx; }
            if (MovesRightEdge(part)) { result.right += dx; }
            if (MovesTopEdge(part)) { result.top += dy; }
            if (MovesBottomEdge(part)) { result.bottom += dy; }
        }

        // Never let a dragged edge cross the one opposite it.
        if (result.right - result.left < kMinimumSize)
        {
            if (MovesLeftEdge(part)) { result.left = result.right - kMinimumSize; }
            else { result.right = result.left + kMinimumSize; }
        }
        if (result.bottom - result.top < kMinimumSize)
        {
            if (MovesTopEdge(part)) { result.top = result.bottom - kMinimumSize; }
            else { result.bottom = result.top + kMinimumSize; }
        }

        if (part == SelectionPart::Body)
        {
            // Moving pushes the whole rectangle back inside rather than clipping
            // it, so that the size the user chose survives the trip to the edge.
            if (result.left < bounds.left) { OffsetRect(&result, bounds.left - result.left, 0); }
            if (result.top < bounds.top) { OffsetRect(&result, 0, bounds.top - result.top); }
            if (result.right > bounds.right) { OffsetRect(&result, bounds.right - result.right, 0); }
            if (result.bottom > bounds.bottom) { OffsetRect(&result, 0, bounds.bottom - result.bottom); }
        }
        else
        {
            result.left = std::max(result.left, bounds.left);
            result.top = std::max(result.top, bounds.top);
            result.right = std::min(result.right, bounds.right);
            result.bottom = std::min(result.bottom, bounds.bottom);
        }

        return result;
    }

    bool IsEmptyRect(const RECT& rect) noexcept
    {
        return rect.right <= rect.left || rect.bottom <= rect.top;
    }

    LPCWSTR CursorForPart(SelectionPart part) noexcept
    {
        switch (part)
        {
        case SelectionPart::TopLeft:
        case SelectionPart::BottomRight:
            return IDC_SIZENWSE;
        case SelectionPart::TopRight:
        case SelectionPart::BottomLeft:
            return IDC_SIZENESW;
        case SelectionPart::Left:
        case SelectionPart::Right:
            return IDC_SIZEWE;
        case SelectionPart::Top:
        case SelectionPart::Bottom:
            return IDC_SIZENS;
        case SelectionPart::Body:
            return IDC_SIZEALL;
        default:
            return IDC_CROSS;
        }
    }
}
