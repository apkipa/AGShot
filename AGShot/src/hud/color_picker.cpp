#include "pch.h"

#include "core/error.h"
#include "hud/color_model.h"
#include "hud/color_picker.h"

namespace agshot
{
    namespace
    {
        constexpr int kPopoverWidthDip = 248;
        constexpr int kPaddingDip = 10;
        constexpr int kQuickSizeDip = 20;
        constexpr int kSvHeightDip = 128;
        constexpr int kSliderHeightDip = 12;
        constexpr int kEyedropperSizeDip = 30;
        constexpr int kReadoutHeightDip = 24;
        constexpr int kRowGapDip = 8;

        constexpr const wchar_t* kFormatLabels[3] = { L"HEX", L"HSB", L"RGB" };

        enum class TopRowSlotKind
        {
            NoneSlot,
            StrokeSolid,
            StrokeDashed,
            StrokeDotted,
            ColorPreset,
        };

        struct TopRowSlot
        {
            TopRowSlotKind kind{ TopRowSlotKind::ColorPreset };
            D2D1_COLOR_F color{};
        };

        constexpr D2D1_COLOR_F kColorRed{ 0.94f, 0.25f, 0.25f, 1.0f };
        constexpr D2D1_COLOR_F kColorOrange{ 0.98f, 0.52f, 0.16f, 1.0f };
        constexpr D2D1_COLOR_F kColorYellow{ 0.98f, 0.80f, 0.08f, 1.0f };
        constexpr D2D1_COLOR_F kColorGreen{ 0.13f, 0.77f, 0.37f, 1.0f };
        constexpr D2D1_COLOR_F kColorBlue{ 0.18f, 0.49f, 0.94f, 1.0f };
        constexpr D2D1_COLOR_F kColorPurple{ 0.65f, 0.33f, 0.96f, 1.0f };
        constexpr D2D1_COLOR_F kColorWhite{ 0.98f, 0.98f, 0.99f, 1.0f };
        constexpr D2D1_COLOR_F kColorBlack{ 0.08f, 0.09f, 0.11f, 1.0f };

        std::array<TopRowSlot, 8> TopRowSlotsFor(
            ColorPickerTarget target, bool allowNone, bool showStrokeStyles) noexcept
        {
            if (showStrokeStyles)
            {
                if (allowNone)
                {
                    return
                    {
                        TopRowSlot{ TopRowSlotKind::NoneSlot, {} },
                        TopRowSlot{ TopRowSlotKind::StrokeSolid, {} },
                        TopRowSlot{ TopRowSlotKind::StrokeDashed, {} },
                        TopRowSlot{ TopRowSlotKind::StrokeDotted, {} },
                        TopRowSlot{ TopRowSlotKind::ColorPreset, kColorRed },
                        TopRowSlot{ TopRowSlotKind::ColorPreset, kColorGreen },
                        TopRowSlot{ TopRowSlotKind::ColorPreset, kColorBlue },
                        TopRowSlot{ TopRowSlotKind::ColorPreset, kColorBlack },
                    };
                }
                return
                {
                    TopRowSlot{ TopRowSlotKind::StrokeSolid, {} },
                    TopRowSlot{ TopRowSlotKind::StrokeDashed, {} },
                    TopRowSlot{ TopRowSlotKind::StrokeDotted, {} },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, kColorRed },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, kColorOrange },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, kColorGreen },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, kColorBlue },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, kColorBlack },
                };
            }

            if (target == ColorPickerTarget::Fill)
            {
                return
                {
                    TopRowSlot{ TopRowSlotKind::NoneSlot, {} },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, D2D1_COLOR_F{ 0.94f, 0.25f, 0.25f, 0.28f } },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, D2D1_COLOR_F{ 0.13f, 0.77f, 0.37f, 0.28f } },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, D2D1_COLOR_F{ 0.18f, 0.49f, 0.94f, 0.28f } },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, D2D1_COLOR_F{ 0.98f, 0.80f, 0.08f, 0.32f } },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, kColorRed },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, kColorWhite },
                    TopRowSlot{ TopRowSlotKind::ColorPreset, kColorBlack },
                };
            }

            return
            {
                TopRowSlot{ allowNone ? TopRowSlotKind::NoneSlot : TopRowSlotKind::ColorPreset, kColorWhite },
                TopRowSlot{ TopRowSlotKind::ColorPreset, kColorRed },
                TopRowSlot{ TopRowSlotKind::ColorPreset, kColorOrange },
                TopRowSlot{ TopRowSlotKind::ColorPreset, kColorYellow },
                TopRowSlot{ TopRowSlotKind::ColorPreset, kColorGreen },
                TopRowSlot{ TopRowSlotKind::ColorPreset, kColorBlue },
                TopRowSlot{ TopRowSlotKind::ColorPreset, kColorPurple },
                TopRowSlot{ TopRowSlotKind::ColorPreset, kColorBlack },
            };
        }
    }

    ColorPickerPopover::ColorPickerPopover()
    {
        m_formatMenu.SetItems({ L"HEX", L"HSB", L"RGB" });
        m_formatMenu.SetSelectedIndex(0);
        m_formatMenu.SetLeftOffsetDip(-4);
    }

    void ColorPickerPopover::DiscardDeviceResources() noexcept
    {
        m_gpu.ResetDevice();
    }

    void ColorPickerPopover::InvalidateLayouts() noexcept
    {
        m_gpu.ResetLayouts();
        m_formatMenu.InvalidateLayouts();
        for (auto& field : m_fields)
        {
            field.InvalidateLayout();
        }
    }

    void ColorPickerPopover::EnsureStaticGradients(ID2D1DeviceContext* context) const
    {
        if (m_gpu.context != context)
        {
            m_gpu.ResetDevice();
            m_gpu.context = context;
        }
        if (!m_gpu.svVertBrush)
        {
            const D2D1_GRADIENT_STOP vStops[2] = {
                { 0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) },
                { 1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f) },
            };
            winrt::com_ptr<ID2D1GradientStopCollection> vColl;
            AGSHOT_CHECK_HR(context->CreateGradientStopCollection(vStops, 2, vColl.put()));
            AGSHOT_CHECK_HR(context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 1.0f)),
                vColl.get(), m_gpu.svVertBrush.put()));
        }
        if (!m_gpu.hueBrush)
        {
            const D2D1_GRADIENT_STOP stops[7] = {
                { 0.00f, D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f) },
                { 1.0f / 6.0f, D2D1::ColorF(1.0f, 1.0f, 0.0f, 1.0f) },
                { 2.0f / 6.0f, D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f) },
                { 3.0f / 6.0f, D2D1::ColorF(0.0f, 1.0f, 1.0f, 1.0f) },
                { 4.0f / 6.0f, D2D1::ColorF(0.0f, 0.0f, 1.0f, 1.0f) },
                { 5.0f / 6.0f, D2D1::ColorF(1.0f, 0.0f, 1.0f, 1.0f) },
                { 1.00f, D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f) },
            };
            winrt::com_ptr<ID2D1GradientStopCollection> coll;
            AGSHOT_CHECK_HR(context->CreateGradientStopCollection(stops, 7, coll.put()));
            AGSHOT_CHECK_HR(context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(1.0f, 0.0f)),
                coll.get(), m_gpu.hueBrush.put()));
        }
    }

    ID2D1LinearGradientBrush* ColorPickerPopover::EnsureSvHorizBrush(
        ID2D1DeviceContext* context, const D2D1_COLOR_F& pureHue) const
    {
        if (!m_gpu.svHorizBrush || std::fabs(m_gpu.svHorizHue - m_hue) > 0.05f)
        {
            const D2D1_GRADIENT_STOP hStops[2] = {
                { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) },
                { 1.0f, pureHue },
            };
            winrt::com_ptr<ID2D1GradientStopCollection> hColl;
            AGSHOT_CHECK_HR(context->CreateGradientStopCollection(hStops, 2, hColl.put()));
            m_gpu.svHorizBrush = nullptr;
            AGSHOT_CHECK_HR(context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(1.0f, 0.0f)),
                hColl.get(), m_gpu.svHorizBrush.put()));
            m_gpu.svHorizHue = m_hue;
        }
        return m_gpu.svHorizBrush.get();
    }

    ID2D1LinearGradientBrush* ColorPickerPopover::EnsureAlphaBrush(ID2D1DeviceContext* context) const
    {
        if (!m_gpu.alphaBrush
            || std::fabs(m_gpu.alphaRgb.r - m_color.r) > 0.001f
            || std::fabs(m_gpu.alphaRgb.g - m_color.g) > 0.001f
            || std::fabs(m_gpu.alphaRgb.b - m_color.b) > 0.001f)
        {
            const D2D1_GRADIENT_STOP stops[2] = {
                { 0.0f, D2D1_COLOR_F{ m_color.r, m_color.g, m_color.b, 0.0f } },
                { 1.0f, D2D1_COLOR_F{ m_color.r, m_color.g, m_color.b, 1.0f } },
            };
            winrt::com_ptr<ID2D1GradientStopCollection> coll;
            AGSHOT_CHECK_HR(context->CreateGradientStopCollection(stops, 2, coll.put()));
            m_gpu.alphaBrush = nullptr;
            AGSHOT_CHECK_HR(context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(1.0f, 0.0f)),
                coll.get(), m_gpu.alphaBrush.put()));
            m_gpu.alphaRgb = D2D1_COLOR_F{ m_color.r, m_color.g, m_color.b, 1.0f };
        }
        return m_gpu.alphaBrush.get();
    }

    IDWriteTextLayout* ColorPickerPopover::EnsureFormatButtonLayout(
        IDWriteFactory* dwrite, IDWriteTextFormat* textFormat, float w, float h) const
    {
        if (dwrite == nullptr || textFormat == nullptr)
        {
            return nullptr;
        }
        if (!m_gpu.fmtLayout
            || m_gpu.fmtFactory != dwrite
            || m_gpu.fmtFormat != textFormat
            || m_gpu.fmtMode != m_formatMode
            || std::fabs(m_gpu.fmtWidth - w) > 0.25f
            || std::fabs(m_gpu.fmtHeight - h) > 0.25f)
        {
            const wchar_t* label = kFormatLabels[static_cast<int>(m_formatMode)];
            m_gpu.fmtLayout = nullptr;
            if (SUCCEEDED(dwrite->CreateTextLayout(
                label, static_cast<UINT32>(wcslen(label)), textFormat,
                std::max(4.0f, w), std::max(1.0f, h), m_gpu.fmtLayout.put())))
            {
                m_gpu.fmtLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_gpu.fmtLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_gpu.fmtLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                m_gpu.fmtFactory = dwrite;
                m_gpu.fmtFormat = textFormat;
                m_gpu.fmtMode = m_formatMode;
                m_gpu.fmtWidth = w;
                m_gpu.fmtHeight = h;
            }
        }
        return m_gpu.fmtLayout.get();
    }

    float ColorPickerPopover::ClampAlphaForMode(float a) const noexcept
    {
        return m_allowNone ? std::clamp(a, 0.0f, 1.0f) : std::clamp(a, 0.01f, 1.0f);
    }

    float ColorPickerPopover::EffectiveFallbackAlpha() const noexcept
    {
        if (m_alpha >= kVisibleAlphaThreshold)
        {
            return m_alpha;
        }
        return (m_restoreAlpha >= kVisibleAlphaThreshold)
            ? std::clamp(m_restoreAlpha, 0.01f, 1.0f)
            : 1.0f;
    }

    void ColorPickerPopover::BumpOpaqueIfTransparent() noexcept
    {
        if (m_alpha < kVisibleAlphaThreshold)
        {
            m_alpha = (m_restoreAlpha >= kVisibleAlphaThreshold)
                ? std::clamp(m_restoreAlpha, 0.01f, 1.0f)
                : 1.0f;
        }
    }

    void ColorPickerPopover::RememberNonZeroAlpha() noexcept
    {
        if (m_alpha >= kVisibleAlphaThreshold)
        {
            m_restoreAlpha = std::clamp(m_alpha, 0.01f, 1.0f);
        }
    }

    SIZE ColorPickerPopover::Measure(int dpi) const noexcept
    {
        const int useDpi = dpi > 0 ? dpi : 96;
        const int panelW = MulDiv(kPopoverWidthDip, useDpi, 96);
        const int pad = MulDiv(kPaddingDip, useDpi, 96);
        const int quickSize = MulDiv(kQuickSizeDip, useDpi, 96);
        const int svH = MulDiv(kSvHeightDip, useDpi, 96);
        const int sliderH = MulDiv(kSliderHeightDip, useDpi, 96);
        const int eyedropperSize = MulDiv(kEyedropperSizeDip, useDpi, 96);
        const int readoutH = MulDiv(kReadoutHeightDip, useDpi, 96);
        const int rowGap = MulDiv(kRowGapDip, useDpi, 96);
        const int slidersAreaH = std::max(eyedropperSize, sliderH * 2 + rowGap);
        const int panelH = pad * 2 + quickSize + rowGap + svH + rowGap + slidersAreaH + rowGap + readoutH;
        return SIZE{ panelW, panelH };
    }

    void ColorPickerPopover::ArrangeReadoutRow(const ui::LayoutEnv& env) noexcept
    {
        const int dpi = env.dpi > 0 ? env.dpi : 96;
        const int pad = MulDiv(kPaddingDip, dpi, 96);
        const int readoutH = MulDiv(kReadoutHeightDip, dpi, 96);
        const RECT content = ui::PadRect(m_placement.panel, pad);
        const RECT bottomRow{ content.left, content.bottom - readoutH, content.right, content.bottom };

        ui::HLayout row{ bottomRow, MulDiv(5, dpi, 96) };
        m_formatBtnRect = row.TakeLeft(MulDiv(46, dpi, 96));
        m_fields[3].Arrange(row.TakeRight(MulDiv(48, dpi, 96)), dpi, nullptr, L"%");

        const RECT mid = row.TakeRemaining();
        if (m_formatMode == ColorFormatMode::Hex)
        {
            m_fields[0].Arrange(mid, dpi, L"#", nullptr);
            m_fields[1].Clear();
            m_fields[2].Clear();
        }
        else
        {
            const int chGap = MulDiv(4, dpi, 96);
            const int availW = static_cast<int>(mid.right - mid.left) - chGap * 2;
            const int w0 = (m_formatMode == ColorFormatMode::Hsb)
                ? MulDiv(34, dpi, 96)
                : (availW / 3);
            const int w1 = (availW - w0) / 2;
            ui::HLayout chRow{ mid, chGap };
            m_fields[0].Arrange(chRow.TakeLeft(w0), dpi, nullptr, nullptr);
            const wchar_t* suf = (m_formatMode == ColorFormatMode::Hsb) ? L"%" : nullptr;
            m_fields[1].Arrange(chRow.TakeLeft(w1), dpi, nullptr, suf);
            m_fields[2].Arrange(chRow.TakeRemaining(), dpi, nullptr, suf);
        }

        m_formatMenu.Arrange(m_formatBtnRect, env);
    }

    void ColorPickerPopover::Arrange(const ui::AnchorSpec& spec, const ui::LayoutEnv& env) noexcept
    {
        m_env = ui::LayoutEnv{ env.viewport, env.dpi > 0 ? env.dpi : 96 };
        const int dpi = m_env.dpi;
        m_placement = ui::PlaceAnchoredPopover(Measure(dpi), spec, m_env);

        const int pad = MulDiv(kPaddingDip, dpi, 96);
        const int quickSize = MulDiv(kQuickSizeDip, dpi, 96);
        const int svH = MulDiv(kSvHeightDip, dpi, 96);
        const int sliderH = MulDiv(kSliderHeightDip, dpi, 96);
        const int eyedropperSize = MulDiv(kEyedropperSizeDip, dpi, 96);
        const int rowGap = MulDiv(kRowGapDip, dpi, 96);
        const int slidersAreaH = std::max(eyedropperSize, sliderH * 2 + rowGap);

        ui::VLayout col{ ui::PadRect(m_placement.panel, pad), rowGap };

        const RECT quickRow = col.TakeTop(quickSize);
        const int quickStep = (static_cast<int>(quickRow.right - quickRow.left) - quickSize) / 7;
        for (int i = 0; i < 8; ++i)
        {
            const int qx = static_cast<int>(quickRow.left) + i * quickStep;
            m_quickSwatches[static_cast<std::size_t>(i)] =
                RECT{ qx, quickRow.top, qx + quickSize, quickRow.bottom };
        }

        m_svPad.Arrange(col.TakeTop(svH));

        ui::HLayout midRow{ col.TakeTop(slidersAreaH), rowGap };
        m_eyedropperRect = ui::CenterVertically(midRow.TakeRight(eyedropperSize), eyedropperSize);
        ui::VLayout sliders{ midRow.TakeRemaining(), rowGap };
        m_hueSlider.Arrange(sliders.TakeTop(sliderH));
        m_alphaSlider.Arrange(sliders.TakeBottom(sliderH));

        ArrangeReadoutRow(m_env);
    }

    void ColorPickerPopover::Open(
        ColorPickerTarget target, bool allowNone, bool showStrokeStyles,
        AnnotationStrokeStyle strokeStyle, D2D1_COLOR_F initialColor,
        const RECT& anchor, const RECT& bounds, int dpi,
        const RECT& avoidBand, float restoreAlpha) noexcept
    {
        m_visible = true;
        m_suspended = false;
        m_target = target;
        m_formatMenu.Close();
        m_formatMenu.SetSelectedIndex(static_cast<int>(m_formatMode));
        m_allowNone = allowNone;
        m_showStrokeStyles = showStrokeStyles;
        m_strokeStyle = strokeStyle;
        m_color = initialColor;
        m_color.a = ClampAlphaForMode(m_color.a);
        m_restoreAlpha = (m_color.a >= kVisibleAlphaThreshold)
            ? std::clamp(m_color.a, 0.01f, 1.0f)
            : ((restoreAlpha >= kVisibleAlphaThreshold) ? std::clamp(restoreAlpha, 0.01f, 1.0f) : 1.0f);
        m_drag = ColorPickerDrag::None;
        m_channelWheelAccum.Reset();
        m_activeField = ColorPickerTextField::None;
        m_input.Clear();
        m_input.ResetClicks();
        ClearHover();
        SyncHsvFromRgba();
        Arrange(ui::AnchorSpec{ anchor, avoidBand, 7 }, ui::LayoutEnv{ bounds, dpi });
    }

    void ColorPickerPopover::Open(
        ColorPickerTarget target, bool allowNone,
        D2D1_COLOR_F initialColor, const RECT& anchor,
        const RECT& bounds, int dpi, const RECT& avoidBand, float restoreAlpha) noexcept
    {
        Open(target, allowNone, false, AnnotationStrokeStyle::Solid,
             initialColor, anchor, bounds, dpi, avoidBand, restoreAlpha);
    }

    void ColorPickerPopover::Close() noexcept
    {
        m_visible = false;
        m_suspended = false;
        m_target = ColorPickerTarget::None;
        m_formatMenu.Close();
        m_showStrokeStyles = false;
        m_drag = ColorPickerDrag::None;
        m_channelWheelAccum.Reset();
        m_activeField = ColorPickerTextField::None;
        m_input.Clear();
        m_input.ResetClicks();
        ClearHover();
    }

    void ColorPickerPopover::Reposition(
        const RECT& anchor, const RECT& bounds, int dpi, const RECT& avoidBand) noexcept
    {
        if (m_visible)
        {
            Arrange(ui::AnchorSpec{ anchor, avoidBand, 7 }, ui::LayoutEnv{ bounds, dpi });
        }
    }

    void ColorPickerPopover::SuspendForEyedropper() noexcept
    {
        if (m_visible)
        {
            m_suspended = true;
            m_formatMenu.Close();
            m_drag = ColorPickerDrag::None;
            m_input.EndDrag();
            ClearHover();
        }
    }

    void ColorPickerPopover::ResumeFromEyedropper() noexcept
    {
        if (m_visible)
        {
            m_suspended = false;
        }
    }

    void ColorPickerPopover::SetPickedPixelColor(D2D1_COLOR_F rgb, int dpi) noexcept
    {
        (void)dpi;
        m_color = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 1.0f);
        m_alpha = 1.0f;
        m_restoreAlpha = 1.0f;
        SyncHsvFromRgba();
        SyncEditBufferFromColor();
    }

    void ColorPickerPopover::SetFormatMode(ColorFormatMode mode, int dpi) noexcept
    {
        m_formatMode = mode;
        m_formatMenu.SetSelectedIndex(static_cast<int>(mode));
        if (m_activeField != ColorPickerTextField::None && m_activeField != ColorPickerTextField::Alpha)
        {
            m_activeField = ColorPickerTextField::None;
            m_input.Clear();
        }
        ui::LayoutEnv env = m_env;
        if (dpi > 0)
        {
            env.dpi = dpi;
        }
        ArrangeReadoutRow(env);
    }

    void ColorPickerPopover::SyncRgbaFromHsv() noexcept
    {
        m_color = HsvToRgb(m_hue, m_saturation, m_value, m_alpha);
    }

    void ColorPickerPopover::SyncHsvFromRgba() noexcept
    {
        RgbToHsv(m_color, m_hue, m_saturation, m_value);
        m_alpha = ClampAlphaForMode(m_color.a);
        m_color.a = m_alpha;
    }

    std::size_t ColorPickerPopover::SlotIndexFor(ColorPickerTextField field) const noexcept
    {
        switch (field)
        {
        case ColorPickerTextField::Hex:
        case ColorPickerTextField::Channel0: return 0;
        case ColorPickerTextField::Channel1: return 1;
        case ColorPickerTextField::Channel2: return 2;
        case ColorPickerTextField::Alpha:    return 3;
        default:                             return 0;
        }
    }

    std::wstring ColorPickerPopover::FormatFieldString(ColorPickerTextField field) const
    {
        if (field == ColorPickerTextField::Hex)
        {
            return FormatHexRgbString(m_color);
        }
        if (field == ColorPickerTextField::Alpha)
        {
            return FormatAlphaPercentNumber(m_alpha);
        }
        int val = 0;
        if (m_formatMode == ColorFormatMode::Hsb)
        {
            if (field == ColorPickerTextField::Channel0)
            {
                val = std::clamp(static_cast<int>(std::lround(m_hue)), 0, 360);
            }
            else if (field == ColorPickerTextField::Channel1)
            {
                val = std::clamp(static_cast<int>(std::lround(m_saturation * 100.0f)), 0, 100);
            }
            else if (field == ColorPickerTextField::Channel2)
            {
                val = std::clamp(static_cast<int>(std::lround(m_value * 100.0f)), 0, 100);
            }
        }
        else if (m_formatMode == ColorFormatMode::Rgb)
        {
            const float comp = (field == ColorPickerTextField::Channel0) ? m_color.r
                : (field == ColorPickerTextField::Channel1) ? m_color.g
                : m_color.b;
            val = std::clamp(static_cast<int>(std::lround(comp * 255.0f)), 0, 255);
        }
        wchar_t buf[16]{};
        swprintf_s(buf, L"%d", val);
        return std::wstring(buf);
    }

    std::wstring ColorPickerPopover::FormatCopyText(bool selectedOnly) const
    {
        if (m_input.HasSelection())
        {
            const auto [s, e] = m_input.SelectionRange();
            std::wstring copy = m_input.text().substr(s, e - s);
            if (m_activeField == ColorPickerTextField::Hex && s == 0 && e == m_input.text().size())
            {
                copy.insert(copy.begin(), L'#');
            }
            return copy;
        }
        if (selectedOnly || m_input.text().empty())
        {
            return {};
        }
        std::wstring copy = m_input.text();
        if (m_activeField == ColorPickerTextField::Hex)
        {
            copy.insert(copy.begin(), L'#');
        }
        else if (m_activeField == ColorPickerTextField::Alpha)
        {
            copy.push_back(L'%');
        }
        return copy;
    }

    ColorPickerTextField ColorPickerPopover::HitTestEditableField(POINT point) const noexcept
    {
        if (m_fields[3].HitTest(point))
        {
            return ColorPickerTextField::Alpha;
        }
        if (m_formatMode == ColorFormatMode::Hex)
        {
            return m_fields[0].HitTest(point) ? ColorPickerTextField::Hex : ColorPickerTextField::None;
        }
        constexpr ColorPickerTextField kChannels[3] = {
            ColorPickerTextField::Channel0,
            ColorPickerTextField::Channel1,
            ColorPickerTextField::Channel2,
        };
        for (std::size_t i = 0; i < 3; ++i)
        {
            if (m_fields[i].HitTest(point))
            {
                return kChannels[i];
            }
        }
        return ColorPickerTextField::None;
    }

    void ColorPickerPopover::SyncEditBufferFromColor() noexcept
    {
        if (m_activeField != ColorPickerTextField::None)
        {
            m_input.SetText(FormatFieldString(m_activeField), true);
        }
    }

    void ColorPickerPopover::FocusTextField(ColorPickerTextField field) noexcept
    {
        m_activeField = field;
        m_fieldStartColor = m_color;
        SyncEditBufferFromColor();
    }

    bool ColorPickerPopover::ApplyLiveTextParse() noexcept
    {
        if (m_activeField == ColorPickerTextField::Hex)
        {
            D2D1_COLOR_F parsed{};
            bool hadExplicitAlpha = false;
            if (ParseHexColorDigits(m_input.text(), false, EffectiveFallbackAlpha(), parsed, hadExplicitAlpha))
            {
                parsed.a = ClampAlphaForMode(parsed.a);
                m_color = parsed;
                SyncHsvFromRgba();
                RememberNonZeroAlpha();
                return true;
            }
            return false;
        }

        int val = 0;
        if (!ParseUnsignedIntDigits(m_input.text(), val))
        {
            return false;
        }

        if (m_activeField == ColorPickerTextField::Alpha)
        {
            m_alpha = ClampAlphaForMode(static_cast<float>(std::clamp(val, 0, 100)) / 100.0f);
            SyncRgbaFromHsv();
            RememberNonZeroAlpha();
            return true;
        }

        if (m_formatMode == ColorFormatMode::Hsb)
        {
            if (m_activeField == ColorPickerTextField::Channel0)
            {
                m_hue = static_cast<float>(std::clamp(val, 0, 360));
            }
            else if (m_activeField == ColorPickerTextField::Channel1)
            {
                m_saturation = static_cast<float>(std::clamp(val, 0, 100)) / 100.0f;
            }
            else if (m_activeField == ColorPickerTextField::Channel2)
            {
                m_value = static_cast<float>(std::clamp(val, 0, 100)) / 100.0f;
            }
            else
            {
                return false;
            }
            BumpOpaqueIfTransparent();
            SyncRgbaFromHsv();
            RememberNonZeroAlpha();
            return true;
        }

        if (m_formatMode == ColorFormatMode::Rgb)
        {
            const float comp = static_cast<float>(std::clamp(val, 0, 255)) / 255.0f;
            if (m_activeField == ColorPickerTextField::Channel0) m_color.r = comp;
            else if (m_activeField == ColorPickerTextField::Channel1) m_color.g = comp;
            else if (m_activeField == ColorPickerTextField::Channel2) m_color.b = comp;
            else return false;

            if (m_color.a < kVisibleAlphaThreshold)
            {
                BumpOpaqueIfTransparent();
                m_color.a = m_alpha;
            }
            SyncHsvFromRgba();
            RememberNonZeroAlpha();
            return true;
        }
        return false;
    }

    void ColorPickerPopover::CommitTextEdit(bool& outChanged) noexcept
    {
        outChanged = false;
        if (m_activeField == ColorPickerTextField::None)
        {
            return;
        }

        bool valid = false;
        if (m_activeField == ColorPickerTextField::Hex)
        {
            D2D1_COLOR_F parsed{};
            bool hadExplicitAlpha = false;
            if (ParseHexColorDigits(m_input.text(), true, EffectiveFallbackAlpha(), parsed, hadExplicitAlpha))
            {
                parsed.a = ClampAlphaForMode(parsed.a);
                m_color = parsed;
                SyncHsvFromRgba();
                RememberNonZeroAlpha();
                valid = true;
            }
        }
        else
        {
            valid = ApplyLiveTextParse();
        }

        if (valid)
        {
            outChanged = !SameRgbaColor(m_color, m_fieldStartColor);
        }
        else
        {
            outChanged = !SameRgbaColor(m_color, m_fieldStartColor);
            m_color = m_fieldStartColor;
            SyncHsvFromRgba();
            RememberNonZeroAlpha();
        }

        m_activeField = ColorPickerTextField::None;
        m_input.Clear();
    }

    void ColorPickerPopover::CancelTextEdit(bool& outChanged) noexcept
    {
        outChanged = false;
        if (m_activeField == ColorPickerTextField::None)
        {
            return;
        }
        outChanged = !SameRgbaColor(m_color, m_fieldStartColor);
        m_color = m_fieldStartColor;
        SyncHsvFromRgba();
        RememberNonZeroAlpha();
        m_activeField = ColorPickerTextField::None;
        m_input.Clear();
    }

    void ColorPickerPopover::ReplaceFieldSelection(std::wstring_view replacement, bool coalesceSingleChar)
    {
        const std::size_t maxLen = (m_activeField == ColorPickerTextField::Hex) ? 8u : 3u;
        m_input.ReplaceSelectionBounded(replacement, maxLen, coalesceSingleChar);
    }

    bool ColorPickerPopover::StepNumericField(ColorPickerTextField field, int deltaSteps) noexcept
    {
        if (field == ColorPickerTextField::Alpha)
        {
            const int minPct = m_allowNone ? 0 : 1;
            const int curPct = std::clamp(static_cast<int>(std::lround(m_alpha * 100.0f)), minPct, 100);
            m_alpha = static_cast<float>(std::clamp(curPct + deltaSteps, minPct, 100)) / 100.0f;
            SyncRgbaFromHsv();
            RememberNonZeroAlpha();
            return true;
        }
        if (m_formatMode == ColorFormatMode::Hsb)
        {
            if (field == ColorPickerTextField::Channel0)
            {
                const int cur = std::clamp(static_cast<int>(std::lround(m_hue)), 0, 360);
                m_hue = static_cast<float>(std::clamp(cur + deltaSteps, 0, 360));
            }
            else if (field == ColorPickerTextField::Channel1)
            {
                const int cur = std::clamp(static_cast<int>(std::lround(m_saturation * 100.0f)), 0, 100);
                m_saturation = static_cast<float>(std::clamp(cur + deltaSteps, 0, 100)) / 100.0f;
            }
            else if (field == ColorPickerTextField::Channel2)
            {
                const int cur = std::clamp(static_cast<int>(std::lround(m_value * 100.0f)), 0, 100);
                m_value = static_cast<float>(std::clamp(cur + deltaSteps, 0, 100)) / 100.0f;
            }
            else
            {
                return false;
            }
            BumpOpaqueIfTransparent();
            SyncRgbaFromHsv();
            RememberNonZeroAlpha();
            return true;
        }
        if (m_formatMode == ColorFormatMode::Rgb)
        {
            float* comp = nullptr;
            if (field == ColorPickerTextField::Channel0) comp = &m_color.r;
            else if (field == ColorPickerTextField::Channel1) comp = &m_color.g;
            else if (field == ColorPickerTextField::Channel2) comp = &m_color.b;
            if (comp != nullptr)
            {
                const int cur = std::clamp(static_cast<int>(std::lround(*comp * 255.0f)), 0, 255);
                *comp = static_cast<float>(std::clamp(cur + deltaSteps, 0, 255)) / 255.0f;
                if (m_color.a < kVisibleAlphaThreshold)
                {
                    BumpOpaqueIfTransparent();
                    m_color.a = m_alpha;
                }
                SyncHsvFromRgba();
                RememberNonZeroAlpha();
                return true;
            }
        }
        return false;
    }

    void ColorPickerPopover::UpdateDragValue(POINT point) noexcept
    {
        if (m_drag == ColorPickerDrag::SaturationValue)
        {
            BumpOpaqueIfTransparent();
            const auto [s, v] = m_svPad.ValuesFromPoint(point);
            m_saturation = s;
            m_value = v;
        }
        else if (m_drag == ColorPickerDrag::Hue)
        {
            BumpOpaqueIfTransparent();
            m_hue = m_hueSlider.ValueFromPoint(point) * 360.0f;
        }
        else if (m_drag == ColorPickerDrag::Alpha)
        {
            m_alpha = ClampAlphaForMode(m_alphaSlider.ValueFromPoint(point));
        }
        SyncRgbaFromHsv();
    }

    bool ColorPickerPopover::HitTestPanel(POINT point) const noexcept
    {
        return visible()
            && (PtInRect(&m_placement.panel, point) != 0 || m_formatMenu.HitTest(point));
    }

    bool ColorPickerPopover::UpdateHover(POINT point) noexcept
    {
        if (!HitTestPanel(point))
        {
            const bool hadFieldHover = std::any_of(
                m_fields.begin(), m_fields.end(), [](const auto& f) { return f.hovered; });
            const bool hadHover = m_eyedropperHovered || m_formatBtnHovered
                || (m_formatMenu.hoveredIndex() >= 0) || hadFieldHover || (m_hoveredQuick >= 0);
            ClearHover();
            return hadHover;
        }

        bool changed = false;
        if (m_formatMenu.HitTest(point))
        {
            const int prevMenu = m_formatMenu.hoveredIndex();
            const bool hadOther = m_eyedropperHovered || m_formatBtnHovered || (m_hoveredQuick >= 0)
                || std::any_of(m_fields.begin(), m_fields.end(), [](const auto& f) { return f.hovered; });
            ClearHover();
            m_formatMenu.UpdateHover(point);
            return hadOther || (m_formatMenu.hoveredIndex() != prevMenu);
        }

        changed |= m_formatMenu.UpdateHover(point);

        const bool nextEye = PtInRect(&m_eyedropperRect, point) != 0;
        const bool nextFmt = PtInRect(&m_formatBtnRect, point) != 0;
        changed |= (nextEye != m_eyedropperHovered) || (nextFmt != m_formatBtnHovered);
        m_eyedropperHovered = nextEye;
        m_formatBtnHovered = nextFmt;

        for (auto& field : m_fields)
        {
            const bool h = field.HitTest(point);
            changed |= (h != field.hovered);
            field.hovered = h;
        }

        int nextQuick = -1;
        for (std::size_t i = 0; i < m_quickSwatches.size(); ++i)
        {
            if (PtInRect(&m_quickSwatches[i], point))
            {
                nextQuick = static_cast<int>(i);
                break;
            }
        }
        changed |= (nextQuick != m_hoveredQuick);
        m_hoveredQuick = nextQuick;
        return changed;
    }

    void ColorPickerPopover::ClearHover() noexcept
    {
        m_eyedropperHovered = false;
        m_formatBtnHovered = false;
        m_hoveredQuick = -1;
        m_formatMenu.ClearHover();
        for (auto& field : m_fields)
        {
            field.hovered = false;
        }
    }

    LPCWSTR ColorPickerPopover::CursorForPoint(POINT point) const noexcept
    {
        if (!HitTestPanel(point))
        {
            return IDC_ARROW;
        }
        if (m_formatMenu.HitTest(point))
        {
            return m_formatMenu.CursorForPoint(point);
        }
        if (m_svPad.HitTest(point))
        {
            return IDC_CROSS;
        }
        if (HitTestEditableField(point) != ColorPickerTextField::None)
        {
            return IDC_IBEAM;
        }
        if (m_hueSlider.HitTest(point) || m_alphaSlider.HitTest(point)
            || PtInRect(&m_eyedropperRect, point) || PtInRect(&m_formatBtnRect, point))
        {
            return IDC_HAND;
        }
        for (const RECT& q : m_quickSwatches)
        {
            if (PtInRect(&q, point))
            {
                return IDC_HAND;
            }
        }
        return IDC_ARROW;
    }

    bool ColorPickerPopover::ApplyPastedClipboard(HWND window, bool allowHexShorthand)
    {
        const std::wstring raw = ReadTextFromClipboard(window, false);
        const std::wstring filtered = (m_activeField == ColorPickerTextField::Hex)
            ? SanitizeHexClipboardText(raw)
            : SanitizeDigitsClipboardText(raw);
        if (filtered.empty())
        {
            return false;
        }
        ReplaceFieldSelection(filtered, false);
        if (m_activeField == ColorPickerTextField::Hex && allowHexShorthand)
        {
            D2D1_COLOR_F parsed{};
            bool hadExplicitAlpha = false;
            if (ParseHexColorDigits(m_input.text(), true, EffectiveFallbackAlpha(), parsed, hadExplicitAlpha))
            {
                parsed.a = ClampAlphaForMode(parsed.a);
                m_color = parsed;
                SyncHsvFromRgba();
                RememberNonZeroAlpha();
                m_input.SetText(FormatHexRgbString(m_color), false);
                return true;
            }
        }
        return ApplyLiveTextParse();
    }

    bool ColorPickerPopover::OnLeftButtonDown(
        POINT point, IDWriteFactory* dwrite, IDWriteTextFormat* textFormat, int dpi,
        bool& outRequestEyedropper, bool& outChanged, KeyModifiers mods)
    {
        outRequestEyedropper = false;
        outChanged = false;
        if (!HitTestPanel(point))
        {
            return false;
        }

        int chosenFmt = -1;
        if (m_formatMenu.OnLeftDown(point, chosenFmt))
        {
            if (chosenFmt >= 0)
            {
                CommitTextEdit(outChanged);
                SetFormatMode(static_cast<ColorFormatMode>(chosenFmt), dpi);
            }
            return true;
        }

        if (PtInRect(&m_formatBtnRect, point))
        {
            CommitTextEdit(outChanged);
            if (m_formatMenu.isOpen())
            {
                m_formatMenu.Close();
            }
            else
            {
                ui::LayoutEnv env = m_env;
                if (dpi > 0) env.dpi = dpi;
                m_formatMenu.Open(m_formatBtnRect, env);
            }
            m_drag = ColorPickerDrag::None;
            m_input.EndDrag();
            return true;
        }

        m_formatMenu.Close();

        const ColorPickerTextField targetField = HitTestEditableField(point);
        if (targetField != ColorPickerTextField::None)
        {
            const bool alreadyFocused = (m_activeField == targetField);
            if (!alreadyFocused)
            {
                CommitTextEdit(outChanged);
                FocusTextField(targetField);
            }
            const bool draggingSel = m_input.OnLeftButtonDown(
                point, alreadyFocused, m_fields[SlotIndexFor(targetField)],
                dwrite, textFormat, mods);
            m_drag = draggingSel ? ColorPickerDrag::TextSelection : ColorPickerDrag::None;
            return true;
        }

        m_input.ResetClicks();
        CommitTextEdit(outChanged);

        const auto slots = TopRowSlotsFor(m_target, m_allowNone, m_showStrokeStyles);
        for (std::size_t i = 0; i < m_quickSwatches.size(); ++i)
        {
            if (PtInRect(&m_quickSwatches[i], point))
            {
                const TopRowSlot& slot = slots[i];
                switch (slot.kind)
                {
                case TopRowSlotKind::NoneSlot:
                    m_alpha = 0.0f;
                    SyncRgbaFromHsv();
                    break;
                case TopRowSlotKind::StrokeSolid:
                    m_strokeStyle = AnnotationStrokeStyle::Solid;
                    BumpOpaqueIfTransparent();
                    SyncRgbaFromHsv();
                    RememberNonZeroAlpha();
                    break;
                case TopRowSlotKind::StrokeDashed:
                    m_strokeStyle = AnnotationStrokeStyle::Dashed;
                    BumpOpaqueIfTransparent();
                    SyncRgbaFromHsv();
                    RememberNonZeroAlpha();
                    break;
                case TopRowSlotKind::StrokeDotted:
                    m_strokeStyle = AnnotationStrokeStyle::Dotted;
                    BumpOpaqueIfTransparent();
                    SyncRgbaFromHsv();
                    RememberNonZeroAlpha();
                    break;
                case TopRowSlotKind::ColorPreset:
                    m_color = slot.color;
                    m_color.a = ClampAlphaForMode(m_color.a);
                    SyncHsvFromRgba();
                    RememberNonZeroAlpha();
                    break;
                }
                outChanged = true;
                return true;
            }
        }

        if (PtInRect(&m_eyedropperRect, point))
        {
            outRequestEyedropper = true;
            return true;
        }
        if (m_svPad.HitTest(point))
        {
            m_drag = ColorPickerDrag::SaturationValue;
            UpdateDragValue(point);
            outChanged = true;
            return true;
        }
        if (m_hueSlider.HitTest(point))
        {
            m_drag = ColorPickerDrag::Hue;
            UpdateDragValue(point);
            outChanged = true;
            return true;
        }
        if (m_alphaSlider.HitTest(point))
        {
            m_drag = ColorPickerDrag::Alpha;
            UpdateDragValue(point);
            outChanged = true;
            return true;
        }

        return true;
    }

    bool ColorPickerPopover::OnMouseMove(
        POINT point, IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
        int dpi, bool& outChanged) noexcept
    {
        (void)dpi;
        outChanged = false;
        if (!visible())
        {
            return false;
        }
        UpdateHover(point);
        if (m_drag == ColorPickerDrag::TextSelection)
        {
            if (m_activeField != ColorPickerTextField::None)
            {
                m_input.OnMouseMove(point, m_fields[SlotIndexFor(m_activeField)], dwrite, textFormat);
            }
            return true;
        }
        if (m_drag != ColorPickerDrag::None)
        {
            UpdateDragValue(point);
            outChanged = true;
            return true;
        }
        return false;
    }

    bool ColorPickerPopover::OnLeftButtonUp(POINT point, bool& outCommit) noexcept
    {
        outCommit = false;
        if (!visible() || m_drag == ColorPickerDrag::None)
        {
            return false;
        }
        if (m_drag == ColorPickerDrag::TextSelection)
        {
            m_drag = ColorPickerDrag::None;
            m_input.EndDrag();
            return true;
        }
        UpdateDragValue(point);
        RememberNonZeroAlpha();
        m_drag = ColorPickerDrag::None;
        outCommit = true;
        return true;
    }

    bool ColorPickerPopover::OnRightButtonUp(
        HWND window, POINT point, IDWriteFactory* dwrite,
        IDWriteTextFormat* textFormat, int dpi,
        bool& outChanged, bool& outCommitUndo)
    {
        (void)dpi;
        outChanged = false;
        outCommitUndo = false;
        if (!HitTestPanel(point))
        {
            return false;
        }

        m_drag = ColorPickerDrag::None;
        m_input.EndDrag();

        if (m_formatMenu.isOpen())
        {
            if (m_formatMenu.HitTest(point))
            {
                return true;
            }
            m_formatMenu.Close();
        }

        const ColorPickerTextField targetField = HitTestEditableField(point);
        if (targetField == ColorPickerTextField::None)
        {
            return true;
        }

        if (m_activeField != targetField)
        {
            CommitTextEdit(outChanged);
            outCommitUndo = outChanged;
            FocusTextField(targetField);
        }
        else if (!m_input.HasSelection())
        {
            const std::size_t pos = ui::SingleLineInputState::HitTestCaret(
                point, m_fields[SlotIndexFor(targetField)], m_input.text(), dwrite, textFormat);
            m_input.CollapseTo(pos, false);
        }

        const TextContextCommand cmd = ShowTextContextMenu(
            window, point, /*editable=*/true, m_input.HasSelection(), !m_input.text().empty());

        switch (cmd)
        {
        case TextContextCommand::Cut:
            if (m_input.HasSelection())
            {
                CopyTextToClipboard(window, FormatCopyText(true));
                ReplaceFieldSelection({}, false);
                if (ApplyLiveTextParse())
                {
                    outChanged = true;
                    outCommitUndo = true;
                    m_fieldStartColor = m_color;
                }
            }
            break;
        case TextContextCommand::Copy:
            CopyTextToClipboard(window, FormatCopyText(false));
            break;
        case TextContextCommand::Paste:
            if (ApplyPastedClipboard(window, true))
            {
                outChanged = true;
                outCommitUndo = true;
                m_fieldStartColor = m_color;
            }
            break;
        case TextContextCommand::Delete:
            if (m_input.HasSelection())
            {
                ReplaceFieldSelection({}, false);
                if (ApplyLiveTextParse())
                {
                    outChanged = true;
                    outCommitUndo = true;
                    m_fieldStartColor = m_color;
                }
            }
            break;
        case TextContextCommand::SelectAll:
            m_input.SelectAll();
            break;
        case TextContextCommand::None:
        default:
            break;
        }

        return true;
    }

    bool ColorPickerPopover::OnWheel(POINT point, int delta, bool& outChanged) noexcept
    {
        outChanged = false;
        if (!HitTestPanel(point) || delta == 0)
        {
            return false;
        }
        if (m_formatMenu.HitTest(point))
        {
            return true;
        }

        const float steps = static_cast<float>(delta) / 120.0f;
        if (m_alphaSlider.HitTest(point) || m_fields[3].HitTest(point))
        {
            m_alpha = ClampAlphaForMode(m_alpha + steps * 0.02f);
            SyncRgbaFromHsv();
            RememberNonZeroAlpha();
            if (m_activeField == ColorPickerTextField::Alpha)
            {
                m_input.SetText(FormatAlphaPercentNumber(m_alpha), false);
                m_fieldStartColor = m_color;
            }
            outChanged = true;
            return true;
        }
        if (m_hueSlider.HitTest(point))
        {
            BumpOpaqueIfTransparent();
            m_hue = std::fmod(std::fmod(m_hue + steps * 4.0f, 360.0f) + 360.0f, 360.0f);
            SyncRgbaFromHsv();
            RememberNonZeroAlpha();
            if (m_activeField != ColorPickerTextField::None && m_activeField != ColorPickerTextField::Alpha)
            {
                m_input.SetText(FormatFieldString(m_activeField), false);
                m_fieldStartColor = m_color;
            }
            outChanged = true;
            return true;
        }

        const ColorPickerTextField wheeledField = HitTestEditableField(point);
        if (wheeledField == ColorPickerTextField::Channel0
            || wheeledField == ColorPickerTextField::Channel1
            || wheeledField == ColorPickerTextField::Channel2)
        {
            const int step = m_channelWheelAccum.Feed(
                delta, static_cast<std::uint64_t>(wheeledField));
            if (step == 0)
            {
                return true;
            }
            if (StepNumericField(wheeledField, step))
            {
                if (m_activeField != ColorPickerTextField::None && m_activeField != ColorPickerTextField::Alpha)
                {
                    m_input.SetText(FormatFieldString(m_activeField), false);
                    m_fieldStartColor = m_color;
                }
                outChanged = true;
                return true;
            }
        }
        return true;
    }

    bool ColorPickerPopover::OnKeyDown(
        HWND window, WPARAM key, bool& outChanged, bool& outCommitUndo, KeyModifiers mods)
    {
        outChanged = false;
        outCommitUndo = false;

        int chosenFmt = -1;
        if (m_formatMenu.OnKeyDown(key, chosenFmt))
        {
            if (chosenFmt >= 0)
            {
                SetFormatMode(static_cast<ColorFormatMode>(chosenFmt), m_env.dpi);
            }
            return true;
        }

        if (!hasTextFocus())
        {
            return false;
        }

        if (key == VK_ESCAPE)
        {
            CancelTextEdit(outChanged);
            return true;
        }
        if (key == VK_RETURN)
        {
            CommitTextEdit(outChanged);
            outCommitUndo = outChanged;
            return true;
        }
        if (key == VK_TAB)
        {
            ColorPickerTextField order[4]{};
            std::size_t count = 0;
            if (m_formatMode == ColorFormatMode::Hex)
            {
                order[0] = ColorPickerTextField::Hex;
                order[1] = ColorPickerTextField::Alpha;
                count = 2;
            }
            else
            {
                order[0] = ColorPickerTextField::Channel0;
                order[1] = ColorPickerTextField::Channel1;
                order[2] = ColorPickerTextField::Channel2;
                order[3] = ColorPickerTextField::Alpha;
                count = 4;
            }
            std::size_t idx = 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                if (order[i] == m_activeField)
                {
                    idx = i;
                    break;
                }
            }
            const std::size_t nextIdx = mods.shift ? ((idx + count - 1) % count) : ((idx + 1) % count);
            CommitTextEdit(outChanged);
            outCommitUndo = outChanged;
            FocusTextField(order[nextIdx]);
            return true;
        }

        if (m_activeField != ColorPickerTextField::Hex && (key == VK_UP || key == VK_DOWN))
        {
            const int step = (mods.shift ? 10 : 1) * (key == VK_UP ? 1 : -1);
            if (StepNumericField(m_activeField, step))
            {
                m_input.SetText(FormatFieldString(m_activeField), false);
                m_fieldStartColor = m_color;
                outChanged = true;
                outCommitUndo = true;
            }
            return true;
        }

        if (mods.ctrl && (key == 'C' || key == 'X'))
        {
            if (m_input.HasSelection())
            {
                CopyTextToClipboard(window, FormatCopyText(true));
                if (key == 'X')
                {
                    ReplaceFieldSelection({}, false);
                    outChanged = ApplyLiveTextParse();
                }
            }
            return true;
        }
        if (mods.ctrl && key == 'V')
        {
            outChanged = ApplyPastedClipboard(window, true);
            return true;
        }

        bool mutated = false;
        if (m_input.HandleEditingKey(key, mods, mutated))
        {
            if (mutated)
            {
                outChanged = ApplyLiveTextParse();
            }
            return true;
        }

        return true;
    }

    bool ColorPickerPopover::OnChar(wchar_t ch, bool& outChanged) noexcept
    {
        outChanged = false;
        if (!hasTextFocus())
        {
            return false;
        }
        if (ch < 32 || ch == 127)
        {
            return true;
        }

        if (m_activeField == ColorPickerTextField::Hex)
        {
            if (ch == L'#')
            {
                return true;
            }
            if (HexDigitValue(ch) >= 0)
            {
                const wchar_t upper = ToUpperHexChar(ch);
                ReplaceFieldSelection(std::wstring_view(&upper, 1), true);
                outChanged = ApplyLiveTextParse();
            }
            return true;
        }

        if (ch == L'%' || ch == L'\x00B0')
        {
            return true;
        }
        if (ch >= L'0' && ch <= L'9')
        {
            ReplaceFieldSelection(std::wstring_view(&ch, 1), true);
            outChanged = ApplyLiveTextParse();
        }
        return true;
    }

    void ColorPickerPopover::Draw(
        ID2D1DeviceContext* context, IDWriteFactory* dwrite,
        IDWriteTextFormat* textFormat, const HudBrushes& brushes,
        int dpi, ID2D1SvgDocument* eyedropperSvg) const
    {
        if (!visible() || context == nullptr)
        {
            return;
        }

        EnsureStaticGradients(context);

        winrt::com_ptr<ID2D1Factory> d2dFactory;
        context->GetFactory(d2dFactory.put());

        const float radius = ScaleByDpi(8.0f, dpi);
        context->FillRoundedRectangle(ToRoundedRect(m_placement.panel, radius), brushes.panel.get());
        context->DrawRoundedRectangle(ToRoundedRect(m_placement.panel, radius), brushes.panelEdge.get(), 1.0f);

        if (d2dFactory && m_placement.drawCaret)
        {
            const float cx = static_cast<float>(m_placement.anchorCenterX);
            const float halfW = ScaleByDpi(6.0f, dpi);
            const float tipH = ScaleByDpi(6.0f, dpi);
            const float baseY = m_placement.placedAbove
                ? (static_cast<float>(m_placement.panel.bottom) - 1.0f)
                : (static_cast<float>(m_placement.panel.top) + 1.0f);
            const float tipY = m_placement.placedAbove
                ? (static_cast<float>(m_placement.panel.bottom) + tipH)
                : (static_cast<float>(m_placement.panel.top) - tipH);

            winrt::com_ptr<ID2D1PathGeometry> tri;
            AGSHOT_CHECK_HR(d2dFactory->CreatePathGeometry(tri.put()));
            winrt::com_ptr<ID2D1GeometrySink> sink;
            AGSHOT_CHECK_HR(tri->Open(sink.put()));
            sink->BeginFigure(D2D1::Point2F(cx - halfW, baseY), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(cx, tipY));
            sink->AddLine(D2D1::Point2F(cx + halfW, baseY));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            AGSHOT_CHECK_HR(sink->Close());
            context->FillGeometry(tri.get(), brushes.panel.get());
            context->DrawLine(D2D1::Point2F(cx - halfW, baseY), D2D1::Point2F(cx, tipY), brushes.panelEdge.get(), 1.0f);
            context->DrawLine(D2D1::Point2F(cx, tipY), D2D1::Point2F(cx + halfW, baseY), brushes.panelEdge.get(), 1.0f);
        }

        // Top row: optional None/StrokeStyle toggles + preset color swatches
        const float qRadius = ScaleByDpi(4.0f, dpi);
        const auto slots = TopRowSlotsFor(m_target, m_allowNone, m_showStrokeStyles);
        const bool isTransparent = !HasVisibleColor(m_color);
        for (std::size_t i = 0; i < m_quickSwatches.size(); ++i)
        {
            const D2D1_RECT_F r = ToRectF(m_quickSwatches[i]);
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, qRadius, qRadius);
            const TopRowSlot& slot = slots[i];
            const bool hoveredSwatch = (m_hoveredQuick == static_cast<int>(i));
            bool activeSwatch = false;

            if (slot.kind == TopRowSlotKind::NoneSlot)
            {
                activeSwatch = isTransparent;
                FillTiledCheckerboard(context, brushes, r, ScaleByDpi(3.5f, dpi), qRadius);
            }
            else if (slot.kind == TopRowSlotKind::StrokeSolid
                     || slot.kind == TopRowSlotKind::StrokeDashed
                     || slot.kind == TopRowSlotKind::StrokeDotted)
            {
                const AnnotationStrokeStyle slotStyle =
                    (slot.kind == TopRowSlotKind::StrokeSolid) ? AnnotationStrokeStyle::Solid
                    : (slot.kind == TopRowSlotKind::StrokeDashed) ? AnnotationStrokeStyle::Dashed
                    : AnnotationStrokeStyle::Dotted;
                activeSwatch = !isTransparent && (m_strokeStyle == slotStyle);
                context->FillRoundedRectangle(
                    rr,
                    activeSwatch ? brushes.accent.get() : (hoveredSwatch ? brushes.hover.get() : brushes.tooltip.get()));
                ID2D1SolidColorBrush* lineBrush = activeSwatch ? brushes.handleFill.get() : brushes.icon.get();
                const float cy = (r.top + r.bottom) * 0.5f;
                const float x0 = r.left + ScaleByDpi(3.5f, dpi);
                const float x1 = r.right - ScaleByDpi(3.5f, dpi);
                const float strokeW = ScaleByDpi(1.8f, dpi);
                if (slotStyle == AnnotationStrokeStyle::Solid)
                {
                    context->DrawLine(D2D1::Point2F(x0, cy), D2D1::Point2F(x1, cy), lineBrush, strokeW);
                }
                else if (slotStyle == AnnotationStrokeStyle::Dashed)
                {
                    const float seg = (x1 - x0) / 5.0f;
                    for (int k = 0; k < 3; ++k)
                    {
                        const float sx = x0 + static_cast<float>(k) * 2.0f * seg;
                        context->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F(sx + seg, cy), lineBrush, strokeW);
                    }
                }
                else
                {
                    const float dotR = ScaleByDpi(1.05f, dpi);
                    for (int k = 0; k < 4; ++k)
                    {
                        const float dx = x0 + (static_cast<float>(k) / 3.0f) * (x1 - x0);
                        context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dx, cy), dotR, dotR), lineBrush);
                    }
                }
            }
            else
            {
                activeSwatch = !isTransparent && SameRgbaColor(m_color, slot.color);
                if (slot.color.a < 0.98f)
                {
                    FillTiledCheckerboard(context, brushes, r, ScaleByDpi(3.5f, dpi), qRadius);
                }
                context->FillRoundedRectangle(rr, brushes.WithColor(slot.color));
            }

            if (activeSwatch)
            {
                const float inset1 = ScaleByDpi(1.1f, dpi);
                const D2D1_ROUNDED_RECT innerR = D2D1::RoundedRect(
                    D2D1::RectF(r.left + inset1, r.top + inset1, r.right - inset1, r.bottom - inset1),
                    std::max(1.0f, qRadius - inset1),
                    std::max(1.0f, qRadius - inset1));
                context->DrawRoundedRectangle(innerR, brushes.handleEdge.get(), ScaleByDpi(1.0f, dpi));
            }
            context->DrawRoundedRectangle(
                rr,
                activeSwatch ? brushes.handleFill.get() : (hoveredSwatch ? brushes.icon.get() : brushes.panelEdge.get()),
                activeSwatch ? ScaleByDpi(1.6f, dpi) : 1.0f);
        }

        // 2D Saturation-Value pad
        const D2D1_RECT_F svF = ToRectF(m_svPad.rect());
        const D2D1_ROUNDED_RECT svRR = D2D1::RoundedRect(svF, ScaleByDpi(4.0f, dpi), ScaleByDpi(4.0f, dpi));
        const D2D1_COLOR_F pureHue = HsvToRgb(m_hue, 1.0f, 1.0f, 1.0f);
        {
            ID2D1LinearGradientBrush* hBrush = EnsureSvHorizBrush(context, pureHue);
            hBrush->SetStartPoint(D2D1::Point2F(svF.left, svF.top));
            hBrush->SetEndPoint(D2D1::Point2F(svF.right, svF.top));
            context->FillRoundedRectangle(svRR, hBrush);

            m_gpu.svVertBrush->SetStartPoint(D2D1::Point2F(svF.left, svF.top));
            m_gpu.svVertBrush->SetEndPoint(D2D1::Point2F(svF.left, svF.bottom));
            context->FillRoundedRectangle(svRR, m_gpu.svVertBrush.get());
            m_svPad.DrawThumb(context, brushes, m_saturation, m_value, dpi);
        }

        // Hue rainbow slider
        const D2D1_RECT_F hueF = ToRectF(m_hueSlider.rect());
        const float barRadius = (hueF.bottom - hueF.top) * 0.5f;
        {
            m_gpu.hueBrush->SetStartPoint(D2D1::Point2F(hueF.left, hueF.top));
            m_gpu.hueBrush->SetEndPoint(D2D1::Point2F(hueF.right, hueF.top));
            context->FillRoundedRectangle(D2D1::RoundedRect(hueF, barRadius, barRadius), m_gpu.hueBrush.get());
            m_hueSlider.DrawThumb(context, brushes, m_hue / 360.0f, pureHue, dpi);
        }

        // Alpha slider over tiled checkerboard
        const D2D1_RECT_F alphaF = ToRectF(m_alphaSlider.rect());
        {
            FillTiledCheckerboard(context, brushes, alphaF, ScaleByDpi(4.0f, dpi), barRadius);
            ID2D1LinearGradientBrush* aBrush = EnsureAlphaBrush(context);
            aBrush->SetStartPoint(D2D1::Point2F(alphaF.left, alphaF.top));
            aBrush->SetEndPoint(D2D1::Point2F(alphaF.right, alphaF.top));
            context->FillRoundedRectangle(D2D1::RoundedRect(alphaF, barRadius, barRadius), aBrush);
            m_alphaSlider.DrawThumb(context, brushes, m_alpha, D2D1_COLOR_F{ m_color.r, m_color.g, m_color.b, 1.0f }, dpi);
        }

        // Eyedropper button
        {
            const float btnR = ScaleByDpi(4.0f, dpi);
            context->FillRoundedRectangle(
                ToRoundedRect(m_eyedropperRect, btnR),
                m_eyedropperHovered ? brushes.hover.get() : brushes.tooltip.get());
            context->DrawRoundedRectangle(ToRoundedRect(m_eyedropperRect, btnR), brushes.panelEdge.get(), 1.0f);
            if (eyedropperSvg != nullptr)
            {
                winrt::com_ptr<ID2D1DeviceContext5> ctx5;
                if (SUCCEEDED(context->QueryInterface(__uuidof(ID2D1DeviceContext5), ctx5.put_void())))
                {
                    winrt::com_ptr<ID2D1SvgElement> root;
                    eyedropperSvg->GetRoot(root.put());
                    if (root)
                    {
                        root->SetAttributeValue(L"fill", brushes.icon->GetColor());
                    }
                    const float targetSpan = ScaleByDpi(15.0f, dpi);
                    const float s = targetSpan / 16.0f;
                    const float left = static_cast<float>(m_eyedropperRect.left)
                        + (static_cast<float>(m_eyedropperRect.right - m_eyedropperRect.left) - targetSpan) * 0.5f;
                    const float top = static_cast<float>(m_eyedropperRect.top)
                        + (static_cast<float>(m_eyedropperRect.bottom - m_eyedropperRect.top) - targetSpan) * 0.5f;
                    D2D1_MATRIX_3X2_F prev{};
                    context->GetTransform(&prev);
                    context->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(left, top) * prev);
                    ctx5->DrawSvgDocument(eyedropperSvg);
                    context->SetTransform(prev);
                }
            }
        }

        // Bottom format trigger + channel inputs + alpha input + floating dropdown
        if (dwrite != nullptr && textFormat != nullptr)
        {
            const float pillR = ScaleByDpi(4.0f, dpi);

            if (m_formatBtnHovered && !m_formatMenu.isOpen())
            {
                context->FillRoundedRectangle(ToRoundedRect(m_formatBtnRect, pillR), brushes.hover.get());
            }
            ID2D1SolidColorBrush* fmtBrush = m_formatMenu.isOpen()
                ? brushes.disabledIcon.get()
                : (m_formatBtnHovered ? brushes.handleFill.get() : brushes.icon.get());
            const float textLeft = static_cast<float>(m_formatBtnRect.left) + ScaleByDpi(4.0f, dpi);
            const float chevronZoneW = ScaleByDpi(13.0f, dpi);
            const float lblW = std::max(4.0f, static_cast<float>(m_formatBtnRect.right) - chevronZoneW - textLeft);
            const float lblH = static_cast<float>(m_formatBtnRect.bottom - m_formatBtnRect.top);
            if (IDWriteTextLayout* lblLayout = EnsureFormatButtonLayout(dwrite, textFormat, lblW, lblH))
            {
                context->DrawTextLayout(
                    D2D1::Point2F(textLeft, static_cast<float>(m_formatBtnRect.top)),
                    lblLayout, fmtBrush);
            }
            const float cx = static_cast<float>(m_formatBtnRect.right) - ScaleByDpi(7.0f, dpi);
            const float cy = (static_cast<float>(m_formatBtnRect.top) + static_cast<float>(m_formatBtnRect.bottom)) * 0.5f;
            const float cw = ScaleByDpi(3.0f, dpi);
            const float ch = ScaleByDpi(1.6f, dpi);
            const float cStroke = ScaleByDpi(1.35f, dpi);
            context->DrawLine(D2D1::Point2F(cx - cw, cy - ch), D2D1::Point2F(cx, cy + ch), fmtBrush, cStroke);
            context->DrawLine(D2D1::Point2F(cx, cy + ch), D2D1::Point2F(cx + cw, cy - ch), fmtBrush, cStroke);

            if (m_formatMode == ColorFormatMode::Hex)
            {
                const bool focused = (m_activeField == ColorPickerTextField::Hex);
                const std::wstring text = focused ? m_input.text() : FormatFieldString(ColorPickerTextField::Hex);
                m_input.DrawSlot(context, dwrite, textFormat, brushes, dpi, m_fields[0], pillR, focused, text);
            }
            else
            {
                constexpr ColorPickerTextField kChannels[3] = {
                    ColorPickerTextField::Channel0,
                    ColorPickerTextField::Channel1,
                    ColorPickerTextField::Channel2,
                };
                for (std::size_t i = 0; i < 3; ++i)
                {
                    const bool focused = (m_activeField == kChannels[i]);
                    const std::wstring text = focused ? m_input.text() : FormatFieldString(kChannels[i]);
                    m_input.DrawSlot(context, dwrite, textFormat, brushes, dpi, m_fields[i], pillR, focused, text);
                }
            }

            const bool alphaFocused = (m_activeField == ColorPickerTextField::Alpha);
            const std::wstring alphaText = alphaFocused ? m_input.text() : FormatFieldString(ColorPickerTextField::Alpha);
            m_input.DrawSlot(context, dwrite, textFormat, brushes, dpi, m_fields[3], pillR, alphaFocused, alphaText);

            m_formatMenu.Draw(context, dwrite, textFormat, brushes, dpi);
        }
    }
}
