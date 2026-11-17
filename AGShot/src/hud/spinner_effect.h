#pragma once

#include <d2d1_1.h>
#include <winrt/base.h>

namespace agshot
{
    // The shape of the recognition spinner, in target pixels. The arc is centred
    // on the bitmap's centre, and the caller rotates it under a transform.
    struct SpinnerParameters
    {
        float radius{};
        float half_stroke{};
        float sweep{};      // radians of arc drawn
        float tail_alpha{}; // opacity at the far end of the tail
        float feather{};    // edge softness, in the same units
    };

    // Rasterizes the conic-gradient spinner arc once on the CPU into a small
    // premultiplied BGRA bitmap sized for the current DPI. Evaluating the angle
    // per pixel avoids segmented-stroke seams and eliminates runtime HLSL
    // compilation (D3DCompile).
    HRESULT CreateSpinnerBitmap(ID2D1DeviceContext* context,
                                const SpinnerParameters& parameters,
                                D2D1_COLOR_F color,
                                winrt::com_ptr<ID2D1Bitmap1>& bitmap) noexcept;
}
