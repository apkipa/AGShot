#pragma once

#include "core/error.h"

#include <windows.h>
#include <d2d1_1.h>
#include <winrt/base.h>

namespace agshot
{
    inline constexpr int kHudTextSizeDip = 13;

    struct HudBrushes
    {
        winrt::com_ptr<ID2D1SolidColorBrush> dim;
        winrt::com_ptr<ID2D1SolidColorBrush> shadow;
        winrt::com_ptr<ID2D1SolidColorBrush> accent;
        winrt::com_ptr<ID2D1SolidColorBrush> handleFill;
        winrt::com_ptr<ID2D1SolidColorBrush> handleEdge;
        winrt::com_ptr<ID2D1SolidColorBrush> panel;
        winrt::com_ptr<ID2D1SolidColorBrush> panelEdge;
        winrt::com_ptr<ID2D1SolidColorBrush> hover;
        winrt::com_ptr<ID2D1SolidColorBrush> icon;
        // The cancel mark is the one destructive control in the row, so it is
        // drawn in red rather than the shared icon colour.
        winrt::com_ptr<ID2D1SolidColorBrush> cancel;
        winrt::com_ptr<ID2D1SolidColorBrush> tooltip;
        winrt::com_ptr<ID2D1SolidColorBrush> textScrollTrack;
        winrt::com_ptr<ID2D1SolidColorBrush> textScrollThumb;

        void Release() noexcept
        {
            textScrollThumb = nullptr;
            textScrollTrack = nullptr;
            tooltip = nullptr;
            cancel = nullptr;
            icon = nullptr;
            hover = nullptr;
            panelEdge = nullptr;
            panel = nullptr;
            handleEdge = nullptr;
            handleFill = nullptr;
            accent = nullptr;
            shadow = nullptr;
            dim = nullptr;
        }
    };

    inline void CreateHudBrushes(ID2D1DeviceContext* context, HudBrushes& out)
    {
        const auto make = [context](float r, float g, float b, float a,
                                    winrt::com_ptr<ID2D1SolidColorBrush>& brush)
        {
            AGSHOT_CHECK_HR(context->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), brush.put()));
        };

        make(0.0f, 0.0f, 0.0f, 0.45f, out.dim);           // everything outside the selection
        make(0.0f, 0.0f, 0.0f, 0.10f, out.shadow);        // magnifier card drop shadow
        make(0.18f, 0.49f, 0.94f, 1.0f, out.accent);      // the selection border
        make(1.0f, 1.0f, 1.0f, 1.0f, out.handleFill);
        make(0.10f, 0.10f, 0.12f, 1.0f, out.handleEdge);
        make(0.14f, 0.15f, 0.18f, 0.97f, out.panel);
        make(1.0f, 1.0f, 1.0f, 0.14f, out.panelEdge);
        make(1.0f, 1.0f, 1.0f, 0.13f, out.hover);
        make(0.96f, 0.97f, 0.99f, 1.0f, out.icon);
        make(0.94f, 0.35f, 0.35f, 1.0f, out.cancel);      // the one destructive mark
        make(0.06f, 0.06f, 0.08f, 0.97f, out.tooltip);
        make(0.90f, 0.91f, 0.93f, 1.0f, out.textScrollTrack);
        make(0.48f, 0.51f, 0.56f, 1.0f, out.textScrollThumb);
    }

    inline D2D1_RECT_F ToRectF(const RECT& rect) noexcept
    {
        return D2D1::RectF(
            static_cast<float>(rect.left), static_cast<float>(rect.top),
            static_cast<float>(rect.right), static_cast<float>(rect.bottom));
    }

    inline D2D1_ROUNDED_RECT ToRoundedRect(const RECT& rect, float radius) noexcept
    {
        return D2D1::RoundedRect(ToRectF(rect), radius, radius);
    }

    inline float ScaleByDpi(int value, int dpi) noexcept
    {
        return static_cast<float>(MulDiv(value, dpi, 96));
    }

    inline float ScaleByDpi(float value, int dpi) noexcept
    {
        return value * static_cast<float>(dpi) / 96.0f;
    }

    inline float MeasuredPixelsForDpi(float pixels, int dpi) noexcept
    {
        const float logicalSize = pixels * 96.0f / static_cast<float>(dpi);
        return ScaleByDpi(logicalSize, dpi);
    }

    inline LONGLONG PerformanceCounterFrequency() noexcept
    {
        static const LONGLONG frequency = []
        {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        return frequency;
    }
}
