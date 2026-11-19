#pragma once

#include "hud/hud_theme.h"

#include <windows.h>
#include <d2d1_1.h>

#include <algorithm>
#include <utility>

namespace agshot::ui
{
    // Reusable horizontal 1D slider track geometry, hit-testing, normalization, and thumb drawing.
    class Slider1D
    {
    public:
        void Arrange(const RECT& rect) noexcept { m_rect = rect; }
        const RECT& rect() const noexcept { return m_rect; }
        bool HitTest(POINT point) const noexcept { return PtInRect(&m_rect, point) != 0; }

        float ValueFromPoint(POINT point) const noexcept
        {
            const float w = std::max(1.0f, static_cast<float>(m_rect.right - m_rect.left));
            return std::clamp(static_cast<float>(point.x - m_rect.left) / w, 0.0f, 1.0f);
        }

        void DrawThumb(ID2D1DeviceContext* context, const HudBrushes& brushes,
                       float normalizedValue, const D2D1_COLOR_F& fillColor, int dpi) const
        {
            const D2D1_RECT_F f = ToRectF(m_rect);
            const float thumbX = f.left + std::clamp(normalizedValue, 0.0f, 1.0f) * (f.right - f.left);
            const float thumbY = (f.top + f.bottom) * 0.5f;
            const float r = ScaleByDpi(5.5f, dpi);
            const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), r, r);
            context->FillEllipse(circle, brushes.WithColor(fillColor));
            context->DrawEllipse(circle, brushes.handleFill.get(), ScaleByDpi(1.6f, dpi));
        }

    private:
        RECT m_rect{};
    };

    // Reusable 2D XY pad (x: left->right [0..1], y: bottom->top [0..1]).
    class Pad2D
    {
    public:
        void Arrange(const RECT& rect) noexcept { m_rect = rect; }
        const RECT& rect() const noexcept { return m_rect; }
        bool HitTest(POINT point) const noexcept { return PtInRect(&m_rect, point) != 0; }

        std::pair<float, float> ValuesFromPoint(POINT point) const noexcept
        {
            const float w = std::max(1.0f, static_cast<float>(m_rect.right - m_rect.left));
            const float h = std::max(1.0f, static_cast<float>(m_rect.bottom - m_rect.top));
            const float x = std::clamp(static_cast<float>(point.x - m_rect.left) / w, 0.0f, 1.0f);
            const float y = std::clamp(1.0f - static_cast<float>(point.y - m_rect.top) / h, 0.0f, 1.0f);
            return { x, y };
        }

        void DrawThumb(ID2D1DeviceContext* context, const HudBrushes& brushes,
                       float normX, float normY, int dpi) const
        {
            const D2D1_RECT_F f = ToRectF(m_rect);
            const float tx = f.left + std::clamp(normX, 0.0f, 1.0f) * (f.right - f.left);
            const float ty = f.top + (1.0f - std::clamp(normY, 0.0f, 1.0f)) * (f.bottom - f.top);
            const float tr = ScaleByDpi(5.0f, dpi);
            const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(tx, ty), tr, tr);
            context->DrawEllipse(circle, brushes.handleEdge.get(), ScaleByDpi(2.5f, dpi));
            context->DrawEllipse(circle, brushes.handleFill.get(), ScaleByDpi(1.5f, dpi));
        }

    private:
        RECT m_rect{};
    };
}
