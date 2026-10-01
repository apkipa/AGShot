#pragma once

#include "core/screen_capture.h"
#include "core/ui_element.h"
#include "hud/selection.h"
#include "hud/toolbar.h"

#include <windows.h>

#include <d2d1_1.h>
#include <dcomp.h>
#include <dwrite.h>

#include <winrt/base.h>

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

        // Covers the whole virtual screen and starts a fresh selection, following
        // the pointer until the user does something decisive.
        void Begin();

        // Hides the overlay, forgets the selection, and gives the foreground back
        // to whoever had it.
        void Cancel() noexcept;

        void Render();

        LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    private:
        // How a drag is changing the selection, if one is in progress.
        enum class DragMode
        {
            None,
            NewRectangle,     // pulling a fresh rectangle out of empty space
            MoveOrResize,     // moving or resizing the one already there
        };

        void CreateDeviceIndependentResources();
        void CreateDeviceResources();
        void CreateSizeDependentResources();

        RECT ClientBounds() const noexcept;

        void OnMouseMove(POINT point);
        void OnLeftButtonDown(POINT point);
        void OnLeftButtonUp(POINT point);
        void OnWheel(POINT point, int delta);

        void RefreshSuggestion(POINT point, int expansionBudget = 2);
        void ApplyLadderLevel();
        void UpdateHover(POINT point);
        void Settle();
        void Invoke(ToolbarButton button);

        void DrawDim();
        void DrawSelection();
        void DrawSizeLabel();
        void DrawToolbar();
        void DrawTooltip();
        void DrawIconCopy(const RECT& box, ID2D1SolidColorBrush* background);
        void DrawIconCancel(const RECT& box);

        float Scaled(int value) const noexcept;

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
        winrt::com_ptr<ID2D1StrokeStyle> m_roundStroke;
        winrt::com_ptr<IDCompositionDevice> m_composition;
        winrt::com_ptr<IDCompositionTarget> m_target;
        winrt::com_ptr<IDCompositionVisual> m_visual;
        winrt::com_ptr<IDCompositionVirtualSurface> m_surface;

        winrt::com_ptr<ID2D1SolidColorBrush> m_dim;
        winrt::com_ptr<ID2D1SolidColorBrush> m_accent;
        winrt::com_ptr<ID2D1SolidColorBrush> m_handleFill;
        winrt::com_ptr<ID2D1SolidColorBrush> m_handleEdge;
        winrt::com_ptr<ID2D1SolidColorBrush> m_panel;
        winrt::com_ptr<ID2D1SolidColorBrush> m_panelEdge;
        winrt::com_ptr<ID2D1SolidColorBrush> m_hover;
        winrt::com_ptr<ID2D1SolidColorBrush> m_icon;
        winrt::com_ptr<ID2D1SolidColorBrush> m_tooltip;

        // The rectangle on show: either the user's, or the suggestion under the
        // pointer while "m_smart" is still true.
        RECT m_selection{};
        bool m_hasSelection{};

        // True while the selection is still following the pointer, which is what
        // "smart selection" means: nothing has been committed yet.
        bool m_smart{true};
        int m_depth{};            // wheel-adjusted level within the UIA tree
        ULONGLONG m_lastQuery{};  // throttles the UIA call

        // The last pointer position, in client pixels, so that hover state and
        // cursor shapes can be worked out without waiting for the next move.
        POINT m_pointer{};

        Toolbar m_toolbar{};
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

        HWND m_previousForeground{};
        bool m_trackingLeave{};

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
