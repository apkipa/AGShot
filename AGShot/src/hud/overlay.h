#pragma once

#include "core/ocr_runtime.h"
#include "core/screen_capture.h"
#include "core/ui_element.h"
#include "hud/selection.h"
#include "hud/spinner_effect.h"
#include "hud/toolbar.h"

#include <windows.h>

#include <d2d1_1.h>
#include <dcomp.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agshot
{
    // The full-screen HUD.
    //
    // A borderless, topmost window covering every monitor, drawn through
    // DirectComposition over the screen image frozen when a capture starts.
    // Hidden until a capture starts.
    //
    // The selection rectangle and everything derived from it are in client
    // pixels, which for this window means "pixels from the top-left of the
    // virtual screen". Screen coordinates are recovered by adding m_bounds.left
    // and m_bounds.top, and only the two places that need them do that.
    class Overlay
    {
    public:
        Overlay() = default;
        ~Overlay();

        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;

        // Creates the window, hidden, and its device resources. Returns false and
        // leaves the last Win32 error set when the window cannot be created.
        bool Create(HINSTANCE instance);
        void Destroy() noexcept;

        HWND window() const noexcept { return m_window; }
        void SetOcrRuntime(OcrRuntime* runtime) noexcept { m_ocrRuntime = runtime; }

        // Draws the frame rate and its recent history over everything else. A
        // diagnostic, so it is off unless the configuration asks for it.
        void SetDebugStats(bool show);

        // Covers the whole virtual screen and starts a fresh selection, following
        // the pointer until the user does something decisive.
        void Begin();

        // Hides the overlay, forgets the selection, and gives the foreground back
        // to whoever had it.
        void Cancel() noexcept;

        void Render();

        LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

#ifdef AGSHOT_TOOLBAR_PROOF
        bool ExportToolbarProof(const wchar_t* path, int dpi, bool textRecognitionActive,
                                float ocrScrollProbe = 0.0f, bool ocrSpinnerProbe = false);
#endif

    private:
        // How a drag is changing the selection, if one is in progress.
        enum class DragMode
        {
            None,
            NewRectangle,     // pulling a fresh rectangle out of empty space
            MoveOrResize,     // moving or resizing the one already there
            ToolbarMove,       // dragging the toolbar by its left handle
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

        void CreateDeviceIndependentResources();
        void InvalidateTextLayouts() noexcept;
        void CreateCopyIconGeometry(float localStroke);
        void CreateDeviceResources();
        void CreateSizeDependentResources();

        struct AnimationFrameSignal
        {
            std::mutex mutex;
            HWND window{};
            bool active{ true };
            bool posted{};
        };

        RECT ClientBounds() const noexcept;
        HitTarget HitTest(POINT point) const noexcept;
        void ShowOcrContextMenu(POINT point);
        void UpdateOcrSelection(POINT point);
        void StopOcrSelection() noexcept;

        // Resolves a pointer message into the information and the client-space
        // point used by the pointer input path. False when pointer information
        // cannot be read. Mouse, pen and touch use the same interaction path.
        bool PointerEvent(WPARAM wparam, POINTER_INFO& info, POINT& point) const noexcept;

        void OnMouseMove(POINT point);
        void OnLeftButtonDown(POINT point);
        void OnLeftButtonUp(POINT point);
        void OnWheel(POINT point, int delta);

        void RefreshSuggestion(POINT point, int expansionBudget = 2);
        void ApplyLadderLevel();
        void RequestRender();
        void StartSelectionAnimation(const RECT& target);
        bool StartAnimationClock() noexcept;
        bool AdvanceSelectionAnimation(LONGLONG now) noexcept;
        void StopAnimationClock() noexcept;
        void StopSelectionAnimation() noexcept;
        void UpdateHover(POINT point);
        void Settle();
        void Invoke(ToolbarButton button);
        void EnterTextRecognitionMode();
        void CopyOcrText(bool closeAfterCopy);
        RECT OcrTextBox() const noexcept;
        RECT OcrScrollTrack() const noexcept;
        RECT OcrScrollThumb() const;
        // The laid-out OCR text. CreateTextLayout() shapes and line-breaks the
        // whole result, so the layout is cached and only rebuilt when the text or
        // the width it wraps to changes.
        winrt::com_ptr<IDWriteTextLayout> OcrTextLayout() const;
        std::size_t OcrPositionFromPoint(POINT point) const;
        float OcrScrollMaximum() const;
        void EnsureOcrCaretVisible();
        void MoveOcrCaret(WPARAM key);
        void DrawOcrSpinner();
        void CreateSpinnerEffect();
        ppocr::ImageBGR OcrImageForSelection() const;

        // The pointer shape for a point in client coordinates, and the same
        // thing applied to wherever the pointer actually is. Kept apart from the
        // message handler because the shape also changes when the state behind it
        // changes without the pointer having moved.
        LPCWSTR CursorShapeForPoint(POINT point) const noexcept;
        void ApplyCursor();

        // Frame timing for the debug readout. Sampling happens at the end of
        // Render(), so it measures what was actually drawn rather than what was
        // asked for.
        void RecordFrame() noexcept;
        void DrawDebugStats();

        void ClearSurface();
        void DrawDim();
        void DrawSelection();
        void DrawTextRecognition();
        void DrawSizeLabel();
        void DrawToolbar();
        void DrawTooltip();
        void DrawPixelMagnifier();
        void DrawIconCopy(const RECT& box);
        void DrawIconCancel(const RECT& box);
        void DrawIconEditSelection(const RECT& box, ID2D1SolidColorBrush* brush);
        void DrawIconTextRecognition(const RECT& box, ID2D1SolidColorBrush* brush);

        float Scaled(int value) const noexcept;
        float Scaled(float value) const noexcept;
        float MeasuredPixels(float pixels) const noexcept;

        HWND m_window{};
        HINSTANCE m_instance{};
        RECT m_bounds{};          // the virtual screen, in screen coordinates
        UINT m_width{};
        UINT m_height{};
        int m_dpi{96};
        int m_handleSize{8};

        // Set once a failure has been reported, so that a repaint cannot stack up
        // one dialog per frame.
        bool m_failed{};

        winrt::com_ptr<IDXGIDevice> m_dxgiDevice;
        winrt::com_ptr<ID2D1Factory1> m_d2dFactory;
        winrt::com_ptr<ID2D1Device> m_d2dDevice;
        winrt::com_ptr<ID2D1DeviceContext> m_d2dContext;
        winrt::com_ptr<IDWriteFactory> m_dwriteFactory;
        winrt::com_ptr<IDWriteTextFormat> m_textFormat;
        winrt::com_ptr<IDWriteTextFormat> m_magnifierTextFormat;
        // Fixed pitch, so the frame-rate digits do not shuffle sideways as the
        // value changes.
        winrt::com_ptr<IDWriteTextFormat> m_statsTextFormat;
        winrt::com_ptr<ID2D1StrokeStyle> m_roundStroke;
        // The same joins, but with square ends: the text-recognition mark is
        // traced from a reference image whose bars are cut off flat.
        winrt::com_ptr<ID2D1StrokeStyle> m_flatStroke;
        // The copy mark's two squares merged into a single stroked area, plus the
        // stroke it was built for, so it can be rebuilt when the DPI changes that
        // stroke. Merging keeps the crossing solid: stroking the squares apart
        // paints it twice and leaves a pale seam.
        winrt::com_ptr<ID2D1PathGeometry> m_copyIconGeometry;
        float m_copyLocalStroke{};
#ifdef AGSHOT_TOOLBAR_PROOF
        float m_ocrScrollProbe{};
        // Renders the recognition spinner instead of the text, at a fixed phase
        // so a proof image is reproducible.
        bool m_ocrSpinnerProbe{};
#endif
        // The spinner is drawn by a custom effect that evaluates the conic
        // gradient per pixel, so nothing here is tessellated or banded. One
        // instance is enough: the arc's shape never changes, so Direct2D caches
        // its rasterization and the turning comes from the transform it is drawn
        // under.
        winrt::com_ptr<ID2D1Effect> m_spinnerEffect;

        winrt::com_ptr<ID2D1Layer> m_magnifierLayer;
        winrt::com_ptr<IDCompositionDevice> m_composition;
        winrt::com_ptr<IDCompositionTarget> m_target;
        winrt::com_ptr<IDCompositionVisual> m_visual;
        winrt::com_ptr<IDCompositionVirtualSurface> m_surface;

        winrt::com_ptr<ID2D1SolidColorBrush> m_dim;
        winrt::com_ptr<ID2D1SolidColorBrush> m_shadow;
        winrt::com_ptr<ID2D1SolidColorBrush> m_accent;
        winrt::com_ptr<ID2D1SolidColorBrush> m_handleFill;
        winrt::com_ptr<ID2D1SolidColorBrush> m_handleEdge;
        winrt::com_ptr<ID2D1SolidColorBrush> m_panel;
        winrt::com_ptr<ID2D1SolidColorBrush> m_panelEdge;
        winrt::com_ptr<ID2D1SolidColorBrush> m_hover;
        winrt::com_ptr<ID2D1SolidColorBrush> m_icon;
        // The cancel mark is the one destructive control in the row, so it is
        // drawn in red rather than the shared icon colour.
        winrt::com_ptr<ID2D1SolidColorBrush> m_cancel;
        winrt::com_ptr<ID2D1SolidColorBrush> m_tooltip;
        winrt::com_ptr<ID2D1SolidColorBrush> m_textScrollTrack;
        winrt::com_ptr<ID2D1SolidColorBrush> m_textScrollThumb;

        // The rectangle on show: either the user's, or the suggestion under the
        // pointer while "m_smart" is still true.
        RECT m_selection{};
        RECT m_displaySelection{};
        RECT m_animationFrom{};
        RECT m_animationTo{};
        LONGLONG m_animationStart{};
        bool m_hasSelection{};
        bool m_animatingSelection{};
        bool m_beginning{};
        winrt::event_token m_animationClockToken{};
        std::shared_ptr<AnimationFrameSignal> m_animationFrameSignal;

        // True while the selection is still following the pointer, which is what
        // "smart selection" means: nothing has been committed yet.
        bool m_smart{true};
        OperationMode m_mode{ OperationMode::EditSelection };
        std::wstring m_ocrText;
        OcrRuntime* m_ocrRuntime{};
        // The overlay's observation of the current OCR job. Releasing this
        // pointer abandons the result for the UI; it never stops the worker.
        std::shared_ptr<OcrCompletion> m_ocrCompletion;
        // Set while the current UI observes a completion that has not arrived. The box stays
        // empty while it is set and draws the spinner instead, so a slow first
        // recognition cannot be mistaken for an empty one.
        bool m_ocrPending{};
        LONGLONG m_ocrPendingStart{};
        RECT m_ocrCacheSelection{};
        bool m_ocrCacheValid{};
        std::size_t m_ocrCaret{};
        std::size_t m_ocrAnchor{};
        bool m_ocrSelecting{};
        bool m_ocrScrollDragging{};
        POINT m_ocrScrollDragOrigin{};
        float m_ocrScrollDragStart{};
        POINT m_ocrSelectionPoint{};
        float m_ocrScrollY{};
        mutable winrt::com_ptr<IDWriteTextLayout> m_ocrLayout;
        mutable std::wstring m_ocrLayoutText;
        mutable float m_ocrLayoutWidth{ -1.0f };
        // The range the selection's drawing effect is currently set on, so it can
        // be moved or cleared without throwing the layout away.
        mutable std::size_t m_ocrLayoutHighlightFirst{};
        mutable std::size_t m_ocrLayoutHighlightLast{};
        // Reused by the per-frame hit tests, so they stop allocating a vector the
        // size of the text on every frame.
        mutable std::vector<DWRITE_HIT_TEST_METRICS> m_ocrHitMetrics;

        // The size label is tied to the current selection range, so its layout is
        // rebuilt when that range changes. The tooltip shows a fixed string per
        // button, so its layout is keyed on the hovered button.
        winrt::com_ptr<IDWriteTextLayout> m_sizeLabelLayout;
        RECT m_sizeLabelSelection{};
        bool m_sizeLabelValid{};
        winrt::com_ptr<IDWriteTextLayout> m_tooltipLayout;
        ToolbarButton m_tooltipLayoutButton{ ToolbarButton::None };
        int m_depth{};            // wheel-adjusted level within the UIA tree
        ULONGLONG m_lastQuery{};  // throttles the UIA call

        // The last pointer position, in client pixels, so that hover state and
        // cursor shapes can be worked out without waiting for the next move.
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

        // A press in smart mode has not committed to anything yet: released
        // where it started it means "yes, that suggestion", dragged away from
        // there it means "no, I am drawing my own".
        bool m_pendingConfirm{};

        // The debug readout. Samples are taken only when a frame is actually
        // drawn; nothing here asks for one, because a frame rate display that
        // produced its own frames would be measuring itself. Each sample carries
        // the time it was taken so the graph is laid out against the clock: a
        // stretch with no frames shows as a gap rather than being compressed
        // away, which is what makes the reading honest.
        static constexpr int kStatsSamples = 240;
        static constexpr int kStatsWindowMs = 250;
        static constexpr double kStatsGraphSeconds = 10.0;
        bool m_showStats{};
        float m_statsSamples[kStatsSamples]{};
        LONGLONG m_statsSampleTime[kStatsSamples]{};
        int m_statsSampleCount{};
        int m_statsSampleHead{};
        // The interval between frames, smoothed. The rate on the panel is derived
        // from it, so the rate and the interval cannot disagree.
        float m_statsFrameMs{};
        // How long generating a frame takes, smoothed. This is the figure the
        // panel reports in milliseconds: an interval says how often frames
        // happen, which is not the same question as how much work one is, and
        // reporting the interval as a duration made the readout look like a cost
        // when it was only a cadence.
        float m_statsRenderMs{};
        LONGLONG m_statsLastFrame{};
        LONGLONG m_statsLastSample{};

        HWND m_previousForeground{};

        // The screen as it was when the capture began. Everything is drawn over
        // it and the copy is a crop of it, so the desktop underneath is never
        // consulted again and nothing can move mid-selection.
        ScreenImage m_image;
        winrt::com_ptr<ID2D1Bitmap> m_background;

        // The per-capture window/UIA cache and the last hit path through it.
        ElementPicker m_picker;
        std::vector<RECT> m_ladder;
    };
}
