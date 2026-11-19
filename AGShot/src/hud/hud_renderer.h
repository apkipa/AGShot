#pragma once

#include "core/screen_capture.h"
#include "hud/hud_theme.h"
#include "hud/render_device.h"
#include "hud/selection.h"
#include "hud/toolbar.h"
#include "hud_icon_ids.h"

#include <windows.h>

#include <array>
#include <d2d1_1.h>
#include <d2d1_3.h>
#include <dwrite.h>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/base.h>

namespace agshot
{
    // Frame-rate and render-duration history for the optional debug overlay.
    class DebugStatsTracker
    {
    public:
        void SetEnabled(bool enabled) noexcept;
        bool enabled() const noexcept { return m_enabled; }

        void RecordFrameDuration(LONGLONG renderStartQpc) noexcept;
        void RecordPresent() noexcept;
        void Draw(const RenderDevice& device, UINT surfaceWidth);

    private:
        static constexpr int kSamples = 240;
        static constexpr int kWindowMs = 250;
        static constexpr double kGraphSeconds = 10.0;

        bool m_enabled{};
        float m_samples[kSamples]{};
        LONGLONG m_sampleTime[kSamples]{};
        int m_sampleCount{};
        int m_sampleHead{};
        float m_frameMs{};
        float m_renderMs{};
        LONGLONG m_lastFrame{};
        LONGLONG m_lastSample{};
    };

    // Pure HUD element drawing (dimming, selection handles & size pill, pixel
    // magnifier, toolbar & icons, and tooltips). Owns only its DPI-cached
    // text layouts and merged copy-icon geometry.
    class HudRenderer
    {
    public:
        void InvalidateLayouts() noexcept;
        void DiscardIndependentResources() noexcept;

        void DrawDim(ID2D1DeviceContext* context, UINT width, UINT height,
                     bool hasSelection, const RECT& selection,
                     const HudBrushes& brushes);
        void DrawSelection(const RenderDevice& device, UINT width, UINT height,
                           const RECT& selection, bool smart, int handleSize,
                           bool showHandles = true);
        void DrawPixelMagnifier(const RenderDevice& device, UINT width, UINT height,
                                const ScreenImage& image, const RECT& bounds,
                                const RECT& selection, POINT pointer,
                                bool draggingSelection, SelectionPart dragPart,
                                bool translucent);
        void DrawToolbar(const RenderDevice& device, const Toolbar& toolbar,
                         ToolbarButton activeMode);
        void DrawToolbar(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                         IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
                         const HudBrushes& brushes, int dpi,
                         const Toolbar& toolbar, ToolbarButton activeMode);
        void DrawTooltip(const RenderDevice& device, UINT width, UINT height,
                         const Toolbar& toolbar);
        ID2D1SvgDocument* EyedropperSvg(ID2D1DeviceContext* context);

#ifdef AGSHOT_TOOLBAR_PROOF
        bool ExportToolbarProof(RenderDevice& device, class OcrTextPanel& ocrPanel,
                                const wchar_t* path, int dpi,
                                bool textRecognitionActive,
                                float ocrScrollProbe, bool ocrSpinnerProbe);
#endif

    private:
        struct CachedSubTextLayout
        {
            IDWriteFactory* factory{};
            IDWriteTextFormat* format{};
            std::wstring text;
            int widthPx{};
            int heightPx{};
            int dpi{};
            int styleKind{}; // 0 = centered, 1 = leading nowrap, 2 = badge-1 (9.8 dip semi-bold)
            winrt::com_ptr<IDWriteTextLayout> layout;
        };

        void EnsureToolbarSvgs(ID2D1DeviceContext* context);
        IDWriteTextLayout* GetOrCreateSubTextLayout(
            IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
            std::wstring_view text, float widthPx, float heightPx, int dpi, int styleKind);
        void DrawSizeLabel(const RenderDevice& device, UINT width, UINT height,
                           const RECT& selection);
        void DrawToolbarSvg(ID2D1DeviceContext* context, ToolbarIconKind kind,
                            const RECT& box, int dpi, ID2D1SolidColorBrush* brush);
        void DrawSubToolbar(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                            IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
                            const HudBrushes& brushes, int dpi, const Toolbar& toolbar);

        ID2D1DeviceContext* m_svgContext{};
        std::array<winrt::com_ptr<ID2D1SvgDocument>, static_cast<std::size_t>(ToolbarIconKind::Count)> m_iconSvgs;
        std::vector<CachedSubTextLayout> m_subTextLayouts;
        winrt::com_ptr<IDWriteTextLayout> m_sizeLabelLayout;
        RECT m_sizeLabelSelection{};
        bool m_sizeLabelValid{};
        winrt::com_ptr<IDWriteTextLayout> m_tooltipLayout;
        ToolbarButton m_tooltipLayoutButton{ ToolbarButton::None };
        SubToolbarAction m_tooltipLayoutSub{ SubToolbarAction::None };
    };
}
