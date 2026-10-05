#include "pch.h"

#include "hud/spinner_effect.h"

#include <cmath>
#include <new>

namespace agshot
{
    namespace
    {
        // {A2E4B1C7-5D3F-4A88-9E21-6B0C7F4D1A93}
        constexpr GUID kSpinnerEffectId =
            { 0xA2E4B1C7, 0x5D3F, 0x4A88, { 0x9E, 0x21, 0x6B, 0x0C, 0x7F, 0x4D, 0x1A, 0x93 } };

        // {C4F8D2A1-7E63-4B95-8A30-521D9C6E48B2}
        constexpr GUID kSpinnerShaderId =
            { 0xC4F8D2A1, 0x7E63, 0x4B95, { 0x8A, 0x30, 0x52, 0x1D, 0x9C, 0x6E, 0x48, 0xB2 } };

        // One evaluation per pixel: the ring is a soft-edged band and the fade is
        // a function of the angle, so there are no steps to see. The arc sits on
        // the origin; the caller's transform puts it where it belongs.
        constexpr char kShaderSource[] = R"(
cbuffer SpinnerConstants : register(b0)
{
    float g_radius;
    float g_halfStroke;
    float g_sweep;
    float g_tailAlpha;
    float g_feather;
};

float4 main(float4 pos : SV_POSITION, float4 uv : TEXCOORD0) : SV_TARGET
{
    float2 d = uv.xy;
    float distance = length(d);

    // A soft-edged ring, so the stroke is antialiased without a tessellated path.
    float ring = saturate((g_halfStroke + g_feather - abs(distance - g_radius)) / g_feather);

    // The tail trails backwards from the head, which points along +x.
    float angle = atan2(d.y, d.x);
    float behind = fmod(-angle + 6.28318530718, 6.28318530718);
    float fade = (behind < g_sweep) ? (1.0 - (1.0 - g_tailAlpha) * (behind / g_sweep)) : 0.0;

    float alpha = ring * fade;

    // The selection blue, premultiplied. Every pixel is written, including the
    // transparent ones. A discarded pixel keeps whatever this effect's output
    // bitmap already held from an earlier frame, and because the arc turns under
    // the caller's transform, that stale content is never overwritten - it piles
    // up until the arc is a filled disc.
    return float4(float3(0.18, 0.49, 0.94) * alpha, alpha);
}
)";

        // Compiled once and reused for every instance.
        winrt::com_ptr<ID3DBlob> g_shaderBlob;

        // The shape to rasterize. The renderer writes it just before drawing and
        // the effect reads it while that draw is processed; both happen on the
        // same thread, and there is one spinner, so a single slot is enough.
        SpinnerParameters g_next{};

        HRESULT EnsureShaderBlob() noexcept
        {
            if (g_shaderBlob)
            {
                return S_OK;
            }
            winrt::com_ptr<ID3DBlob> errors;
            return D3DCompile(
                kShaderSource, sizeof(kShaderSource) - 1, "AGShotSpinner", nullptr, nullptr,
                "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                g_shaderBlob.put(), errors.put());
        }

        bool IsSpinnerRegistered(ID2D1Factory1* factory) noexcept
        {
            UINT32 registered = 0;
            if (FAILED(factory->GetRegisteredEffects(nullptr, 0, nullptr, &registered))
                || registered == 0)
            {
                return false;
            }

            std::unique_ptr<CLSID[]> effects{ new (std::nothrow) CLSID[registered] };
            if (!effects)
            {
                return false;
            }

            UINT32 returned = 0;
            if (FAILED(factory->GetRegisteredEffects(effects.get(), registered, &returned, nullptr)))
            {
                return false;
            }
            for (UINT32 i = 0; i < returned; ++i)
            {
                if (IsEqualGUID(effects[i], kSpinnerEffectId))
                {
                    return true;
                }
            }
            return false;
        }
    }

    const GUID& SpinnerEffect::ClassId() noexcept
    {
        return kSpinnerEffectId;
    }

    SpinnerEffect::Constants SpinnerEffect::Pack(const SpinnerParameters& parameters) noexcept
    {
        Constants constants{};
        constants.radius = parameters.radius;
        constants.half_stroke = parameters.half_stroke;
        constants.sweep = parameters.sweep;
        constants.tail_alpha = parameters.tail_alpha;
        constants.feather = parameters.feather;
        return constants;
    }

    void SpinnerEffect::SetNextParameters(const SpinnerParameters& parameters) noexcept
    {
        g_next = parameters;
    }

    HRESULT CALLBACK SpinnerEffect::CreateEffect(IUnknown** effect) noexcept
    {
        if (effect == nullptr)
        {
            return E_POINTER;
        }
        *effect = static_cast<ID2D1EffectImpl*>(new (std::nothrow) SpinnerEffect());
        return *effect != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT SpinnerEffect::Register(ID2D1Factory1* factory) noexcept
    {
        if (factory == nullptr)
        {
            return E_POINTER;
        }

        // Registration is process-wide and reference-counted by Direct2D. Query
        // the actual registry instead of caching a process-wide success flag, so
        // this remains correct if another part of the process unregisters it.
        // Only the four required metadata properties and the input list can
        // declared: the schema rejects every custom Property declaration tried
        // here, with E_INVALIDARG, and the created effect reports zero
        // properties, so there is nothing to SetValue on. That is why the shape
        // is handed over through SetNextParameters and the turning is left to
        // the caller's transform - neither needs the effect to be invalidated.
        static const WCHAR kPropertyXml[] =
            L"<?xml version='1.0'?>"
            L"<Effect>"
            L"<Property name='DisplayName' type='string' value='AGShotSpinner'/>"
            L"<Property name='Author' type='string' value='AGShot'/>"
            L"<Property name='Category' type='string' value='AGShot'/>"
            L"<Property name='Description' type='string' value='Recognition spinner'/>"
            L"<Inputs/>"
            L"</Effect>";

        if (IsSpinnerRegistered(factory))
        {
            return S_OK;
        }

        const HRESULT hr = factory->RegisterEffectFromString(
            kSpinnerEffectId, kPropertyXml, nullptr, 0, &SpinnerEffect::CreateEffect);
        if (SUCCEEDED(hr) || IsSpinnerRegistered(factory))
        {
            return S_OK;
        }
        return hr;
    }

    IFACEMETHODIMP_(ULONG) SpinnerEffect::AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }

    IFACEMETHODIMP_(ULONG) SpinnerEffect::Release()
    {
        const ULONG count = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    IFACEMETHODIMP SpinnerEffect::QueryInterface(REFIID iid, void** object)
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        *object = nullptr;

        if (iid == __uuidof(IUnknown) || iid == __uuidof(ID2D1EffectImpl))
        {
            *object = static_cast<ID2D1EffectImpl*>(this);
        }
        else if (iid == __uuidof(ID2D1DrawTransform))
        {
            *object = static_cast<ID2D1DrawTransform*>(this);
        }
        else if (iid == __uuidof(ID2D1Transform))
        {
            *object = static_cast<ID2D1Transform*>(this);
        }
        else if (iid == __uuidof(ID2D1TransformNode))
        {
            *object = static_cast<ID2D1TransformNode*>(this);
        }
        else
        {
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP SpinnerEffect::Initialize(ID2D1EffectContext* context, ID2D1TransformGraph* graph)
    {
        if (context == nullptr || graph == nullptr)
        {
            return E_POINTER;
        }

        const HRESULT compiled = EnsureShaderBlob();
        if (FAILED(compiled))
        {
            return compiled;
        }

        const HRESULT loaded = context->LoadPixelShader(
            kSpinnerShaderId, static_cast<const BYTE*>(g_shaderBlob->GetBufferPointer()),
            static_cast<UINT32>(g_shaderBlob->GetBufferSize()));
        if (FAILED(loaded))
        {
            return loaded;
        }

        return graph->SetSingleTransformNode(this);
    }

    IFACEMETHODIMP SpinnerEffect::PrepareForRender(D2D1_CHANGE_TYPE)
    {
        if (!m_draw_info)
        {
            return E_FAIL;
        }
        m_constants = Pack(g_next);
        return m_draw_info->SetPixelShaderConstantBuffer(
            reinterpret_cast<const BYTE*>(&m_constants), sizeof(m_constants));
    }

    IFACEMETHODIMP SpinnerEffect::SetGraph(ID2D1TransformGraph* graph)
    {
        if (graph == nullptr)
        {
            return E_POINTER;
        }
        return graph->SetSingleTransformNode(this);
    }

    IFACEMETHODIMP_(UINT32) SpinnerEffect::GetInputCount() const
    {
        return 0;
    }

    IFACEMETHODIMP SpinnerEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L*, D2D1_RECT_L*, UINT32 input_count) const
    {
        // No inputs, so there is nothing to map onto.
        return input_count == 0 ? S_OK : E_INVALIDARG;
    }

    IFACEMETHODIMP SpinnerEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L*, const D2D1_RECT_L*, UINT32 input_count,
        D2D1_RECT_L* output, D2D1_RECT_L* output_opaque)
    {
        if (output == nullptr || output_opaque == nullptr)
        {
            return E_POINTER;
        }
        if (input_count != 0)
        {
            return E_INVALIDARG;
        }

        // A square around the origin, holding the arc and its soft edge. It does
        // not depend on where the spinner is on screen, which is what makes the
        // cached rasterization valid for every later draw.
        const float reach = g_next.radius + g_next.half_stroke + g_next.feather + 1.0f;
        *output = D2D1_RECT_L{
            static_cast<LONG>(std::floor(-reach)),
            static_cast<LONG>(std::floor(-reach)),
            static_cast<LONG>(std::ceil(reach)),
            static_cast<LONG>(std::ceil(reach)) };
        // The shader writes coverage, so nothing about the result is opaque.
        *output_opaque = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP SpinnerEffect::MapInvalidRect(
        UINT32 input_index, D2D1_RECT_L, D2D1_RECT_L* invalid_output) const
    {
        if (invalid_output == nullptr)
        {
            return E_POINTER;
        }
        if (input_index != 0)
        {
            return E_INVALIDARG;
        }
        // Without inputs, everything the effect covers has to be redrawn.
        *invalid_output = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP SpinnerEffect::SetDrawInfo(ID2D1DrawInfo* draw_info)
    {
        if (draw_info == nullptr)
        {
            return E_POINTER;
        }
        m_draw_info.copy_from(draw_info);
        return m_draw_info->SetPixelShader(kSpinnerShaderId);
    }
}
