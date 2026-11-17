#include "pch.h"

#include "hud/spinner_effect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace agshot
{
    HRESULT CreateSpinnerBitmap(ID2D1DeviceContext* context,
                                const SpinnerParameters& parameters,
                                D2D1_COLOR_F color,
                                winrt::com_ptr<ID2D1Bitmap1>& bitmap) noexcept
    {
        if (context == nullptr)
        {
            return E_POINTER;
        }
        bitmap = nullptr;

        constexpr float kTwoPi = 6.28318530718f;
        const float feather = std::max(parameters.feather, 0.001f);
        const float sweep = std::max(parameters.sweep, 0.001f);
        const float reach = parameters.radius + parameters.half_stroke + feather + 1.0f;
        const UINT32 side = std::max<UINT32>(2, static_cast<UINT32>(std::ceil(reach)) * 2);
        const float half = static_cast<float>(side) * 0.5f;

        std::vector<uint8_t> pixels;
        try
        {
            pixels.resize(static_cast<std::size_t>(side) * side * 4);
        }
        catch (...)
        {
            return E_OUTOFMEMORY;
        }

        const float red = std::clamp(color.r, 0.0f, 1.0f);
        const float green = std::clamp(color.g, 0.0f, 1.0f);
        const float blue = std::clamp(color.b, 0.0f, 1.0f);
        const float colorAlpha = std::clamp(color.a, 0.0f, 1.0f);

        for (UINT32 y = 0; y < side; ++y)
        {
            const float dy = static_cast<float>(y) + 0.5f - half;
            for (UINT32 x = 0; x < side; ++x)
            {
                const float dx = static_cast<float>(x) + 0.5f - half;
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float ring = std::clamp(
                    (parameters.half_stroke + feather - std::abs(distance - parameters.radius))
                        / feather,
                    0.0f, 1.0f);

                const float angle = std::atan2(dy, dx);
                float behind = std::fmod(-angle + kTwoPi, kTwoPi);
                if (behind < 0.0f)
                {
                    behind += kTwoPi;
                }
                const float fade = behind < sweep
                    ? (1.0f - (1.0f - parameters.tail_alpha) * (behind / sweep))
                    : 0.0f;
                const float alpha = ring * fade * colorAlpha;

                const std::size_t offset = (static_cast<std::size_t>(y) * side + x) * 4;
                pixels[offset + 0] = static_cast<uint8_t>(std::lround(blue * alpha * 255.0f));
                pixels[offset + 1] = static_cast<uint8_t>(std::lround(green * alpha * 255.0f));
                pixels[offset + 2] = static_cast<uint8_t>(std::lround(red * alpha * 255.0f));
                pixels[offset + 3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
            }
        }

        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f);
        return context->CreateBitmap(
            D2D1::SizeU(side, side),
            pixels.data(),
            side * 4,
            properties,
            bitmap.put());
    }
}
