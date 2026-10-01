#include "pch.h"

#include "hud/overlay.h"

#include "core/error.h"
#include "core/screen_capture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>

namespace agshot
{
    namespace
    {
        constexpr wchar_t kOverlayClass[] = L"AGShot.Overlay";
        constexpr wchar_t kOverlayTitle[] = L"AGShot";

        // No frame, no taskbar button, never in Alt+Tab, and no redirection bitmap
        // so that DirectComposition owns every pixel - including the alpha that
        // makes the rest of the screen show through.
        constexpr DWORD kOverlayStyle = WS_POPUP;
        constexpr DWORD kOverlayExStyle =
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;

        // All of these are DIPs at 96 DPI and are scaled on the way out.
        constexpr int kHandleSize = 8;
        constexpr int kBorderWidth = 2;
        constexpr int kCornerRadius = 6;
        constexpr int kTextSize = 13;
        constexpr int kLabelPadding = 7;
        constexpr int kTooltipPadding = 8;
        constexpr int kTooltipGap = 6;
        constexpr int kIconInset = 7;
        constexpr int kIconStroke = 2;
        constexpr int kButtonRadius = 4;

        // A drag that never reaches this size is a click that wobbled, not a
        // selection, and goes back to following the pointer.
        constexpr int kMinimumDrag = 3;

        // UIA is a cross-process call. The pointer moves far more often than a
        // suggestion can usefully change, so the call is throttled.
        constexpr ULONGLONG kQueryIntervalMs = 30;

        RECT VirtualScreen() noexcept
        {
            const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
            return RECT{ x, y,
                         x + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                         y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        }

        D2D1_RECT_F ToRectF(const RECT& rect) noexcept
        {
            return D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
                               static_cast<float>(rect.right), static_cast<float>(rect.bottom));
        }

        D2D1_ROUNDED_RECT ToRoundedRect(const RECT& rect, float radius) noexcept
        {
            return D2D1::RoundedRect(ToRectF(rect), radius, radius);
        }

        RECT Inset(const RECT& rect, int by) noexcept
        {
            return RECT{ rect.left + by, rect.top + by, rect.right - by, rect.bottom - by };
        }

        RECT RectFromPoints(POINT a, POINT b) noexcept
        {
            return RECT{ std::min(a.x, b.x), std::min(a.y, b.y),
                         std::max(a.x, b.x), std::max(a.y, b.y) };
        }

        // There is one overlay, and the window procedure has to reach it.
        Overlay* g_overlay{};

        LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            if (g_overlay == nullptr)
            {
                return DefWindowProcW(hwnd, message, wparam, lparam);
            }

            // DispatchMessageW has no handler above it, so an exception escaping a
            // message handler would not unwind: the CRT would call abort(). Unlike
            // the main window, a fault here does not have to kill the application -
            // the HUD can simply step out of the way and let the user try again.
            try
            {
                return g_overlay->HandleMessage(hwnd, message, wparam, lparam);
            }
            catch (...)
            {
                ReportCurrentException(__FILEW__, __LINE__);
                g_overlay->Cancel();
                return 0;
            }
        }
    }

    Overlay::~Overlay()
    {
        Destroy();
    }

    bool Overlay::Create(HINSTANCE instance)
    {
        m_instance = instance;
        m_dpi = static_cast<int>(GetDpiForSystem());
        if (m_dpi <= 0)
        {
            m_dpi = 96;
        }
        m_handleSize = MulDiv(kHandleSize, m_dpi, 96);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayWindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = kOverlayClass;
        if (RegisterClassExW(&wc) == 0)
        {
            return false;
        }

        m_bounds = VirtualScreen();

        // Before the window exists: creating it sends messages, and they arrive
        // here.
        g_overlay = this;

        m_window = CreateWindowExW(
            kOverlayExStyle, kOverlayClass, kOverlayTitle, kOverlayStyle,
            m_bounds.left, m_bounds.top,
            m_bounds.right - m_bounds.left, m_bounds.bottom - m_bounds.top,
            nullptr, nullptr, instance, nullptr);
        if (m_window == nullptr)
        {
            g_overlay = nullptr;
            return false;
        }

        CreateDeviceIndependentResources();
        CreateDeviceResources();

        RECT client{};
        AGSHOT_CHECK_WIN32(GetClientRect(m_window, &client));
        m_width = static_cast<UINT>(client.right - client.left);
        m_height = static_cast<UINT>(client.bottom - client.top);
        CreateSizeDependentResources();

        AGSHOT_CHECK_HR(m_target->SetRoot(m_visual.get()));
        AGSHOT_CHECK_HR(m_composition->Commit());
        return true;
    }

    void Overlay::Destroy() noexcept
    {
        if (m_window != nullptr)
        {
            DestroyWindow(m_window);
            m_window = nullptr;
        }
        if (g_overlay == this)
        {
            g_overlay = nullptr;
        }

        m_surface = nullptr;
        m_visual = nullptr;
        m_target = nullptr;
        m_composition = nullptr;
        m_roundStroke = nullptr;
        m_textFormat = nullptr;
        m_dwriteFactory = nullptr;
        m_icon = nullptr;
        m_tooltip = nullptr;
        m_hover = nullptr;
        m_panelEdge = nullptr;
        m_panel = nullptr;
        m_handleEdge = nullptr;
        m_handleFill = nullptr;
        m_accent = nullptr;
        m_dim = nullptr;
        m_d2dContext = nullptr;
        m_d2dDevice = nullptr;
        m_d2dFactory = nullptr;
        m_dxgiDevice = nullptr;
    }

    void Overlay::CreateDeviceIndependentResources()
    {
        AGSHOT_CHECK_HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.put()));

        AGSHOT_CHECK_HR(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.put_void())));

        AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            Scaled(kTextSize),
            L"en-US",
            m_textFormat.put()));

        // Round caps, so the cross of the cancel button reads as a drawn mark
        // rather than four clipped ends.
        const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 1.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        AGSHOT_CHECK_HR(m_d2dFactory->CreateStrokeStyle(properties, nullptr, 0, m_roundStroke.put()));
    }

    void Overlay::CreateDeviceResources()
    {
#if 1
        const D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
#else
        const D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_WARP };
#endif

        HRESULT hr = E_FAIL;
        winrt::com_ptr<ID3D11Device> device;
        for (D3D_DRIVER_TYPE driver : drivers)
        {
            hr = D3D11CreateDevice(
                nullptr,
                driver,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED
                    | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                device.put(),
                nullptr,
                nullptr);

            if (SUCCEEDED(hr))
            {
                break;
            }
        }
        AGSHOT_CHECK_HR(hr);

        AGSHOT_CHECK_HR(device->QueryInterface(__uuidof(IDXGIDevice), m_dxgiDevice.put_void()));

        AGSHOT_CHECK_HR(m_d2dFactory->CreateDevice(m_dxgiDevice.get(), m_d2dDevice.put()));
        AGSHOT_CHECK_HR(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.put()));

        AGSHOT_CHECK_HR(DCompositionCreateDevice(
            m_dxgiDevice.get(), __uuidof(IDCompositionDevice), m_composition.put_void()));
        AGSHOT_CHECK_HR(m_composition->CreateTargetForHwnd(m_window, TRUE, m_target.put()));
        AGSHOT_CHECK_HR(m_composition->CreateVisual(m_visual.put()));

        const auto brush = [this](float r, float g, float b, float a, winrt::com_ptr<ID2D1SolidColorBrush>& out)
        {
            AGSHOT_CHECK_HR(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), out.put()));
        };

        brush(0.0f, 0.0f, 0.0f, 0.45f, m_dim);          // everything outside the selection
        brush(0.18f, 0.49f, 0.94f, 1.0f, m_accent);      // the selection border
        brush(1.0f, 1.0f, 1.0f, 1.0f, m_handleFill);
        brush(0.10f, 0.10f, 0.12f, 1.0f, m_handleEdge);
        brush(0.14f, 0.15f, 0.18f, 0.97f, m_panel);
        brush(1.0f, 1.0f, 1.0f, 0.14f, m_panelEdge);
        brush(1.0f, 1.0f, 1.0f, 0.13f, m_hover);
        brush(0.96f, 0.97f, 0.99f, 1.0f, m_icon);
        brush(0.06f, 0.06f, 0.08f, 0.97f, m_tooltip);
    }

    void Overlay::CreateSizeDependentResources()
    {
        if (m_width == 0 || m_height == 0)
        {
            return;
        }

        if (!m_surface)
        {
            AGSHOT_CHECK_HR(m_composition->CreateVirtualSurface(
                m_width, m_height,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_PREMULTIPLIED,
                m_surface.put()));
            AGSHOT_CHECK_HR(m_visual->SetContent(m_surface.get()));
        }
        else
        {
            AGSHOT_CHECK_HR(m_surface->Resize(m_width, m_height));
        }

        AGSHOT_CHECK_HR(m_composition->Commit());
    }

    float Overlay::Scaled(int value) const noexcept
    {
        return static_cast<float>(MulDiv(value, m_dpi, 96));
    }

    RECT Overlay::ClientBounds() const noexcept
    {
        return RECT{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    }

    void Overlay::Begin()
    {
        if (m_window == nullptr)
        {
            return;
        }

        // Already up: a second press has nothing to add. Freezing again would
        // mean hiding the overlay to photograph the screen without it, and the
        // picture the user is already looking at is the one they meant.
        if (IsWindowVisible(m_window))
        {
            return;
        }

        m_previousForeground = GetForegroundWindow();
        if (m_previousForeground == m_window)
        {
            m_previousForeground = nullptr;
        }

        // The desktop can have changed shape since the last capture: a monitor
        // added, a resolution switched.
        const RECT bounds = VirtualScreen();
        if (!EqualRect(&bounds, &m_bounds))
        {
            m_bounds = bounds;
            SetWindowPos(m_window, HWND_TOPMOST,
                         m_bounds.left, m_bounds.top,
                         m_bounds.right - m_bounds.left, m_bounds.bottom - m_bounds.top,
                         SWP_NOACTIVATE);
        }

        // Freeze the z-order/window snapshot while the overlay is still hidden.
        // UIA nodes are expanded lazily on this same STA thread and cached by the
        // picker; mouse moves never re-enumerate the desktop.
        m_picker.Freeze(m_window);
        m_ladder.clear();
        m_lastQuery = 0;

        // Taken while the overlay is still hidden, so what it holds is the
        // desktop rather than the previous capture.
        m_image.Capture(m_bounds);
        m_background = nullptr;
        if (!m_image.empty())
        {
            const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f,
                96.0f);
            AGSHOT_CHECK_HR(m_d2dContext->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(m_image.width()),
                            static_cast<UINT32>(m_image.height())),
                m_image.pixels(),
                static_cast<UINT32>(m_image.stride()),
                properties,
                m_background.put()));
        }

        m_hasSelection = false;
        m_smart = true;
        m_depth = 0;
        m_dragMode = DragMode::None;
        m_dragPart = SelectionPart::None;
        m_pressedButton = ToolbarButton::None;
        m_pendingConfirm = false;
        m_toolbarVisible = false;
        m_toolbar.hovered = ToolbarButton::None;
        m_trackingLeave = false;

        // Activating is deliberate: the overlay needs the keyboard for Escape and
        // Return, and the mouse for everything else.
        SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(m_window);
        SetFocus(m_window);

        // Suggest something straight away rather than waiting for the first move.
        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            ScreenToClient(m_window, &cursor);
            m_pointer = cursor;
            RefreshSuggestion(cursor, 6);
        }

        Render();
    }

    void Overlay::Cancel() noexcept
    {
        if (m_window == nullptr)
        {
            return;
        }

        ShowWindow(m_window, SW_HIDE);

        m_hasSelection = false;
        m_smart = true;
        m_depth = 0;
        m_dragMode = DragMode::None;
        m_dragPart = SelectionPart::None;
        m_pressedButton = ToolbarButton::None;
        m_pendingConfirm = false;
        m_toolbarVisible = false;
        m_toolbar.hovered = ToolbarButton::None;

        if (m_previousForeground != nullptr && IsWindow(m_previousForeground))
        {
            SetForegroundWindow(m_previousForeground);
        }
        m_previousForeground = nullptr;
    }

    void Overlay::RefreshSuggestion(POINT point, int expansionBudget)
    {
        const ULONGLONG now = GetTickCount64();
        if (now - m_lastQuery < kQueryIntervalMs)
        {
            return;
        }
        m_lastQuery = now;

        // The whole ladder is rebuilt here, on the move, so that the wheel can
        // then walk it without another round of cross-process calls.
        const POINT screen{ point.x + m_bounds.left, point.y + m_bounds.top };
        m_picker.Ladder(screen, m_ladder, expansionBudget);
        ApplyLadderLevel();
    }

    void Overlay::ApplyLadderLevel()
    {
        if (m_ladder.empty())
        {
            m_hasSelection = false;
            return;
        }

        // Level 0 is the smallest thing the pointer is on, which is what a person
        // means by "this thing"; each notch up widens it.
        const std::size_t level = static_cast<std::size_t>(
            std::min(-m_depth, static_cast<int>(m_ladder.size()) - 1));
        const RECT& found = m_ladder[level];

        const RECT local{ found.left - m_bounds.left, found.top - m_bounds.top,
                          found.right - m_bounds.left, found.bottom - m_bounds.top };

        // A picker can report something that hangs off the edge of the desktop,
        // and a selection has to stay something that can be captured.
        const RECT bounds = ClientBounds();
        RECT clipped{};
        if (IntersectRect(&clipped, &local, &bounds) && !IsEmptyRect(clipped))
        {
            m_selection = clipped;
            m_hasSelection = true;
            return;
        }

        m_hasSelection = false;
    }

    void Overlay::UpdateHover(POINT point)
    {
        const ToolbarButton button = m_toolbarVisible
            ? HitTestToolbar(m_toolbar, point)
            : ToolbarButton::None;

        if (button != m_toolbar.hovered)
        {
            m_toolbar.hovered = button;
        }
    }

    void Overlay::Settle()
    {
        m_toolbarVisible = true;
        LayoutToolbar(m_toolbar, m_selection, ClientBounds(), m_dpi);
        m_toolbar.hovered = HitTestToolbar(m_toolbar, m_pointer);
    }

    void Overlay::OnMouseMove(POINT point)
    {
        m_pointer = point;

        if (!m_trackingLeave)
        {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = m_window;
            if (TrackMouseEvent(&track))
            {
                m_trackingLeave = true;
            }
        }

        switch (m_dragMode)
        {
        case DragMode::NewRectangle:
        {
            // Until the pointer has actually moved, this is still a click, and in
            // smart mode a click means "confirm the suggestion".
            if (m_pendingConfirm)
            {
                const int dx = point.x > m_dragOrigin.x ? point.x - m_dragOrigin.x : m_dragOrigin.x - point.x;
                const int dy = point.y > m_dragOrigin.y ? point.y - m_dragOrigin.y : m_dragOrigin.y - point.y;
                if (dx < kMinimumDrag && dy < kMinimumDrag)
                {
                    break;
                }
                // It is a drag after all, so the suggestion is abandoned.
                m_pendingConfirm = false;
                m_hasSelection = false;
            }

            const RECT bounds = ClientBounds();
            RECT candidate = RectFromPoints(m_dragOrigin, point);
            RECT clipped{};
            if (IntersectRect(&clipped, &candidate, &bounds))
            {
                m_selection = clipped;
                m_hasSelection = !IsEmptyRect(clipped);
            }
            break;
        }

        case DragMode::MoveOrResize:
            m_selection = DragSelection(m_dragStart, m_dragPart, m_dragOrigin, point, ClientBounds());
            m_hasSelection = true;
            break;

        default:
            if (m_smart)
            {
                RefreshSuggestion(point);
            }
            else
            {
                UpdateHover(point);
            }
            break;
        }

        Render();
    }

    void Overlay::OnLeftButtonDown(POINT point)
    {
        SetCapture(m_window);

        if (m_toolbarVisible)
        {
            const ToolbarButton button = HitTestToolbar(m_toolbar, point);
            if (button != ToolbarButton::None)
            {
                // Acted on when the button is released, so that a press which
                // slides off can still be abandoned.
                m_pressedButton = button;
                return;
            }
        }

        // From here on the pointer is the user's, not the suggestion's. Leaving
        // this set would let the next mouse move quietly replace a rectangle the
        // user had just drawn with whatever happens to be under the cursor.
        const bool wasFollowing = m_smart;
        m_smart = false;
        m_depth = 0;

        if (wasFollowing)
        {
            // What this press means is not decided yet: it is a click if it is
            // released where it started, and a new selection if it is dragged.
            m_dragMode = DragMode::NewRectangle;
            m_dragOrigin = point;
            m_dragStart = m_selection;
            m_pendingConfirm = m_hasSelection;
            if (!m_hasSelection)
            {
                m_toolbarVisible = false;
            }
            return;
        }

        const SelectionPart part = HitTestSelection(m_selection, point, m_handleSize);
        if (part != SelectionPart::None)
        {
            m_dragMode = DragMode::MoveOrResize;
            m_dragPart = part;
            m_dragOrigin = point;
            m_dragStart = m_selection;
            return;
        }

        m_depth = 0;
        m_toolbarVisible = false;
        m_dragMode = DragMode::NewRectangle;
        m_dragOrigin = point;
        m_hasSelection = false;
    }

    void Overlay::OnLeftButtonUp(POINT point)
    {
        if (GetCapture() == m_window)
        {
            ReleaseCapture();
        }

        if (m_pressedButton != ToolbarButton::None)
        {
            const ToolbarButton pressed = m_pressedButton;
            m_pressedButton = ToolbarButton::None;

            if (HitTestToolbar(m_toolbar, point) == pressed)
            {
                Invoke(pressed);
            }
            return;
        }

        if (m_dragMode == DragMode::None)
        {
            return;
        }

        const bool wasNew = m_dragMode == DragMode::NewRectangle;
        m_dragMode = DragMode::None;
        m_dragPart = SelectionPart::None;

        if (m_pendingConfirm)
        {
            // The press never turned into a drag, so it was a click: the pointer
            // was sitting on a suggestion and the user said yes to it.
            m_pendingConfirm = false;
            m_smart = false;
            m_depth = 0;
            Settle();
            Render();
            return;
        }

        if (wasNew)
        {
            const RECT& r = m_selection;
            if (!m_hasSelection || IsEmptyRect(r)
                || (r.right - r.left) < kMinimumDrag || (r.bottom - r.top) < kMinimumDrag)
            {
                // A click that drew nothing: go back to following the pointer
                // rather than leaving a sliver behind.
                m_hasSelection = false;
                m_smart = true;
                m_depth = 0;
                RefreshSuggestion(point);
                Render();
                return;
            }
        }

        Settle();
        Render();
    }

    void Overlay::OnWheel(POINT point, int delta)
    {
        // Only while nothing has been committed: once the user has drawn or
        // confirmed a rectangle, the wheel is not a way to change it.
        if (!m_smart)
        {
            return;
        }

        // Level 0 is the innermost element under the pointer, which is what a
        // person means by "this thing"; each notch up widens the suggestion
        // towards the whole window, and down narrows it back.
        m_depth += delta < 0 ? 1 : -1;
        m_depth = std::clamp(m_depth, -8, 0);

        // A wheel notch explicitly asks for a deeper cached hit path. Ordinary
        // mouse movement keeps the smaller initial expansion budget.
        const POINT screen{ point.x + m_bounds.left, point.y + m_bounds.top };
        m_picker.Ladder(screen, m_ladder, 6);
        m_lastQuery = GetTickCount64();
        ApplyLadderLevel();
        Render();
    }

    void Overlay::Invoke(ToolbarButton button)
    {
        if (button == ToolbarButton::Cancel)
        {
            Cancel();
            return;
        }

        if (button != ToolbarButton::Copy)
        {
            return;
        }

        const RECT region{ m_selection.left + m_bounds.left,
                           m_selection.top + m_bounds.top,
                           m_selection.right + m_bounds.left,
                           m_selection.bottom + m_bounds.top };

        // The picture is already in memory, so this is a crop rather than a
        // second look at the screen: nothing to hide, nothing to wait for, and
        // no way for the dimming or the toolbar to end up in it.
        try
        {
            m_image.CopyToClipboard(m_window, region);
        }
        catch (...)
        {
            // Copying is the whole point of the button, so a failure has to be
            // said out loud rather than swallowed.
            ReportCurrentException(__FILEW__, __LINE__);
        }

        Cancel();
    }

    void Overlay::Render()
    {
        if (!m_surface || !m_d2dContext || m_width == 0 || m_height == 0)
        {
            return;
        }

        const RECT update = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };

        winrt::com_ptr<IDXGISurface> updateSurface;
        POINT updateOffset{};
        AGSHOT_CHECK_HR(m_surface->BeginDraw(
            &update, __uuidof(IDXGISurface), updateSurface.put_void(), &updateOffset));

        // One DIP per pixel, so that what is drawn and what gets captured are the
        // same thing and no scaling creeps in between them.
        m_d2dContext->SetDpi(96.0f, 96.0f);

        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f);

        winrt::com_ptr<ID2D1Bitmap1> tile;
        AGSHOT_CHECK_HR(m_d2dContext->CreateBitmapFromDxgiSurface(updateSurface.get(), &properties, tile.put()));
        m_d2dContext->SetTarget(tile.get());

        m_d2dContext->BeginDraw();

        // BeginDraw hands back a tile out of DirectComposition's atlas rather than
        // a view of the surface, so shift the surface's origin onto it.
        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x), static_cast<float>(updateOffset.y)));

        // The frozen picture covers all of this, so the clear only decides what a
        // hole would look like. Black, so that one would read as a hole rather
        // than as the desktop leaking through.
        m_d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));

        if (m_background)
        {
            m_d2dContext->DrawBitmap(
                m_background.get(),
                D2D1::RectF(0.0f, 0.0f,
                            static_cast<float>(m_width), static_cast<float>(m_height)),
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }

        DrawDim();
        if (m_hasSelection)
        {
            DrawSelection();
        }
        if (m_toolbarVisible)
        {
            DrawToolbar();
            DrawTooltip();
        }

        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        AGSHOT_CHECK_HR(m_d2dContext->EndDraw());

        m_d2dContext->SetTarget(nullptr);
        tile = nullptr;

        AGSHOT_CHECK_HR(m_surface->EndDraw());
        AGSHOT_CHECK_HR(m_composition->Commit());
    }

    void Overlay::DrawDim()
    {
        const D2D1_RECT_F full = D2D1::RectF(0.0f, 0.0f,
                                             static_cast<float>(m_width),
                                             static_cast<float>(m_height));

        if (!m_hasSelection)
        {
            m_d2dContext->FillRectangle(full, m_dim.get());
            return;
        }

        // Four rectangles around the selection rather than one with a hole cut in
        // it: cheaper than a mask layer and just as exact.
        const RECT& s = m_selection;
        const float left = static_cast<float>(s.left);
        const float top = static_cast<float>(s.top);
        const float right = static_cast<float>(s.right);
        const float bottom = static_cast<float>(s.bottom);

        m_d2dContext->FillRectangle(D2D1::RectF(full.left, full.top, full.right, top), m_dim.get());
        m_d2dContext->FillRectangle(D2D1::RectF(full.left, bottom, full.right, full.bottom), m_dim.get());
        m_d2dContext->FillRectangle(D2D1::RectF(full.left, top, left, bottom), m_dim.get());
        m_d2dContext->FillRectangle(D2D1::RectF(right, top, full.right, bottom), m_dim.get());
    }

    void Overlay::DrawSelection()
    {
        const float stroke = Scaled(kBorderWidth);

        // Inset by half the stroke so that the border sits inside the selection
        // rather than straddling it and covering a line of the picture.
        RECT border = m_selection;
        const int half = static_cast<int>(stroke / 2.0f);
        InflateRect(&border, -half, -half);

        m_d2dContext->DrawRectangle(ToRectF(border), m_accent.get(), stroke);

        if (!m_smart)
        {
            for (const RECT& handle : HandleRects(m_selection, m_handleSize))
            {
                const D2D1_RECT_F box = ToRectF(handle);
                m_d2dContext->FillRectangle(box, m_handleFill.get());
                m_d2dContext->DrawRectangle(box, m_handleEdge.get(), 1.0f);
            }
        }

        DrawSizeLabel();
    }

    void Overlay::DrawSizeLabel()
    {
        wchar_t text[64]{};
        swprintf_s(text, L"%d x %d",
                   m_selection.right - m_selection.left,
                   m_selection.bottom - m_selection.top);

        winrt::com_ptr<IDWriteTextLayout> layout;
        AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextLayout(
            text,
            static_cast<UINT32>(wcslen(text)),
            m_textFormat.get(),
            static_cast<float>(m_width),
            static_cast<float>(m_height),
            layout.put()));

        DWRITE_TEXT_METRICS metrics{};
        AGSHOT_CHECK_HR(layout->GetMetrics(&metrics));

        const float padding = Scaled(kLabelPadding);
        const float width = metrics.width + padding * 2.0f;
        const float height = metrics.height + padding;

        float x = static_cast<float>(m_selection.left);
        float y = static_cast<float>(m_selection.top) - height - Scaled(3);
        if (y < 0.0f)
        {
            y = static_cast<float>(m_selection.top) + Scaled(3);
        }
        if (x + width > static_cast<float>(m_width))
        {
            x = static_cast<float>(m_width) - width;
        }
        x = std::max(x, 0.0f);

        const RECT box{ static_cast<LONG>(x), static_cast<LONG>(y),
                        static_cast<LONG>(x + width), static_cast<LONG>(y + height) };

        m_d2dContext->FillRoundedRectangle(ToRoundedRect(box, Scaled(4)), m_panel.get());
        m_d2dContext->DrawTextLayout(
            D2D1::Point2F(x + padding, y + padding * 0.5f), layout.get(), m_icon.get());
    }

    void Overlay::DrawToolbar()
    {
        const float radius = Scaled(kCornerRadius);

        m_d2dContext->FillRoundedRectangle(ToRoundedRect(m_toolbar.panel, radius), m_panel.get());
        m_d2dContext->DrawRoundedRectangle(ToRoundedRect(m_toolbar.panel, radius), m_panelEdge.get(), 1.0f);

        const bool copyHovered = m_toolbar.hovered == ToolbarButton::Copy;
        const bool cancelHovered = m_toolbar.hovered == ToolbarButton::Cancel;

        if (copyHovered)
        {
            m_d2dContext->FillRoundedRectangle(
                ToRoundedRect(m_toolbar.copy, Scaled(kButtonRadius)), m_hover.get());
        }
        if (cancelHovered)
        {
            m_d2dContext->FillRoundedRectangle(
                ToRoundedRect(m_toolbar.cancel, Scaled(kButtonRadius)), m_hover.get());
        }

        const int inset = MulDiv(kIconInset, m_dpi, 96);
        DrawIconCopy(Inset(m_toolbar.copy, inset), copyHovered ? m_hover.get() : m_panel.get());
        DrawIconCancel(Inset(m_toolbar.cancel, inset));
    }

    void Overlay::DrawIconCopy(const RECT& box, ID2D1SolidColorBrush* background)
    {
        const float stroke = Scaled(kIconStroke);
        const float radius = Scaled(2);
        const int width = box.right - box.left;
        const int height = box.bottom - box.top;

        // Two sheets. The front one is filled with the button's own colour so that
        // it punches a hole in the back one rather than showing it through.
        const RECT back{ box.left, box.top,
                         box.left + MulDiv(width, 62, 100),
                         box.top + MulDiv(height, 72, 100) };
        const RECT front{ box.left + MulDiv(width, 30, 100),
                          box.top + MulDiv(height, 28, 100),
                          box.right, box.bottom };

        m_d2dContext->DrawRoundedRectangle(ToRoundedRect(back, radius), m_icon.get(), stroke);
        m_d2dContext->FillRoundedRectangle(ToRoundedRect(front, radius), background);
        m_d2dContext->DrawRoundedRectangle(ToRoundedRect(front, radius), m_icon.get(), stroke);
    }

    void Overlay::DrawIconCancel(const RECT& box)
    {
        const float stroke = Scaled(kIconStroke);
        const float inset = static_cast<float>(box.right - box.left) * 0.24f;

        const D2D1_POINT_2F topLeft{ static_cast<float>(box.left) + inset,
                                     static_cast<float>(box.top) + inset };
        const D2D1_POINT_2F bottomRight{ static_cast<float>(box.right) - inset,
                                         static_cast<float>(box.bottom) - inset };
        const D2D1_POINT_2F topRight{ static_cast<float>(box.right) - inset,
                                      static_cast<float>(box.top) + inset };
        const D2D1_POINT_2F bottomLeft{ static_cast<float>(box.left) + inset,
                                        static_cast<float>(box.bottom) - inset };

        m_d2dContext->DrawLine(topLeft, bottomRight, m_icon.get(), stroke, m_roundStroke.get());
        m_d2dContext->DrawLine(topRight, bottomLeft, m_icon.get(), stroke, m_roundStroke.get());
    }

    void Overlay::DrawTooltip()
    {
        if (m_toolbar.hovered == ToolbarButton::None)
        {
            return;
        }

        const LPCWSTR text = ToolbarLabel(m_toolbar.hovered);
        if (text == nullptr || text[0] == L'\0')
        {
            return;
        }

        winrt::com_ptr<IDWriteTextLayout> layout;
        AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextLayout(
            text,
            static_cast<UINT32>(wcslen(text)),
            m_textFormat.get(),
            static_cast<float>(m_width),
            static_cast<float>(m_height),
            layout.put()));

        DWRITE_TEXT_METRICS metrics{};
        AGSHOT_CHECK_HR(layout->GetMetrics(&metrics));

        const float padding = Scaled(kTooltipPadding);
        const float width = metrics.width + padding * 2.0f;
        const float height = metrics.height + padding;
        const float gap = Scaled(kTooltipGap);

        const RECT& button = m_toolbar.hovered == ToolbarButton::Copy
            ? m_toolbar.copy
            : m_toolbar.cancel;

        float x = static_cast<float>(button.left + button.right) / 2.0f - width / 2.0f;
        float y = static_cast<float>(button.bottom) + gap;

        if (y + height > static_cast<float>(m_height))
        {
            y = static_cast<float>(button.top) - gap - height;
        }
        x = std::clamp(x, 0.0f, std::max(0.0f, static_cast<float>(m_width) - width));
        y = std::clamp(y, 0.0f, std::max(0.0f, static_cast<float>(m_height) - height));

        const RECT box{ static_cast<LONG>(x), static_cast<LONG>(y),
                        static_cast<LONG>(x + width), static_cast<LONG>(y + height) };

        m_d2dContext->FillRoundedRectangle(ToRoundedRect(box, Scaled(4)), m_tooltip.get());
        m_d2dContext->DrawTextLayout(
            D2D1::Point2F(x + padding, y + padding * 0.5f), layout.get(), m_icon.get());
    }

    LRESULT Overlay::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED)
            {
                m_width = static_cast<UINT>(LOWORD(lparam));
                m_height = static_cast<UINT>(HIWORD(lparam));
                if (m_d2dContext && m_width > 0 && m_height > 0)
                {
                    CreateSizeDependentResources();
                    Render();
                }
            }
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            Render();
            return 0;
        }

        case WM_ERASEBKGND:
            // Nothing is drawn through GDI; DirectComposition owns every pixel.
            return 1;

        case WM_GETOBJECT:
            // Answering this is what would put the HUD into the accessibility
            // tree. Saying nothing keeps it out, and that is what makes smart
            // selection possible at all: with the overlay in the tree, every
            // query about the pointer would come back describing the overlay
            // rather than the thing underneath it.
            return 0;

        case WM_MOUSEMOVE:
            OnMouseMove(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });
            return 0;

        case WM_LBUTTONDOWN:
            OnLeftButtonDown(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });
            return 0;

        case WM_LBUTTONUP:
            OnLeftButtonUp(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });
            return 0;

        case WM_RBUTTONUP:
            // The usual way out of a capture overlay, and cheaper than finding the
            // button.
            Cancel();
            return 0;

        case WM_MOUSEWHEEL:
        {
            // Wheel messages carry screen coordinates, not client ones.
            POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(hwnd, &point);
            OnWheel(point, GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        }

        case WM_MOUSELEAVE:
            m_trackingLeave = false;
            m_toolbar.hovered = ToolbarButton::None;
            Render();
            return 0;

        case WM_SETCURSOR:
        {
            LPCWSTR shape = IDC_CROSS;
            if (m_dragMode == DragMode::MoveOrResize)
            {
                shape = CursorForPart(m_dragPart);
            }
            else if (m_toolbarVisible && HitTestToolbar(m_toolbar, m_pointer) != ToolbarButton::None)
            {
                shape = IDC_HAND;
            }
            else if (!m_smart && m_hasSelection)
            {
                shape = CursorForPart(HitTestSelection(m_selection, m_pointer, m_handleSize));
            }

            SetCursor(LoadCursorW(nullptr, shape));
            return TRUE;
        }

        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE)
            {
                Cancel();
                return 0;
            }
            if (wparam == VK_RETURN && !m_smart && m_hasSelection)
            {
                Invoke(ToolbarButton::Copy);
                return 0;
            }
            break;

        case WM_DESTROY:
            return 0;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
