#pragma once

#include <d2d1_1.h>
#include <d2d1effectauthor.h>
#include <d3dcompiler.h>
#include <winrt/base.h>

#include <cstdint>

namespace agshot
{
    // The shape of the arc, in the effect's own space. The arc is centred on the
    // effect's origin, and where it lands on screen is the caller's business:
    // the caller draws it under a transform.
    struct SpinnerParameters
    {
        float radius{};
        float half_stroke{};
        float sweep{};      // radians of arc drawn
        float tail_alpha{}; // opacity at the far end of the tail
        float feather{};    // edge softness, in the same units
    };

    // Draws the recognition spinner as a conic gradient evaluated per pixel.
    //
    // Direct2D has no sweep gradient - only linear and radial - so the fade
    // cannot come from a brush, and splitting the arc into segments only
    // approximates it with visible steps. Evaluating the angle in a shader is
    // what makes the gradient continuous.
    //
    // The effect takes no input and always covers the same square around its
    // origin, so Direct2D rasterizes it once and reuses that for every later
    // draw. That is what the caller wants: the arc's shape never changes, and
    // the turning comes from the transform the caller draws it under, so the
    // cached rasterization stays correct and there is nothing to invalidate.
    class SpinnerEffect final : public ID2D1EffectImpl, public ID2D1DrawTransform
    {
    public:
        static const GUID& ClassId() noexcept;

        // Registers the effect with the factory. Safe to call more than once.
        static HRESULT Register(ID2D1Factory1* factory) noexcept;

        static HRESULT CALLBACK CreateEffect(IUnknown** effect) noexcept;

        // The shape to rasterize. Only the first call for a given device matters,
        // because the result is cached; see the class comment.
        static void SetNextParameters(const SpinnerParameters& parameters) noexcept;

        // IUnknown
        IFACEMETHOD_(ULONG, AddRef)() override;
        IFACEMETHOD_(ULONG, Release)() override;
        IFACEMETHOD(QueryInterface)(REFIID iid, void** object) override;

        // ID2D1EffectImpl
        IFACEMETHOD(Initialize)(ID2D1EffectContext* context, ID2D1TransformGraph* graph) override;
        IFACEMETHOD(PrepareForRender)(D2D1_CHANGE_TYPE change_type) override;
        IFACEMETHOD(SetGraph)(ID2D1TransformGraph* graph) override;

        // ID2D1TransformNode
        IFACEMETHOD_(UINT32, GetInputCount)() const override;

        // ID2D1Transform
        IFACEMETHOD(MapOutputRectToInputRects)(
            const D2D1_RECT_L* output, D2D1_RECT_L* inputs, UINT32 input_count) const override;
        IFACEMETHOD(MapInputRectsToOutputRect)(
            const D2D1_RECT_L* inputs, const D2D1_RECT_L* input_opaque, UINT32 input_count,
            D2D1_RECT_L* output, D2D1_RECT_L* output_opaque) override;
        IFACEMETHOD(MapInvalidRect)(
            UINT32 input_index, D2D1_RECT_L invalid_input, D2D1_RECT_L* invalid_output) const override;

        // ID2D1DrawTransform
        IFACEMETHOD(SetDrawInfo)(ID2D1DrawInfo* draw_info) override;

    private:
        SpinnerEffect() = default;
        ~SpinnerEffect() = default;

        // Mirrors the shader's cbuffer, field for field.
        struct Constants
        {
            float radius;
            float half_stroke;
            float sweep;
            float tail_alpha;
            float feather;
        };

        static Constants Pack(const SpinnerParameters& parameters) noexcept;

        LONG m_ref{ 1 };
        Constants m_constants{};
        winrt::com_ptr<ID2D1DrawInfo> m_draw_info;
    };
}
