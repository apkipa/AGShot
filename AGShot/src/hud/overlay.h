#pragma once

#include "core/memory_watch.h"
#include "core/ocr_runtime.h"
#include "core/screen_capture.h"
#include "core/ui_element.h"
#include "hud/annotation_controller.h"
#include "hud/annotations.h"
#include "hud/color_picker.h"
#include "hud/compositor_clock.h"
#include "hud/hud_renderer.h"
#include "hud/hud_theme.h"
#include "hud/ocr_text_panel.h"
#include "hud/render_device.h"
#include "hud/selection.h"
#include "hud/toolbar.h"
#include "hud/toolbar_controller.h"
#include "hud/ui/ui_layer_stack.h"
#include "hud/ui/ui_layout.h"

#include <windows.h>

#include <vector>

namespace agshot
{
    // Full-screen HUD window host & composition root.
    // Owns the DirectComposition window, frozen capture, and value-owned HUD
    // subsystems, routing all pointer/keyboard/cursor/IME interactions through a
    // single Z-ordered ui::LayerStack and flushing host invalidations in one exit.
    class Overlay : private IToolbarHost
    {
    public:
        Overlay();
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
        enum class CanvasDrag
        {
            None,
            NewRectangle,
            MoveOrResize,
            Annotation,
        };

        struct EyedropperLayer final : ui::IHudLayer
        {
            Overlay* owner{};
            bool IsVisible() const noexcept override;
            bool HitTest(POINT point) const override;
            void OnHoverMove(POINT point, ui::Invalidate& inv) override;
            ui::EventOutcome OnLeftDown(const ui::PointerInput& in, ui::Invalidate& inv) override;
            ui::EventOutcome OnRightUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
            ui::EventOutcome OnKeyDown(WPARAM key, KeyModifiers mods, ui::Invalidate& inv) override;
            std::optional<LPCWSTR> CursorForPoint(POINT point, bool captured) const override;
            ui::ImePolicy QueryImePolicy() const noexcept override { return ui::ImePolicy::Disable; }
        };

        struct ColorPickerLayer final : ui::IHudLayer
        {
            Overlay* owner{};
            bool IsVisible() const noexcept override;
            bool HitTest(POINT point) const override;
            void OnHoverMove(POINT point, ui::Invalidate& inv) override;
            void OnHoverLeave(ui::Invalidate& inv) override;
            ui::OutsideClickPolicy OnOutsidePointer(
                ui::PointerButton button, POINT point, const ui::IHudLayer* hitBelow,
                ui::Invalidate& inv) override;
            ui::EventOutcome OnLeftDown(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCapturedMove(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCapturedUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCancelCapture(ui::Invalidate& inv) override;
            ui::EventOutcome OnRightUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
            ui::EventOutcome OnWheel(const ui::PointerInput& in, ui::Invalidate& inv) override;
            ui::EventOutcome OnKeyDown(WPARAM key, KeyModifiers mods, ui::Invalidate& inv) override;
            ui::EventOutcome OnChar(wchar_t ch, ui::Invalidate& inv) override;
            std::optional<LPCWSTR> CursorForPoint(POINT point, bool captured) const override;
            ui::ImePolicy QueryImePolicy() const noexcept override;
        };

        struct TextEditorLayer final : ui::IHudLayer
        {
            Overlay* owner{};
            bool IsVisible() const noexcept override;
            bool HitTest(POINT point) const override;
            ui::OutsideClickPolicy OnOutsidePointer(
                ui::PointerButton button, POINT point, const ui::IHudLayer* hitBelow,
                ui::Invalidate& inv) override;
            ui::EventOutcome OnLeftDown(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCapturedMove(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCapturedUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCancelCapture(ui::Invalidate& inv) override;
            ui::EventOutcome OnRightUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
            ui::EventOutcome OnKeyDown(WPARAM key, KeyModifiers mods, ui::Invalidate& inv) override;
            ui::EventOutcome OnChar(wchar_t ch, ui::Invalidate& inv) override;
            std::optional<LPCWSTR> CursorForPoint(POINT point, bool captured) const override;
            ui::ImePolicy QueryImePolicy() const noexcept override { return ui::ImePolicy::Enable; }
        };

        struct OcrLayer final : ui::IHudLayer
        {
            Overlay* owner{};
            bool IsVisible() const noexcept override;
            bool HitTest(POINT point) const override;
            ui::EventOutcome OnLeftDown(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCapturedMove(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCapturedUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCancelCapture(ui::Invalidate& inv) override;
            ui::EventOutcome OnRightUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
            ui::EventOutcome OnWheel(const ui::PointerInput& in, ui::Invalidate& inv) override;
            ui::EventOutcome OnKeyDown(WPARAM key, KeyModifiers mods, ui::Invalidate& inv) override;
            ui::EventOutcome OnChar(wchar_t ch, ui::Invalidate& inv) override;
            std::optional<LPCWSTR> CursorForPoint(POINT point, bool captured) const override;
        };

        struct CanvasLayer final : ui::IHudLayer
        {
            Overlay* owner{};
            bool IsVisible() const noexcept override { return true; }
            bool HitTest(POINT point) const override { (void)point; return true; }
            void OnHoverMove(POINT point, ui::Invalidate& inv) override;
            ui::EventOutcome OnLeftDown(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCapturedMove(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCapturedUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
            void OnCancelCapture(ui::Invalidate& inv) override;
            void OnCapturedModifiersChanged(POINT point, KeyModifiers mods, ui::Invalidate& inv) override;
            ui::EventOutcome OnRightUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
            ui::EventOutcome OnWheel(const ui::PointerInput& in, ui::Invalidate& inv) override;
            ui::EventOutcome OnKeyDown(WPARAM key, KeyModifiers mods, ui::Invalidate& inv) override;
            std::optional<LPCWSTR> CursorForPoint(POINT point, bool captured) const override;
        };

        HRESULT RecoverDeviceResources() noexcept;
        HRESULT RenderFrame();
        void GiveUpRendering(HRESULT hr, const wchar_t* operation) noexcept;

        RECT ClientBounds() const noexcept;
        ui::LayoutEnv CurrentLayoutEnv() const noexcept override;
        int EffectiveHandleSize(ui::PointerKind kind) const noexcept;
        bool PointerEvent(WPARAM wparam, POINTER_INFO& info, POINT& point) const noexcept;
        void FlushInvalidation(ui::Invalidate inv);
        void RunLayout();

        // IToolbarHost overrides
        HWND HostWindow() const noexcept override { return m_window; }
        bool IsToolbarSuppressed() const noexcept override { return m_eyedropperActive; }
        ToolbarState CurrentToolbarState() const noexcept override;
        void OnToolbarButtonInvoked(ToolbarButton button, ui::Invalidate& inv) override { Invoke(button, inv); }
        void OnSubToolbarActionInvoked(SubToolbarAction action, ui::Invalidate& inv) override { InvokeSubAction(action, inv); }
        void DismissColorPicker(bool commitEdit, ui::Invalidate& inv) override;
        void OnToolbarMoved() override;

        void RefreshSuggestion(POINT point, int expansionBudget = 2);
        void ApplyLadderLevel();
        void RequestRender();
        void StartSelectionAnimation(const RECT& target);
        bool AdvanceSelectionAnimation(LONGLONG now) noexcept;
        void StopSelectionAnimation() noexcept;
        void SyncImeState() noexcept;
        void Settle();
        void Invoke(ToolbarButton button, ui::Invalidate& inv);
        void InvokeSubAction(SubToolbarAction action, ui::Invalidate& inv);
        void ToggleColorPicker(ColorPickerTarget target, SubToolbarAction anchorAction, ui::Invalidate& inv);
        void ApplyColorFromPicker(bool commitUndo);
        void EnterTextRecognitionMode(ui::Invalidate& inv);
        void CopyOcrText(bool closeAfterCopy);

        void ApplyCursor();

        void TrimCpuCaches() noexcept;
        void TrimGpuCaches() noexcept;
        void ReconcileMemoryState() noexcept;
        void OnMemoryPressure() noexcept;
        void OnVramPressure() noexcept;

        HWND m_window{};
        RECT m_bounds{};
        UINT m_width{};
        UINT m_height{};
        int m_handleSize{ 8 };
        bool m_failed{};

        RenderDevice m_device;
        SystemMemoryWatch m_memoryWatch;
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
        ToolbarButton m_toolMode{ ToolbarButton::EditSelection };
        OcrRuntime* m_ocrRuntime{};

        AnnotationController m_annotController;
        ColorPickerPopover m_colorPicker;
        SubToolbarAction m_pickerAnchorAction{ SubToolbarAction::None };
        bool m_eyedropperActive{};

        int m_depth{};
        ULONGLONG m_lastQuery{};
        POINT m_pointer{};
        ui::WheelDetentAccumulator m_canvasWheelAccum{};

        ToolbarController m_toolbarController;

        CanvasDrag m_canvasDrag{ CanvasDrag::None };
        SelectionPart m_dragPart{ SelectionPart::None };
        POINT m_dragOrigin{};
        RECT m_dragStart{};
        ULONGLONG m_lastClickMs{};
        POINT m_lastClickPt{};
        bool m_pendingConfirm{};
        bool m_reselectPending{};

        EyedropperLayer m_eyedropperLayer{};
        ColorPickerLayer m_colorPickerLayer{};
        TextEditorLayer m_textEditorLayer{};
        OcrLayer m_ocrLayer{};
        CanvasLayer m_canvasLayer{};
        ui::LayerStack m_layerStack;

        HWND m_previousForeground{};

        ScreenImage m_image;
        ElementPicker m_picker;
        std::vector<RECT> m_ladder;
    };
}
