#include "pch.h"

#include "hud/overlay.h"

#include "core/error.h"

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

        constexpr int kHandleSize = 8;
        constexpr int kMinimumDrag = 3;
        constexpr ULONGLONG kQueryIntervalMs = 30;
        constexpr ULONGLONG kSelectionAnimationMs = 120;
        constexpr UINT kFrameMessage = WM_APP + 0x4A;
        constexpr UINT kMemoryPressureMessage = WM_APP + 0x4D;
        constexpr UINT kVramPressureMessage = WM_APP + 0x4E;

        RECT VirtualScreen() noexcept
        {
            const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
            return RECT{ x, y,
                         x + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                         y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        }

        RECT RectFromPoints(POINT a, POINT b) noexcept
        {
            return RECT{ std::min(a.x, b.x), std::min(a.y, b.y),
                         std::max(a.x, b.x), std::max(a.y, b.y) };
        }

        LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            if (message == WM_NCCREATE)
            {
                const auto* created = reinterpret_cast<const CREATESTRUCTW*>(lparam);
                auto* overlay = static_cast<Overlay*>(created->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
                return overlay != nullptr
                    ? DefWindowProcW(hwnd, message, wparam, lparam)
                    : FALSE;
            }

            auto* overlay = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (overlay == nullptr)
            {
                return DefWindowProcW(hwnd, message, wparam, lparam);
            }

            // DispatchMessageW has no handler above it, so an exception escaping a
            // message handler would not unwind: the CRT would call abort(). Unlike
            // the main window, a fault here does not have to kill the application -
            // the HUD can simply step out of the way and let the user try again.
            try
            {
                return overlay->HandleMessage(hwnd, message, wparam, lparam);
            }
            catch (...)
            {
                ReportCurrentException(__FILEW__, __LINE__);
                overlay->Cancel();
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
        m_handleSize = kHandleSize;

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

        m_window = CreateWindowExW(
            kOverlayExStyle, kOverlayClass, kOverlayTitle, kOverlayStyle,
            m_bounds.left, m_bounds.top,
            m_bounds.right - m_bounds.left, m_bounds.bottom - m_bounds.top,
            nullptr, nullptr, instance, this);
        if (m_window == nullptr)
        {
            return false;
        }

        int dpi = static_cast<int>(GetDpiForWindow(m_window));
        if (dpi <= 0)
        {
            dpi = 96;
        }
        m_handleSize = MulDiv(kHandleSize, dpi, 96);

        m_memoryWatch.Initialize(m_window, kMemoryPressureMessage);
        m_device.Initialize(m_window, dpi, kVramPressureMessage);

        RECT client{};
        AGSHOT_CHECK_WIN32(GetClientRect(m_window, &client));
        m_width = static_cast<UINT>(client.right - client.left);
        m_height = static_cast<UINT>(client.bottom - client.top);

        const HRESULT hr = m_device.ResizeSurface(m_width, m_height);
        if (RenderDevice::IsDeviceLost(hr))
        {
            if (const HRESULT recoverHr = RecoverDeviceResources(); FAILED(recoverHr))
            {
                AGSHOT_CHECK_HR(recoverHr);
            }
        }
        else
        {
            AGSHOT_CHECK_HR(hr);
        }
        return true;
    }

    void Overlay::Destroy() noexcept
    {
        StopSelectionAnimation();
        m_clock.Stop();
        m_ocrPanel.Destroy(m_window);
        m_renderer.DiscardIndependentResources();
        m_memoryWatch.Destroy();
        m_device.Destroy();
        if (m_window != nullptr)
        {
            SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
            DestroyWindow(m_window);
            m_window = nullptr;
        }

        m_image.Clear();
        m_picker.Trim();
        std::vector<RECT>{}.swap(m_ladder);
    }

    HRESULT Overlay::RecoverDeviceResources() noexcept
    {
        m_ocrPanel.InvalidateSpinner();
        m_renderer.DiscardIndependentResources();
        const HRESULT hr = m_device.Recover(m_width, m_height);
        if (FAILED(hr))
        {
            return hr;
        }

        if (!m_device.hasSurface())
        {
            return S_OK;
        }

        const HRESULT backgroundHr = m_device.UpdateBackgroundBitmap(m_image);
        if (FAILED(backgroundHr))
        {
            m_device.DiscardDeviceResources();
            return backgroundHr;
        }

        m_failed = false;
        return S_OK;
    }

    RECT Overlay::ClientBounds() const noexcept
    {
        return RECT{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    }

    void Overlay::Begin()
    {
        if (m_window == nullptr || m_beginning)
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

        m_beginning = true;
        auto resetBeginning = wil::scope_exit([&] { m_beginning = false; });
        m_failed = false;

        m_previousForeground = GetForegroundWindow();
        if (m_previousForeground == m_window)
        {
            m_previousForeground = nullptr;
        }

        const RECT bounds = VirtualScreen();
        if (!EqualRect(&bounds, &m_bounds))
        {
            m_bounds = bounds;
            SetWindowPos(m_window, HWND_TOPMOST,
                         m_bounds.left, m_bounds.top,
                         m_bounds.right - m_bounds.left, m_bounds.bottom - m_bounds.top,
                         SWP_NOACTIVATE);
        }

        // Freeze the screen pixels immediately while the overlay is hidden, then
        // snapshot window bounds (with UIA COM setup deferred) and upload into
        // the warm background bitmap.
        m_image.Capture(m_bounds);
        m_picker.Freeze(m_window);
        m_ladder.clear();
        m_lastQuery = 0;
        StopSelectionAnimation();
        m_clock.Cancel();

        HRESULT backgroundHr = m_device.UpdateBackgroundBitmap(m_image);
        if (RenderDevice::IsDeviceLost(backgroundHr))
        {
            const HRESULT recoverHr = RecoverDeviceResources();
            if (FAILED(recoverHr))
            {
                GiveUpRendering(recoverHr, L"Overlay::RecoverDeviceResources");
                Cancel();
                return;
            }
            backgroundHr = S_OK;
        }
        AGSHOT_CHECK_HR(backgroundHr);

        m_selection = RECT{};
        m_displaySelection = RECT{};
        m_animationFrom = RECT{};
        m_animationTo = RECT{};
        m_animationStart = 0;
        m_hasSelection = false;
        m_smart = true;
        m_mode = OperationMode::EditSelection;
        m_ocrPanel.ResetSession();
        m_depth = 0;
        m_dragMode = DragMode::None;
        m_dragPart = SelectionPart::None;
        m_pressedButton = ToolbarButton::None;
        m_pendingConfirm = false;
        m_toolbarVisible = false;
        m_toolbar.hovered = ToolbarButton::None;

        // Resolve and draw the initial suggestion while hidden so the very first
        // visible frame already has both the frozen desktop and smart selection.
        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            ScreenToClient(m_window, &cursor);
            m_pointer = cursor;
            RefreshSuggestion(cursor, 6);
        }
        Render();

        if (!m_device.hasSurface() || m_failed)
        {
            Cancel();
            return;
        }

        // Activating is deliberate: the overlay needs the keyboard for Escape and
        // Return, and the mouse for everything else.
        SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(m_window);
        SetFocus(m_window);
    }

    void Overlay::Cancel() noexcept
    {
        if (m_window == nullptr)
        {
            return;
        }

        StopSelectionAnimation();
        m_clock.Cancel();
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

        // Always drop cross-process UIA element proxies and per-session text
        // state, and trim the virtual surface back to uncommitted so stale
        // pixels cannot flash on the next F1, while keeping the capture bitmap,
        // background texture, and loaded NPU OCR engine warm unless memory or
        // VRAM is tight.
        m_picker.Clear();
        m_ladder.clear();
        m_ocrPanel.CancelSession(m_window);
        m_device.TrimSurface();
        ReconcileMemoryState();

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

        // TODO: Consider resetting m_depth when the pointer moves to a different candidate.
        const POINT screen{ point.x + m_bounds.left, point.y + m_bounds.top };
        if (m_picker.Ladder(screen, m_ladder, expansionBudget) < 0)
        {
            return;
        }
        ApplyLadderLevel();
    }

    void Overlay::ApplyLadderLevel()
    {
        if (m_ladder.empty())
        {
            StopSelectionAnimation();
            m_hasSelection = false;
            return;
        }

        const std::size_t level = static_cast<std::size_t>(
            std::min(-m_depth, static_cast<int>(m_ladder.size()) - 1));
        const RECT& found = m_ladder[level];

        const RECT local{ found.left - m_bounds.left, found.top - m_bounds.top,
                          found.right - m_bounds.left, found.bottom - m_bounds.top };

        const RECT bounds = ClientBounds();
        RECT clipped{};
        if (IntersectRect(&clipped, &local, &bounds) && !IsEmptyRect(clipped))
        {
            StartSelectionAnimation(clipped);
            m_selection = clipped;
            m_hasSelection = true;
            return;
        }

        StopSelectionAnimation();
        m_hasSelection = false;
    }

    void Overlay::StartSelectionAnimation(const RECT& target)
    {
        if (!m_smart || !m_hasSelection)
        {
            StopSelectionAnimation();
            m_displaySelection = target;
            return;
        }

        if (m_animatingSelection && EqualRect(&m_animationTo, &target))
        {
            return;
        }

        if (EqualRect(&m_displaySelection, &target))
        {
            StopSelectionAnimation();
            return;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        m_animationFrom = m_displaySelection;
        m_animationTo = target;
        m_animationStart = now.QuadPart;
        m_animatingSelection = true;
        if (!m_clock.RequestFrame(m_window, kFrameMessage))
        {
            m_displaySelection = target;
            StopSelectionAnimation();
        }
    }

    // TODO(render): there is no dirty tracking behind this. Render() always
    // redraws the whole surface - the update rect handed to BeginDraw is the
    // full client area, never a sub-rectangle.
    void Overlay::RequestRender()
    {
        // Never commit frames while hidden or without a captured desktop:
        // Begin() renders synchronously once the background bitmap is ready.
        if (m_window == nullptr || !IsWindowVisible(m_window) || !m_device.hasBackground())
        {
            return;
        }

        ApplyCursor();

        if (!m_clock.RequestFrame(m_window, kFrameMessage))
        {
            Render();
        }
    }

    void Overlay::SetDebugStats(bool show)
    {
        if (m_stats.enabled() == show)
        {
            return;
        }
        m_stats.SetEnabled(show);
        RequestRender();
    }

    bool Overlay::AdvanceSelectionAnimation(LONGLONG now) noexcept
    {
        const LONGLONG elapsed = std::max<LONGLONG>(0, now - m_animationStart);
        const double duration = static_cast<double>(PerformanceCounterFrequency())
            * static_cast<double>(kSelectionAnimationMs) / 1000.0;
        const float linear = static_cast<float>(std::min(1.0, elapsed / duration));
        const float eased = 1.0f - (1.0f - linear) * (1.0f - linear);
        const auto interpolate = [eased](LONG from, LONG to)
        {
            return static_cast<LONG>(from + (to - from) * eased);
        };

        m_displaySelection = RECT{
            interpolate(m_animationFrom.left, m_animationTo.left),
            interpolate(m_animationFrom.top, m_animationTo.top),
            interpolate(m_animationFrom.right, m_animationTo.right),
            interpolate(m_animationFrom.bottom, m_animationTo.bottom) };

        if (linear >= 1.0f)
        {
            m_displaySelection = m_animationTo;
            m_animatingSelection = false;
            return false;
        }
        return true;
    }

    void Overlay::StopSelectionAnimation() noexcept
    {
        m_animatingSelection = false;
    }

    Overlay::HitTarget Overlay::HitTest(POINT point) const noexcept
    {
        if (m_toolbarVisible)
        {
            const ToolbarHitTarget target = HitTestToolbarTarget(m_toolbar, point);
            if (target == ToolbarHitTarget::Handle) return HitTarget::ToolbarHandle;
            if (target == ToolbarHitTarget::Panel) return HitTarget::ToolbarPanel;
            if (target != ToolbarHitTarget::None) return HitTarget::ToolbarButton;
        }

        if (!m_hasSelection)
        {
            return HitTarget::Canvas;
        }

        if (m_mode == OperationMode::TextRecognition)
        {
            const RECT track = m_ocrPanel.ScrollTrack(m_selection, m_device.dpi());
            if (m_ocrPanel.HasScrollbar(m_selection, m_device.dpi(),
                                        m_device.dwriteFactory(), m_device.textFormat())
                && PtInRect(&track, point))
            {
                return HitTarget::OcrScrollbar;
            }
            return PtInRect(&m_selection, point) ? HitTarget::OcrText : HitTarget::Canvas;
        }

        if (!m_smart)
        {
            const SelectionPart part = HitTestSelection(m_selection, point, m_handleSize);
            if (part == SelectionPart::Body)
            {
                return HitTarget::Selection;
            }
            if (part != SelectionPart::None)
            {
                return HitTarget::SelectionHandle;
            }
        }

        return PtInRect(&m_selection, point) ? HitTarget::Selection : HitTarget::Canvas;
    }

    LPCWSTR Overlay::CursorShapeForPoint(POINT point) const noexcept
    {
        if (m_dragMode == DragMode::MoveOrResize)
        {
            return CursorForPart(m_dragPart);
        }
        if (m_dragMode == DragMode::ToolbarMove)
        {
            return IDC_SIZEALL;
        }
        if (m_dragMode == DragMode::NewRectangle)
        {
            return IDC_CROSS;
        }

        switch (HitTest(point))
        {
        case HitTarget::ToolbarHandle:
            return IDC_SIZEALL;
        case HitTarget::ToolbarButton:
            return IDC_HAND;
        case HitTarget::ToolbarPanel:
        case HitTarget::OcrScrollbar:
            return IDC_ARROW;
        case HitTarget::OcrText:
            return IDC_IBEAM;
        case HitTarget::SelectionHandle:
            return CursorForPart(HitTestSelection(m_selection, point, m_handleSize));
        case HitTarget::Selection:
            return m_smart ? IDC_CROSS : IDC_SIZEALL;
        case HitTarget::Canvas:
        default:
            return m_mode == OperationMode::TextRecognition ? IDC_ARROW : IDC_CROSS;
        }
    }

    void Overlay::ApplyCursor()
    {
        if (m_window == nullptr || !IsWindowVisible(m_window))
        {
            return;
        }

        POINT point{};
        if (!GetCursorPos(&point))
        {
            return;
        }
        ScreenToClient(m_window, &point);

        const RECT bounds = ClientBounds();
        if (!PtInRect(&bounds, point))
        {
            return;
        }
        SetCursor(LoadCursorW(nullptr, CursorShapeForPoint(point)));
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
        m_toolbarVisible = LayoutToolbar(m_toolbar, m_selection, ClientBounds(), m_device.dpi());
        m_toolbar.hovered = m_toolbarVisible
            ? HitTestToolbar(m_toolbar, m_pointer)
            : ToolbarButton::None;
    }

    void Overlay::ShowOcrContextMenu(POINT point)
    {
        const HitTarget target = HitTest(point);
        if (target == HitTarget::ToolbarPanel || target == HitTarget::ToolbarHandle
            || target == HitTarget::ToolbarButton || target == HitTarget::OcrScrollbar)
        {
            return;
        }
        if (target != HitTarget::OcrText)
        {
            Cancel();
            return;
        }

        m_ocrPanel.ShowContextMenu(
            m_window, m_selection, point, m_device.dpi(),
            m_device.dwriteFactory(), m_device.textFormat());
        RequestRender();
    }

    bool Overlay::PointerEvent(WPARAM wparam, POINTER_INFO& info, POINT& point) const noexcept
    {
        info = POINTER_INFO{};
        if (!GetPointerInfo(GET_POINTERID_WPARAM(wparam), &info))
        {
            return false;
        }

        point = info.ptPixelLocation;
        ScreenToClient(m_window, &point);
        return info.pointerType == PT_MOUSE || info.pointerType == PT_PEN;
    }

    void Overlay::OnMouseMove(POINT point)
    {
        UpdateHover(point);

        if (point.x == m_pointer.x && point.y == m_pointer.y)
        {
            return;
        }

        m_pointer = point;

        if (m_ocrPanel.OnMouseMove(m_selection, point, m_device.dpi(),
                                   m_device.dwriteFactory(), m_device.textFormat()))
        {
            RequestRender();
            return;
        }

        switch (m_dragMode)
        {
        case DragMode::NewRectangle:
        {
            if (m_pendingConfirm)
            {
                const int dx = point.x > m_dragOrigin.x ? point.x - m_dragOrigin.x : m_dragOrigin.x - point.x;
                const int dy = point.y > m_dragOrigin.y ? point.y - m_dragOrigin.y : m_dragOrigin.y - point.y;
                if (dx < kMinimumDrag && dy < kMinimumDrag)
                {
                    break;
                }
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

        case DragMode::ToolbarMove:
            DragToolbar(m_toolbar, m_toolbarDragStart, m_toolbarDragOrigin, point, ClientBounds());
            m_toolbar.hovered = ToolbarButton::None;
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

        RequestRender();
    }

    void Overlay::OnLeftButtonDown(POINT point)
    {
        const HitTarget hit = HitTest(point);

        if (hit == HitTarget::ToolbarButton || hit == HitTarget::ToolbarHandle
            || hit == HitTarget::ToolbarPanel)
        {
            switch (HitTestToolbarTarget(m_toolbar, point))
            {
            case ToolbarHitTarget::EditSelection:
                m_pressedButton = ToolbarButton::EditSelection;
                return;
            case ToolbarHitTarget::TextRecognition:
                m_pressedButton = ToolbarButton::TextRecognition;
                return;
            case ToolbarHitTarget::Copy:
                m_pressedButton = ToolbarButton::Copy;
                return;
            case ToolbarHitTarget::Cancel:
                m_pressedButton = ToolbarButton::Cancel;
                return;
            case ToolbarHitTarget::Handle:
                m_dragMode = DragMode::ToolbarMove;
                m_toolbarDragOrigin = point;
                m_toolbarDragStart = m_toolbar;
                m_toolbar.hovered = ToolbarButton::None;
                return;
            case ToolbarHitTarget::Panel:
            case ToolbarHitTarget::None:
                return;
            }
        }

        if (m_mode == OperationMode::TextRecognition)
        {
            if (hit == HitTarget::OcrText || hit == HitTarget::OcrScrollbar)
            {
                bool needsRender = false;
                m_ocrPanel.OnLeftButtonDown(m_window, m_selection, point, m_device.dpi(),
                                            m_device.dwriteFactory(), m_device.textFormat(), needsRender);
                if (needsRender)
                {
                    RequestRender();
                }
            }
            return;
        }

        const bool wasFollowing = m_smart;
        StopSelectionAnimation();
        m_displaySelection = m_selection;
        m_smart = false;
        m_depth = 0;

        if (wasFollowing)
        {
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
            m_toolbarVisible = false;
            m_toolbar.hovered = ToolbarButton::None;
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
        m_pointer = point;

        if (m_ocrPanel.OnLeftButtonUp(m_window, m_selection, point, m_device.dpi(),
                                      m_device.dwriteFactory(), m_device.textFormat()))
        {
            RequestRender();
            return;
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

        if (m_dragMode == DragMode::ToolbarMove)
        {
            m_dragMode = DragMode::None;
            UpdateHover(point);
            RequestRender();
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
            m_pendingConfirm = false;
            m_smart = false;
            m_depth = 0;
            Settle();
            RequestRender();
            return;
        }

        if (wasNew)
        {
            const RECT& r = m_selection;
            if (!m_hasSelection || IsEmptyRect(r)
                || (r.right - r.left) < kMinimumDrag || (r.bottom - r.top) < kMinimumDrag)
            {
                m_hasSelection = false;
                m_smart = true;
                m_depth = 0;
                RefreshSuggestion(point);
                RequestRender();
                return;
            }
        }

        Settle();
        RequestRender();
    }

    void Overlay::OnWheel(POINT point, int delta)
    {
        const RECT bounds = ClientBounds();
        if (delta == 0 || !PtInRect(&bounds, point))
        {
            return;
        }

        if (m_mode == OperationMode::TextRecognition)
        {
            const HitTarget hit = HitTest(point);
            if (hit == HitTarget::OcrText || hit == HitTarget::OcrScrollbar)
            {
                m_ocrPanel.OnWheel(m_selection, delta, m_device.dpi(), kHudTextSizeDip,
                                   m_device.dwriteFactory(), m_device.textFormat());
                RequestRender();
            }
            return;
        }

        if (!m_smart)
        {
            if (!m_hasSelection || m_dragMode != DragMode::None
                || (m_toolbarVisible
                    && HitTestToolbarTarget(m_toolbar, point) != ToolbarHitTarget::None))
            {
                return;
            }

            RECT adjusted = m_selection;
            if (delta > 0)
            {
                adjusted.left = std::max(bounds.left, adjusted.left - 1);
                adjusted.top = std::max(bounds.top, adjusted.top - 1);
                adjusted.right = std::min(bounds.right, adjusted.right + 1);
                adjusted.bottom = std::min(bounds.bottom, adjusted.bottom + 1);
            }
            else
            {
                if (adjusted.right - adjusted.left <= 2
                    || adjusted.bottom - adjusted.top <= 2)
                {
                    return;
                }
                ++adjusted.left;
                ++adjusted.top;
                --adjusted.right;
                --adjusted.bottom;
            }

            if (EqualRect(&adjusted, &m_selection))
            {
                return;
            }
            m_selection = adjusted;
            m_displaySelection = adjusted;
            Settle();
            RequestRender();
            return;
        }

        const int previousDepth = m_depth;
        m_depth += delta < 0 ? 1 : -1;
        m_depth = std::clamp(m_depth, -8, 0);

        const POINT screen{ point.x + m_bounds.left, point.y + m_bounds.top };
        if (m_picker.Ladder(screen, m_ladder, 6) < 0)
        {
            m_depth = previousDepth;
            return;
        }
        m_lastQuery = GetTickCount64();
        ApplyLadderLevel();
        RequestRender();
    }

    void Overlay::Invoke(ToolbarButton button)
    {
        if (button == ToolbarButton::Cancel)
        {
            Cancel();
            return;
        }
        if (button == ToolbarButton::EditSelection)
        {
            m_mode = OperationMode::EditSelection;
            m_ocrPanel.CancelInteractions(m_window);
            SetFocus(m_window);
            RequestRender();
            return;
        }
        if (button == ToolbarButton::TextRecognition)
        {
            EnterTextRecognitionMode();
            return;
        }
        if (button != ToolbarButton::Copy)
        {
            return;
        }
        if (m_mode == OperationMode::TextRecognition)
        {
            CopyOcrText(true);
            return;
        }

        const RECT region{ m_selection.left + m_bounds.left,
                           m_selection.top + m_bounds.top,
                           m_selection.right + m_bounds.left,
                           m_selection.bottom + m_bounds.top };

        try
        {
            m_image.CopyToClipboard(m_window, region);
        }
        catch (...)
        {
            ReportCurrentException(__FILEW__, __LINE__);
        }

        Cancel();
    }

    void Overlay::EnterTextRecognitionMode()
    {
        if (!m_hasSelection)
        {
            return;
        }

        m_ocrPanel.Enter(m_window, m_ocrRuntime, m_image, m_bounds, m_selection);
        m_mode = OperationMode::TextRecognition;
        m_smart = false;
        SetFocus(m_window);
        RequestRender();
    }

    void Overlay::CopyOcrText(bool closeAfterCopy)
    {
        m_ocrPanel.CopyText(m_window);

        if (closeAfterCopy)
        {
            Cancel();
        }
    }

    void Overlay::TrimCpuCaches() noexcept
    {
        m_image.Clear();
        m_picker.Trim();
        std::vector<RECT>{}.swap(m_ladder);
        m_ocrPanel.CancelSession(m_window);
        if (m_ocrRuntime != nullptr)
        {
            m_ocrRuntime->Trim();
        }
    }

    void Overlay::TrimGpuCaches() noexcept
    {
        m_ocrPanel.InvalidateLayout();
        m_ocrPanel.InvalidateSpinner();
        m_renderer.InvalidateLayouts();
        m_device.TrimGpuCaches();
    }

    void Overlay::ReconcileMemoryState() noexcept
    {
        if (m_window == nullptr || IsWindowVisible(m_window) || m_beginning)
        {
            return;
        }

        if (m_memoryWatch.IsLow())
        {
            m_memoryWatch.Acknowledge();
            TrimCpuCaches();
            TrimGpuCaches();
            return;
        }

        const bool hasWarmCpuPayload = !m_image.empty()
            || (m_ocrRuntime != nullptr && m_ocrRuntime->hasLoadedEngine());
        if (hasWarmCpuPayload)
        {
            m_memoryWatch.Arm();
        }

        if ((m_device.hasBackground() || !m_device.surfaceTrimmed())
            && m_device.IsVideoMemoryTight())
        {
            TrimGpuCaches();
        }
    }

    void Overlay::OnMemoryPressure() noexcept
    {
        m_memoryWatch.Acknowledge();
        ReconcileMemoryState();
    }

    void Overlay::OnVramPressure() noexcept
    {
        ReconcileMemoryState();
    }

    void Overlay::Render()
    {
        if (m_width == 0 || m_height == 0)
        {
            return;
        }

        if (!m_device.hasDeviceContext() || !m_device.hasSurface())
        {
            const HRESULT recoverHr = RecoverDeviceResources();
            if (FAILED(recoverHr) || !m_device.hasSurface())
            {
                GiveUpRendering(recoverHr, L"Overlay::RecoverDeviceResources");
                return;
            }
        }

        HRESULT hr = RenderFrame();
        if (RenderDevice::IsDeviceLost(hr))
        {
            const HRESULT recoverHr = RecoverDeviceResources();
            if (FAILED(recoverHr))
            {
                GiveUpRendering(recoverHr, L"Overlay::RecoverDeviceResources");
                return;
            }
            if (!m_device.hasSurface())
            {
                GiveUpRendering(hr, L"Overlay::RenderFrame");
                return;
            }
            hr = RenderFrame();
        }

        if (RenderDevice::IsDeviceLost(hr))
        {
            GiveUpRendering(hr, L"Overlay::RenderFrame");
            return;
        }
        AGSHOT_CHECK_HR(hr);

        m_stats.RecordPresent();
    }

    void Overlay::GiveUpRendering(HRESULT hr, const wchar_t* operation) noexcept
    {
        if (SUCCEEDED(hr))
        {
            hr = E_UNEXPECTED;
        }

        if (IsWindowVisible(m_window))
        {
            Cancel();
        }

        if (m_failed)
        {
            return;
        }
        m_failed = true;

        try
        {
            Failure failure = DescribeHresult(operation, hr, __FILEW__, __LINE__);
            if (!failure.what.empty())
            {
                failure.detail = failure.what + L"\n" + failure.detail;
            }
            ShowWarning(std::move(failure),
                        L"AGShot could not rebuild its graphics device, so the capture overlay was closed.");
        }
        catch (...)
        {
        }
    }

    HRESULT Overlay::RenderFrame()
    {
        const bool collectStats = m_stats.enabled();
        LARGE_INTEGER renderStart{};
        if (collectStats)
        {
            QueryPerformanceCounter(&renderStart);
        }

        HRESULT hr = m_device.BeginFrameDraw(m_width, m_height);
        if (FAILED(hr))
        {
            return hr;
        }
        auto abortFrame = wil::scope_exit([&] { m_device.AbortFrameDraw(); });

        ID2D1DeviceContext* context = m_device.d2dContext();
        if (m_device.hasBackground())
        {
            context->DrawBitmap(
                m_device.background(),
                D2D1::RectF(0.0f, 0.0f,
                            static_cast<float>(m_width), static_cast<float>(m_height)),
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }
        else
        {
            context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        }

        const RECT activeSelection = m_smart ? m_displaySelection : m_selection;
        m_renderer.DrawDim(context, m_width, m_height, m_hasSelection, activeSelection,
                           m_device.brushes());
        if (m_hasSelection)
        {
            if (m_mode == OperationMode::TextRecognition)
            {
                m_ocrPanel.Draw(context, m_device.dwriteFactory(), m_device.textFormat(),
                                m_selection, m_device.dpi(), m_device.brushes());
            }
            else
            {
                m_renderer.DrawSelection(m_device, m_width, m_height,
                                         activeSelection, m_smart, m_handleSize);
            }
        }
        if (m_toolbarVisible)
        {
            m_renderer.DrawToolbar(m_device, m_toolbar, m_mode == OperationMode::TextRecognition);
            m_renderer.DrawTooltip(m_device, m_width, m_height, m_toolbar);
        }
        const bool pointerOverToolbar = m_toolbarVisible
            && HitTestToolbarTarget(m_toolbar, m_pointer) != ToolbarHitTarget::None;
        if (m_mode == OperationMode::EditSelection && !pointerOverToolbar
            && (!m_toolbarVisible || (m_hasSelection && PtInRect(&m_selection, m_pointer))))
        {
            const bool translucent =
                m_dragMode == DragMode::NewRectangle || m_dragMode == DragMode::MoveOrResize;
            m_renderer.DrawPixelMagnifier(
                m_device, m_width, m_height, m_image, m_bounds,
                m_selection, m_pointer, m_dragMode == DragMode::MoveOrResize,
                m_dragPart, translucent);
        }
        m_stats.Draw(m_device, m_width);

        abortFrame.release();
        const HRESULT endHr = m_device.EndFrameDraw();
        if (collectStats && SUCCEEDED(endHr))
        {
            // Draw() must run before EndDraw()/Commit(), so record the full
            // frame draw-and-commit duration here for display on the next frame.
            m_stats.RecordFrameDuration(renderStart.QuadPart);
        }
        return endHr;
    }

#ifdef AGSHOT_TOOLBAR_PROOF
    bool Overlay::ExportToolbarProof(const wchar_t* path, int dpi, bool textRecognitionActive,
                                     float ocrScrollProbe, bool ocrSpinnerProbe)
    {
        return m_renderer.ExportToolbarProof(
            m_device, m_ocrPanel, path, dpi, textRecognitionActive,
            ocrScrollProbe, ocrSpinnerProbe);
    }
#endif

    LRESULT Overlay::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case kOcrResultMessage:
            if (m_ocrPanel.HandleResultMessage(lparam) && m_mode == OperationMode::TextRecognition)
            {
                RequestRender();
            }
            return 0;

        case WM_DPICHANGED:
        {
            int dpi = HIWORD(wparam);
            if (dpi <= 0)
            {
                dpi = 96;
            }
            m_handleSize = MulDiv(kHandleSize, dpi, 96);
            m_ocrPanel.InvalidateSpinner();
            m_ocrPanel.InvalidateLayout();
            m_renderer.InvalidateLayouts();
            m_device.OnDpiChanged(dpi);
            if (m_toolbarVisible)
            {
                Settle();
            }
            RequestRender();
            return 0;
        }

        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED)
            {
                m_width = static_cast<UINT>(LOWORD(lparam));
                m_height = static_cast<UINT>(HIWORD(lparam));
                if (m_width > 0 && m_height > 0 && m_window != nullptr)
                {
                    if (m_device.hasDeviceContext() && m_device.hasComposition())
                    {
                        const HRESULT hr = m_device.ResizeSurface(m_width, m_height);
                        if (RenderDevice::IsDeviceLost(hr))
                        {
                            if (const HRESULT recoverHr = RecoverDeviceResources(); FAILED(recoverHr))
                            {
                                AGSHOT_CHECK_HR(recoverHr);
                            }
                        }
                        else
                        {
                            AGSHOT_CHECK_HR(hr);
                        }
                    }
                    RequestRender();
                }
            }
            return 0;

        case WM_TIMER:
            if (wparam == kOcrSelectionTimer
                && m_ocrPanel.OnSelectionTimer(m_selection, m_device.dpi(),
                                               m_device.dwriteFactory(), m_device.textFormat()))
            {
                RequestRender();
                return 0;
            }
            break;

        case kFrameMessage:
        {
            m_clock.BeginFrame();
            auto endClock = wil::scope_exit([&] { m_clock.EndFrame(); });

            if (!IsWindowVisible(m_window) || !m_device.hasBackground())
            {
                return 0;
            }

            if (m_animatingSelection)
            {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                AdvanceSelectionAnimation(now.QuadPart);
            }
            Render();
            endClock.reset();

            if (m_animatingSelection
                || (m_mode == OperationMode::TextRecognition && m_ocrPanel.pending()))
            {
                m_clock.RequestFrame(m_window, kFrameMessage);
            }
            return 0;
        }

        case kMemoryPressureMessage:
            OnMemoryPressure();
            return 0;

        case kVramPressureMessage:
            OnVramPressure();
            return 0;

        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_GETOBJECT:
            return 0;

        case WM_POINTERUPDATE:
        case WM_POINTERDOWN:
        case WM_POINTERUP:
        {
            POINTER_INFO info{};
            POINT point{};
            if (!PointerEvent(wparam, info, point))
            {
                break;
            }

            switch (info.ButtonChangeType)
            {
            case POINTER_CHANGE_FIRSTBUTTON_DOWN:
                OnLeftButtonDown(point);
                return 0;
            case POINTER_CHANGE_FIRSTBUTTON_UP:
                OnLeftButtonUp(point);
                return 0;
            case POINTER_CHANGE_SECONDBUTTON_UP:
                ShowOcrContextMenu(point);
                return 0;
            default:
                break;
            }

            OnMouseMove(point);
            return 0;
        }

        case WM_POINTERENTER:
        {
            POINTER_INFO info{};
            POINT point{};
            if (!PointerEvent(wparam, info, point))
            {
                break;
            }
            OnMouseMove(point);
            return 0;
        }

        case WM_POINTERLEAVE:
            m_toolbar.hovered = ToolbarButton::None;
            RequestRender();
            return 0;

        case WM_POINTERWHEEL:
        {
            POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(hwnd, &point);
            OnWheel(point, GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        }

        case WM_POINTERHWHEEL:
            return 0;

        case WM_POINTERCAPTURECHANGED:
            if (m_dragMode != DragMode::None)
            {
                m_dragMode = DragMode::None;
                m_dragPart = SelectionPart::None;
                Settle();
            }
            m_ocrPanel.CancelInteractions(m_window);
            RequestRender();
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
            ShowOcrContextMenu(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) });
            return 0;

        case WM_MOUSELEAVE:
            m_toolbar.hovered = ToolbarButton::None;
            RequestRender();
            return 0;

        case WM_MOUSEWHEEL:
        {
            POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(hwnd, &point);
            OnWheel(point, GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lparam) != HTCLIENT)
            {
                break;
            }
            ApplyCursor();
            return TRUE;

        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE)
            {
                Cancel();
                return 0;
            }
            if (m_mode == OperationMode::TextRecognition)
            {
                bool shouldClose = false;
                bool needsRender = false;
                if (m_ocrPanel.OnKeyDown(m_window, m_selection, wparam, m_device.dpi(),
                                         m_device.dwriteFactory(), m_device.textFormat(),
                                         shouldClose, needsRender))
                {
                    if (shouldClose)
                    {
                        Cancel();
                    }
                    else if (needsRender)
                    {
                        RequestRender();
                    }
                    return 0;
                }
                break;
            }
            if (wparam == VK_RETURN && !m_smart && m_hasSelection)
            {
                Invoke(ToolbarButton::Copy);
                return 0;
            }
            if (wparam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)
                && !m_smart && m_hasSelection)
            {
                Invoke(ToolbarButton::Copy);
                return 0;
            }
            if (wparam == VK_LEFT || wparam == VK_UP
                || wparam == VK_RIGHT || wparam == VK_DOWN)
            {
                POINT cursor{};
                if (GetCursorPos(&cursor))
                {
                    switch (wparam)
                    {
                    case VK_LEFT:  --cursor.x; break;
                    case VK_UP:    --cursor.y; break;
                    case VK_RIGHT: ++cursor.x; break;
                    case VK_DOWN:  ++cursor.y; break;
                    }
                    SetCursorPos(cursor.x, cursor.y);
                }
                return 0;
            }
            break;

        case WM_CHAR:
            if (m_mode == OperationMode::TextRecognition)
            {
                return 0;
            }
            break;

        case WM_CANCELMODE:
            m_ocrPanel.StopSelection(m_window);
            return 0;

        case WM_NCDESTROY:
        {
            using CompletionMessage = std::shared_ptr<OcrCompletion>;
            MSG pending{};
            while (PeekMessageW(&pending, hwnd, kOcrResultMessage,
                                kOcrResultMessage, PM_REMOVE))
            {
                delete reinterpret_cast<CompletionMessage*>(pending.lParam);
            }
            break;
        }

        case WM_DESTROY:
            m_ocrPanel.StopSelection(m_window);
            return 0;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
