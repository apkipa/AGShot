#pragma once

#include "hud/annotations.h"
#include "hud/hud_theme.h"
#include "hud/text_edit_utils.h"
#include "hud/ui/ui_dropdown.h"
#include "hud/ui/ui_layout.h"
#include "hud/ui/ui_slider.h"
#include "hud/ui/ui_text_field.h"
#include "hud/ui/ui_types.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <d2d1_1.h>
#include <d2d1_3.h>
#include <dwrite.h>
#include <string>

#include <winrt/base.h>

namespace agshot
{
    enum class ColorPickerTarget
    {
        None,
        Stroke,
        Fill,
    };

    enum class ColorFormatMode
    {
        Hex,
        Hsb,
        Rgb,
    };

    enum class ColorPickerDrag
    {
        None,
        SaturationValue,
        Hue,
        Alpha,
        TextSelection,
    };

    enum class ColorPickerTextField
    {
        None,
        Hex,
        Channel0,
        Channel1,
        Channel2,
        Alpha,
    };

    // Floating Direct2D HSV + Alpha color picker popover composed from ui::Pad2D,
    // ui::Slider1D, ui::DropdownMenu, and ui::TextFieldSlot / SingleLineInputState.
    class ColorPickerPopover
    {
    public:
        ColorPickerPopover();

        bool visible() const noexcept { return m_visible && !m_suspended; }
        bool isOpen() const noexcept { return m_visible; }
        bool suspended() const noexcept { return m_visible && m_suspended; }
        bool hasTextFocus() const noexcept { return visible() && m_activeField != ColorPickerTextField::None; }
        ColorPickerTextField activeField() const noexcept { return m_activeField; }
        bool HasEditSelection() const noexcept { return m_input.HasSelection(); }
        std::size_t caret() const noexcept { return m_input.caret(); }
        std::size_t anchor() const noexcept { return m_input.anchor(); }
        ColorPickerTarget target() const noexcept { return m_target; }
        ColorFormatMode formatMode() const noexcept { return m_formatMode; }
        bool isFormatMenuOpen() const noexcept { return visible() && m_formatMenu.isOpen(); }
        bool showsStrokeStyles() const noexcept { return m_showStrokeStyles; }
        AnnotationStrokeStyle strokeStyle() const noexcept { return m_strokeStyle; }
        D2D1_COLOR_F color() const noexcept { return m_color; }
        float restoreAlpha() const noexcept { return m_restoreAlpha; }
        const RECT& panel() const noexcept { return m_placement.panel; }
        const RECT& formatButtonRect() const noexcept { return m_formatBtnRect; }
        const RECT& formatMenuRect() const noexcept { return m_formatMenu.rect(); }
        RECT formatMenuItemRect(int index) const noexcept { return m_formatMenu.itemRect(index); }
        const RECT& channelRect(int index) const noexcept { return m_fields[static_cast<std::size_t>(index)].box; }
        const RECT& hexRect() const noexcept { return m_fields[0].box; }
        const RECT& alphaTextRect() const noexcept { return m_fields[3].box; }
        bool dragging() const noexcept { return m_drag != ColorPickerDrag::None; }

        void DiscardDeviceResources() noexcept;
        void InvalidateLayouts() noexcept;

        SIZE Measure(int dpi) const noexcept;
        void Arrange(const ui::AnchorSpec& spec, const ui::LayoutEnv& env) noexcept;

        void Open(ColorPickerTarget target, bool allowNone, bool showStrokeStyles,
                  AnnotationStrokeStyle strokeStyle, D2D1_COLOR_F initialColor,
                  const RECT& anchor, const RECT& bounds, int dpi,
                  const RECT& avoidBand = RECT{}, float restoreAlpha = 1.0f) noexcept;
        void Open(ColorPickerTarget target, bool allowNone,
                  D2D1_COLOR_F initialColor, const RECT& anchor,
                  const RECT& bounds, int dpi,
                  const RECT& avoidBand = RECT{}, float restoreAlpha = 1.0f) noexcept;
        void Close() noexcept;
        void Reposition(const RECT& anchor, const RECT& bounds, int dpi,
                        const RECT& avoidBand = RECT{}) noexcept;
        void SuspendForEyedropper() noexcept;
        void ResumeFromEyedropper() noexcept;
        void EndDrag() noexcept { m_drag = ColorPickerDrag::None; m_input.EndDrag(); }
        void SetPickedPixelColor(D2D1_COLOR_F rgb, int dpi) noexcept;
        void SetFormatMode(ColorFormatMode mode, int dpi) noexcept;
        bool CloseFormatMenu() noexcept { return m_formatMenu.Close(); }

        void CommitTextEdit(bool& outChanged) noexcept;
        void CancelTextEdit(bool& outChanged) noexcept;

        bool HitTestPanel(POINT point) const noexcept;
        bool UpdateHover(POINT point) noexcept;
        void ClearHover() noexcept;
        LPCWSTR CursorForPoint(POINT point) const noexcept;

        bool OnLeftButtonDown(POINT point, IDWriteFactory* dwrite,
                              IDWriteTextFormat* textFormat, int dpi,
                              bool& outRequestEyedropper, bool& outChanged,
                              KeyModifiers mods = QueryCurrentKeyModifiers());
        bool OnMouseMove(POINT point, IDWriteFactory* dwrite,
                         IDWriteTextFormat* textFormat, int dpi, bool& outChanged) noexcept;
        bool OnLeftButtonUp(POINT point, bool& outCommit) noexcept;
        bool OnRightButtonUp(HWND window, POINT point, IDWriteFactory* dwrite,
                             IDWriteTextFormat* textFormat, int dpi,
                             bool& outChanged, bool& outCommitUndo);
        bool OnWheel(POINT point, int delta, bool& outChanged) noexcept;
        bool OnKeyDown(HWND window, WPARAM key, bool& outChanged, bool& outCommitUndo,
                       KeyModifiers mods = QueryCurrentKeyModifiers());
        bool OnChar(wchar_t ch, bool& outChanged) noexcept;

        void Draw(ID2D1DeviceContext* context, IDWriteFactory* dwrite,
                  IDWriteTextFormat* textFormat, const HudBrushes& brushes,
                  int dpi, ID2D1SvgDocument* eyedropperSvg) const;

    private:
        struct GpuCache
        {
            ID2D1DeviceContext* context{};
            winrt::com_ptr<ID2D1LinearGradientBrush> svVertBrush;
            winrt::com_ptr<ID2D1LinearGradientBrush> hueBrush;
            winrt::com_ptr<ID2D1LinearGradientBrush> svHorizBrush;
            float svHorizHue{ -1.0f };
            winrt::com_ptr<ID2D1LinearGradientBrush> alphaBrush;
            D2D1_COLOR_F alphaRgb{ -1.0f, -1.0f, -1.0f, -1.0f };

            IDWriteFactory* fmtFactory{};
            IDWriteTextFormat* fmtFormat{};
            ColorFormatMode fmtMode{ ColorFormatMode::Hex };
            float fmtWidth{ -1.0f };
            float fmtHeight{ -1.0f };
            winrt::com_ptr<IDWriteTextLayout> fmtLayout;

            void ResetDevice() noexcept
            {
                context = nullptr;
                svVertBrush = nullptr;
                hueBrush = nullptr;
                svHorizBrush = nullptr;
                svHorizHue = -1.0f;
                alphaBrush = nullptr;
                alphaRgb = D2D1_COLOR_F{ -1.0f, -1.0f, -1.0f, -1.0f };
            }

            void ResetLayouts() noexcept
            {
                fmtFactory = nullptr;
                fmtFormat = nullptr;
                fmtWidth = -1.0f;
                fmtHeight = -1.0f;
                fmtLayout = nullptr;
            }
        };

        void EnsureStaticGradients(ID2D1DeviceContext* context) const;
        ID2D1LinearGradientBrush* EnsureSvHorizBrush(ID2D1DeviceContext* context, const D2D1_COLOR_F& pureHue) const;
        ID2D1LinearGradientBrush* EnsureAlphaBrush(ID2D1DeviceContext* context) const;
        IDWriteTextLayout* EnsureFormatButtonLayout(IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
                                                    float w, float h) const;

        float ClampAlphaForMode(float a) const noexcept;
        float EffectiveFallbackAlpha() const noexcept;
        void BumpOpaqueIfTransparent() noexcept;
        void RememberNonZeroAlpha() noexcept;
        void ArrangeReadoutRow(const ui::LayoutEnv& env) noexcept;
        void SyncRgbaFromHsv() noexcept;
        void SyncHsvFromRgba() noexcept;
        void UpdateDragValue(POINT point) noexcept;
        void FocusTextField(ColorPickerTextField field) noexcept;
        void SyncEditBufferFromColor() noexcept;
        bool ApplyLiveTextParse() noexcept;
        std::wstring FormatFieldString(ColorPickerTextField field) const;
        std::wstring FormatCopyText(bool selectedOnly) const;
        std::size_t SlotIndexFor(ColorPickerTextField field) const noexcept;
        ColorPickerTextField HitTestEditableField(POINT point) const noexcept;
        bool StepNumericField(ColorPickerTextField field, int deltaSteps) noexcept;
        void ReplaceFieldSelection(std::wstring_view replacement, bool coalesceSingleChar = false);
        bool ApplyPastedClipboard(HWND window, bool allowHexShorthand);

        bool m_visible{};
        bool m_suspended{};
        ColorPickerTarget m_target{ ColorPickerTarget::None };
        ColorFormatMode m_formatMode{ ColorFormatMode::Hex };
        bool m_allowNone{};
        bool m_showStrokeStyles{};
        AnnotationStrokeStyle m_strokeStyle{ AnnotationStrokeStyle::Solid };

        D2D1_COLOR_F m_color{ 0.94f, 0.25f, 0.25f, 1.0f };
        float m_hue{};
        float m_saturation{ 1.0f };
        float m_value{ 1.0f };
        float m_alpha{ 1.0f };
        float m_restoreAlpha{ 1.0f };

        ColorPickerTextField m_activeField{ ColorPickerTextField::None };
        ui::SingleLineInputState m_input;
        D2D1_COLOR_F m_fieldStartColor{};

        // Composed UI sub-controls
        ui::PopoverPlacement m_placement{};
        ui::LayoutEnv m_env{};
        std::array<RECT, 8> m_quickSwatches{};
        ui::Pad2D m_svPad;
        ui::Slider1D m_hueSlider;
        ui::Slider1D m_alphaSlider;
        RECT m_eyedropperRect{};
        RECT m_formatBtnRect{};
        ui::DropdownMenu m_formatMenu;
        // 0..2: Hex (0) or Channel0/1/2; 3: Alpha%
        std::array<ui::TextFieldSlot, 4> m_fields{};

        bool m_eyedropperHovered{};
        bool m_formatBtnHovered{};
        int m_hoveredQuick{ -1 };
        ColorPickerDrag m_drag{ ColorPickerDrag::None };
        ui::WheelDetentAccumulator m_channelWheelAccum{};
        mutable GpuCache m_gpu;
    };
}
