#pragma once

#include "core/error.h"

#include <windows.h>
#include <d2d1_1.h>
#include <winrt/base.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

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
        winrt::com_ptr<ID2D1SolidColorBrush> disabledIcon;
        // The cancel mark is the one destructive control in the row, so it is
        // drawn in red rather than the shared icon colour.
        winrt::com_ptr<ID2D1SolidColorBrush> cancel;
        winrt::com_ptr<ID2D1SolidColorBrush> tooltip;
        winrt::com_ptr<ID2D1SolidColorBrush> textScrollTrack;
        winrt::com_ptr<ID2D1SolidColorBrush> textScrollThumb;
        // Shared mutable color brush and 2x2 tiled checkerboard bitmap brush so
        // immediate-mode Draw() paths avoid per-frame COM allocations and loops.
        winrt::com_ptr<ID2D1SolidColorBrush> scratch;
        winrt::com_ptr<ID2D1BitmapBrush> checkerboard;

        ID2D1SolidColorBrush* WithColor(const D2D1_COLOR_F& color) const noexcept
        {
            if (!scratch)
            {
                return nullptr;
            }
            scratch->SetColor(color);
            return scratch.get();
        }

        void Release() noexcept
        {
            checkerboard = nullptr;
            scratch = nullptr;
            textScrollThumb = nullptr;
            textScrollTrack = nullptr;
            tooltip = nullptr;
            cancel = nullptr;
            disabledIcon = nullptr;
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
        make(0.96f, 0.97f, 0.99f, 0.28f, out.disabledIcon);
        make(0.94f, 0.35f, 0.35f, 1.0f, out.cancel);      // the one destructive mark
        make(0.06f, 0.06f, 0.08f, 0.97f, out.tooltip);
        make(0.90f, 0.91f, 0.93f, 1.0f, out.textScrollTrack);
        make(0.48f, 0.51f, 0.56f, 1.0f, out.textScrollThumb);
        make(1.0f, 1.0f, 1.0f, 1.0f, out.scratch);

        // 2x2 BGRA8 checkerboard tile ([light, dark; dark, light]) wrapped with
        // nearest-neighbor sampling so any rounded swatch or slider track fills in
        // a single draw call.
        constexpr std::uint32_t kLight = 0xFFCBD0D8u;
        constexpr std::uint32_t kDark  = 0xFF7B828Du;
        const std::uint32_t pixels[4] = { kLight, kDark, kDark, kLight };
        const D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        winrt::com_ptr<ID2D1Bitmap> tile;
        AGSHOT_CHECK_HR(context->CreateBitmap(D2D1::SizeU(2, 2), pixels, 2 * sizeof(std::uint32_t), bmpProps, tile.put()));

        const D2D1_BITMAP_BRUSH_PROPERTIES brushProps = D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_WRAP,
            D2D1_EXTEND_MODE_WRAP,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        out.checkerboard = nullptr;
        AGSHOT_CHECK_HR(context->CreateBitmapBrush(tile.get(), brushProps, out.checkerboard.put()));
    }

    inline void FillTiledCheckerboard(ID2D1DeviceContext* context, const HudBrushes& brushes,
                                      const D2D1_RECT_F& rect, float cellPx, float cornerRadius = 0.0f) noexcept
    {
        if (context == nullptr || !brushes.checkerboard || cellPx <= 0.0f)
        {
            return;
        }
        brushes.checkerboard->SetTransform(
            D2D1::Matrix3x2F::Scale(cellPx, cellPx)
            * D2D1::Matrix3x2F::Translation(rect.left, rect.top));
        if (cornerRadius > 0.0f)
        {
            context->FillRoundedRectangle(D2D1::RoundedRect(rect, cornerRadius, cornerRadius), brushes.checkerboard.get());
        }
        else
        {
            context->FillRectangle(rect, brushes.checkerboard.get());
        }
    }

    inline void FillTiledCheckerboardGeometry(ID2D1DeviceContext* context, const HudBrushes& brushes,
                                              ID2D1Geometry* geometry, const D2D1_RECT_F& originRect,
                                              float cellPx) noexcept
    {
        if (context == nullptr || geometry == nullptr || !brushes.checkerboard || cellPx <= 0.0f)
        {
            return;
        }
        brushes.checkerboard->SetTransform(
            D2D1::Matrix3x2F::Scale(cellPx, cellPx)
            * D2D1::Matrix3x2F::Translation(originRect.left, originRect.top));
        context->FillGeometry(geometry, brushes.checkerboard.get());
    }

    inline constexpr float RelativeLuminance(float r, float g, float b) noexcept
    {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    inline constexpr float RelativeLuminance(const D2D1_COLOR_F& c) noexcept
    {
        return RelativeLuminance(c.r, c.g, c.b);
    }

    inline constexpr bool IsLightColor(const D2D1_COLOR_F& c, float threshold = 0.62f) noexcept
    {
        return RelativeLuminance(c) > threshold;
    }

    template <typename T, std::size_t N>
    constexpr std::size_t ClosestValueIndex(const std::array<T, N>& presets, T value) noexcept
    {
        std::size_t best = 0;
        double bestDiff = std::fabs(static_cast<double>(presets[0]) - static_cast<double>(value));
        for (std::size_t i = 1; i < N; ++i)
        {
            const double diff = std::fabs(static_cast<double>(presets[i]) - static_cast<double>(value));
            if (diff < bestDiff)
            {
                bestDiff = diff;
                best = i;
            }
        }
        return best;
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
