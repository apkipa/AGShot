#pragma once

#include "core/ocr_runtime.h"
#include "core/screen_capture.h"
#include "core/ui_element.h"
#include "hud/compositor_clock.h"
#include "hud/hud_renderer.h"
#include "hud/hud_theme.h"
#include "hud/ocr_text_panel.h"
#include "hud/render_device.h"
#include "hud/selection.h"
#include "hud/toolbar.h"

#include <windows.h>

#include <vector>

namespace agshot
{
    // The full-screen HUD.
    //
    // Coordinates the borderless DirectComposition window, frozen capture,
    // UIA element picking, selection interaction, and delegates rendering,
    // graphics device recovery, compositor clock, and OCR UI to focused
    // value-owned subcomponents.
    class Overlay
    {
    public:
        Overlay() = default;
        ~Overlay();

        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;

        bool Create(HINSTANCE instance);
        void Destroy() noexcept;

        HWND window() const noexcept { return m_window; }
        void SetOcrRuntime(OcrRuntime* runtime) noexcept { m_ocrRuntime = runtime; }
        void SetDebugStats(bool show);

        void Begin();
        void Cancel() noexcept;
        void Render();

        LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

#ifdef AGSHOT_TOOLBAR_PROOF
        bool ExportToolbarProof(const wchar_t* path, int dpi, bool textRecognitionActive,
                                float ocrScrollProbe = 0.0f, bool ocrSpinnerProbe = false);
#endif

    private:
        enum class DragMode
        {
            None,
            NewRectangle,
            MoveOrResize,
            ToolbarMove,
        };

        enum class OperationMode
        {
            EditSelection,
            TextRecognition,
        };

        enum class HitTarget
        {
            Canvas,
            Selection,
            SelectionHandle,
            OcrText,
            OcrScrollbar,
            ToolbarPanel,
            ToolbarHandle,
            ToolbarButton,
        };

        HRESULT RecoverDeviceResources() noexcept;
        HRESULT RenderFrame();
        void GiveUpRendering(HRESULT hr, const wchar_t* operation) noexcept;

        RECT ClientBounds() const noexcept;
        HitTarget HitTest(POINT point) const noexcept;
        void ShowOcrContextMenu(POINT point);
        bool PointerEvent(WPARAM wparam, POINTER_INFO& info, POINT& point) const noexcept;

        void OnMouseMove(POINT point);
        void OnLeftButtonDown(POINT point);
        void OnLeftButtonUp(POINT point);
        void OnWheel(POINT point, int delta);

        void RefreshSuggestion(POINT point, int expansionBudget = 2);
        void ApplyLadderLevel();
        void RequestRender();
        void StartSelectionAnimation(const RECT& target);
        bool AdvanceSelectionAnimation(LONGLONG now) noexcept;
        void StopAnimationClock() noexcept;
        void StopSelectionAnimation() noexcept;
        void UpdateHover(POINT point);
        void Settle();
        void Invoke(ToolbarButton button);
        void EnterTextRecognitionMode();
        void CopyOcrText(bool closeAfterCopy);

        LPCWSTR CursorShapeForPoint(POINT point) const noexcept;
        void ApplyCursor();

        void TrimCpuCaches() noexcept;
        void TrimGpuCaches() noexcept;
        void OnMemoryPressure() noexcept;
        void OnVramPressure() noexcept;

        HWND m_window{};
        RECT m_bounds{};
        UINT m_width{};
        UINT m_height{};
        int m_handleSize{ 8 };
        bool m_failed{};

        RenderDevice m_device;
        HudRenderer m_renderer;
        DebugStatsTracker m_stats;
        CompositorClockClient m_clock;
        OcrTextPanel m_ocrPanel;

        RECT m_selection{};
        RECT m_displaySelection{};
        RECT m_animationFrom{};
        RECT m_animationTo{};
        LONGLONG m_animationStart{};
        bool m_hasSelection{};
        bool m_animatingSelection{};
        bool m_beginning{};

        bool m_smart{ true };
        OperationMode m_mode{ OperationMode::EditSelection };
        OcrRuntime* m_ocrRuntime{};

        int m_depth{};
        ULONGLONG m_lastQuery{};
        POINT m_pointer{};

        Toolbar m_toolbar{};
        Toolbar m_toolbarDragStart{};
        POINT m_toolbarDragOrigin{};
        bool m_toolbarVisible{};

        DragMode m_dragMode{ DragMode::None };
        SelectionPart m_dragPart{ SelectionPart::None };
        POINT m_dragOrigin{};
        RECT m_dragStart{};
        ToolbarButton m_pressedButton{ ToolbarButton::None };
        bool m_pendingConfirm{};

        HWND m_previousForeground{};

        ScreenImage m_image;
        ElementPicker m_picker;
        std::vector<RECT> m_ladder;
    };
}
