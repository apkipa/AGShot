#include "pch.h"

#include "hud/overlay.h"

#include "core/error.h"
#include "hud/color_model.h"

#include <imm.h>

namespace agshot
{
    namespace
    {
        constexpr wchar_t kOverlayClass[] = L"AGShot.Overlay";
        constexpr wchar_t kOverlayTitle[] = L"AGShot";

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

    Overlay::Overlay()
    {
        m_eyedropperLayer.owner = this;
        m_colorPickerLayer.owner = this;
        m_toolbarController.Bind(this, &m_annotController, &m_device, &m_toolMode);
        m_textEditorLayer.owner = this;
        m_ocrLayer.owner = this;
        m_canvasLayer.owner = this;

        // Single Z-Order registry (front-to-back) driving hit-test, hover,
        // light-dismiss, pointer/keyboard/wheel dispatch, cursor, and IME.
        m_layerStack.Register(&m_eyedropperLayer);
        m_layerStack.Register(&m_colorPickerLayer);
        m_layerStack.Register(&m_toolbarController);
        m_layerStack.Register(&m_textEditorLayer);
        m_layerStack.Register(&m_ocrLayer);
        m_layerStack.Register(&m_canvasLayer);
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
        m_layerStack.ResetTransientState();
        m_annotController.ResetSession(m_window);
        m_annotController.DiscardDeviceResources();
        m_colorPicker.DiscardDeviceResources();
        m_toolbarController.InvalidateLayouts();
        m_ocrPanel.Destroy(m_window);
        m_renderer.DiscardIndependentResources();
        m_memoryWatch.Destroy();
        m_device.Destroy();
        if (m_window != nullptr)
        {
            ImmAssociateContextEx(m_window, nullptr, IACE_DEFAULT);
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
        m_annotController.DiscardDeviceResources();
        m_colorPicker.DiscardDeviceResources();
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

    ui::LayoutEnv Overlay::CurrentLayoutEnv() const noexcept
    {
        return ui::LayoutEnv{ ClientBounds(), m_device.dpi() };
    }

    int Overlay::EffectiveHandleSize(ui::PointerKind kind) const noexcept
    {
        if (kind == ui::PointerKind::Touch)
        {
            return std::max(m_handleSize, MulDiv(18, m_device.dpi(), 96));
        }
        return m_handleSize;
    }

    void Overlay::Begin()
    {
        if (m_window == nullptr || m_beginning || IsWindowVisible(m_window))
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
        m_toolMode = ToolbarButton::EditSelection;
        m_annotController.ResetSession(m_window);
        m_colorPicker.Close();
        m_toolbarController.Reset();
        m_pickerAnchorAction = SubToolbarAction::None;
        m_eyedropperActive = false;
        m_ocrPanel.ResetSession();
        m_depth = 0;
        m_canvasDrag = CanvasDrag::None;
        m_dragPart = SelectionPart::None;
        m_lastClickMs = 0;
        m_pendingConfirm = false;
        m_reselectPending = false;
        m_layerStack.ResetTransientState();
        m_canvasWheelAccum.Reset();
        SyncImeState();

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

        m_annotController.ResetSession(m_window);
        m_colorPicker.Close();
        m_toolbarController.Reset();
        m_pickerAnchorAction = SubToolbarAction::None;
        m_eyedropperActive = false;

        m_hasSelection = false;
        m_smart = true;
        m_toolMode = ToolbarButton::EditSelection;
        m_depth = 0;
        m_canvasDrag = CanvasDrag::None;
        m_dragPart = SelectionPart::None;
        m_pendingConfirm = false;
        m_reselectPending = false;
        m_layerStack.ResetTransientState();
        m_canvasWheelAccum.Reset();
        SyncImeState();

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

    void Overlay::RequestRender()
    {
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

    void Overlay::ApplyCursor()
    {
        if (m_window == nullptr || !IsWindowVisible(m_window))
        {
            return;
        }
        SetCursor(LoadCursorW(nullptr, m_layerStack.ResolveCursor(m_pointer)));
    }

    void Overlay::SyncImeState() noexcept
    {
        if (m_window == nullptr)
        {
            return;
        }
        ImmAssociateContextEx(m_window, nullptr, m_layerStack.ResolveWantIme() ? IACE_DEFAULT : 0);
    }

    void Overlay::OnToolbarMoved()
    {
        if (m_colorPicker.isOpen())
        {
            m_colorPicker.Arrange(
                ui::AnchorSpec{
                    m_toolbarController.PickerAnchorRect(m_pickerAnchorAction),
                    m_toolbarController.PickerAvoidBand(),
                    7 },
                CurrentLayoutEnv());
        }
    }

    ToolbarState Overlay::CurrentToolbarState() const noexcept
    {
        ToolbarState state{};
        state.activeMode = m_toolMode;
        state.activePicker = m_colorPicker.isOpen() ? m_colorPicker.target() : ColorPickerTarget::None;
        state.mosaicEffectMenuOpen = m_toolbarController.isMosaicMenuOpen();
        m_annotController.PopulateToolbarState(state, m_toolMode);
        return state;
    }

    void Overlay::RunLayout()
    {
        const ui::LayoutEnv env = CurrentLayoutEnv();
        m_toolbarController.LayoutSub(CurrentToolbarState(), env);
        if (m_colorPicker.isOpen())
        {
            m_colorPicker.Arrange(
                ui::AnchorSpec{
                    m_toolbarController.PickerAnchorRect(m_pickerAnchorAction),
                    m_toolbarController.PickerAvoidBand(),
                    7 },
                env);
        }
        if (!m_layerStack.hasCapture()
            && !m_colorPicker.HitTestPanel(m_pointer)
            && !m_toolbarController.isMosaicMenuOpen())
        {
            m_toolbarController.UpdateHover(m_pointer);
        }
    }

    void Overlay::FlushInvalidation(ui::Invalidate inv)
    {
        if (inv == ui::Invalidate::None)
        {
            return;
        }
        if (ui::HasFlag(inv, ui::Invalidate::Layout))
        {
            RunLayout();
            inv |= ui::Invalidate::Paint;
        }
        if (ui::HasFlag(inv, ui::Invalidate::Ime))
        {
            SyncImeState();
        }
        if (ui::HasFlag(inv, ui::Invalidate::Paint))
        {
            RequestRender();
        }
        else if (ui::HasFlag(inv, ui::Invalidate::Cursor))
        {
            ApplyCursor();
        }
    }

    void Overlay::DismissColorPicker(bool commitEdit, ui::Invalidate& inv)
    {
        if (!m_colorPicker.isOpen())
        {
            return;
        }
        if (commitEdit)
        {
            bool commitChanged = false;
            m_colorPicker.CommitTextEdit(commitChanged);
            if (commitChanged)
            {
                ApplyColorFromPicker(true);
            }
        }
        m_colorPicker.Close();
        m_pickerAnchorAction = SubToolbarAction::None;
        m_annotController.ClearLiveEditSnapshot();
        inv |= ui::Invalidate::Layout | ui::Invalidate::Ime | ui::Invalidate::Paint;
    }

    void Overlay::Settle()
    {
        m_colorPicker.Close();
        m_pickerAnchorAction = SubToolbarAction::None;
        m_annotController.ClearLiveEditSnapshot();
        m_toolbarController.Settle(CurrentToolbarState(), m_selection, CurrentLayoutEnv());
        m_toolbarController.UpdateHover(m_pointer);
    }

    void Overlay::ToggleColorPicker(
        ColorPickerTarget target, SubToolbarAction anchorAction, ui::Invalidate& inv)
    {
        if (m_colorPicker.isOpen() && m_colorPicker.target() == target)
        {
            DismissColorPicker(true, inv);
            return;
        }

        m_pickerAnchorAction = anchorAction;
        const RECT anchor = m_toolbarController.PickerAnchorRect(anchorAction);
        const ColorPickerOpenInfo info = m_annotController.ResolvePickerOpenInfo(target, m_toolMode);
        m_annotController.ClearLiveEditSnapshot();
        m_colorPicker.Open(
            target, info.allowNone, info.showStrokeStyles, info.strokeStyle,
            info.initialColor, anchor, ClientBounds(), m_device.dpi(),
            m_toolbarController.PickerAvoidBand(), info.restoreAlpha);
        inv |= ui::Invalidate::Layout | ui::Invalidate::Ime | ui::Invalidate::Paint;
    }

    void Overlay::ApplyColorFromPicker(bool commitUndo)
    {
        m_annotController.ApplyColorFromPicker(m_colorPicker, m_toolMode, commitUndo);
    }

    void Overlay::Invoke(ToolbarButton button, ui::Invalidate& inv)
    {
        if (button == ToolbarButton::Cancel)
        {
            Cancel();
            return;
        }

        DismissColorPicker(false, inv);
        m_toolbarController.CloseMosaicEffectMenu();
        m_annotController.CommitTextEditor(m_window);
        inv |= ui::Invalidate::Ime;

        if (button == ToolbarButton::Undo)
        {
            if (m_annotController.document().Undo())
            {
                inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            }
            return;
        }
        if (button == ToolbarButton::Redo)
        {
            if (m_annotController.document().Redo())
            {
                inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            }
            return;
        }
        if (button == ToolbarButton::EditSelection)
        {
            m_toolMode = ToolbarButton::EditSelection;
            m_annotController.document().ClearSelection();
            m_ocrPanel.CancelInteractions(m_window);
            SetFocus(m_window);
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            return;
        }
        if (button == ToolbarButton::SelectObject)
        {
            m_toolMode = ToolbarButton::SelectObject;
            m_ocrPanel.CancelInteractions(m_window);
            SetFocus(m_window);
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            return;
        }
        if (IsAnnotationToolButton(button))
        {
            m_toolMode = button;
            m_annotController.document().ClearSelection();
            m_ocrPanel.CancelInteractions(m_window);
            SetFocus(m_window);
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            return;
        }
        if (button == ToolbarButton::TextRecognition)
        {
            EnterTextRecognitionMode(inv);
            return;
        }
        if (button != ToolbarButton::Copy)
        {
            return;
        }
        if (m_toolMode == ToolbarButton::TextRecognition)
        {
            CopyOcrText(true);
            return;
        }

        try
        {
            m_annotController.ExportToClipboard(
                m_window, m_device, m_image, m_bounds, m_selection);
        }
        catch (...)
        {
            ReportCurrentException(__FILEW__, __LINE__);
        }

        Cancel();
    }

    void Overlay::InvokeSubAction(SubToolbarAction action, ui::Invalidate& inv)
    {
        if (action == SubToolbarAction::StrokePicker)
        {
            ToggleColorPicker(ColorPickerTarget::Stroke, action, inv);
            return;
        }
        if (action == SubToolbarAction::FillPicker)
        {
            ToggleColorPicker(ColorPickerTarget::Fill, action, inv);
            return;
        }

        DismissColorPicker(false, inv);
        if (m_annotController.InvokeSubAction(
            action, m_toolMode, m_device.dwriteFactory(), m_device.dpi()))
        {
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
        }
    }

    void Overlay::EnterTextRecognitionMode(ui::Invalidate& inv)
    {
        if (!m_hasSelection)
        {
            return;
        }

        m_annotController.CommitTextEditor(m_window);
        m_annotController.document().ClearSelection();
        m_ocrPanel.Enter(m_window, m_ocrRuntime, m_image, m_bounds, m_selection);
        m_toolMode = ToolbarButton::TextRecognition;
        m_smart = false;
        SetFocus(m_window);
        inv |= ui::Invalidate::Layout | ui::Invalidate::Ime | ui::Invalidate::Paint;
    }

    void Overlay::CopyOcrText(bool closeAfterCopy)
    {
        m_ocrPanel.CopyText(m_window);
        if (closeAfterCopy)
        {
            Cancel();
        }
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
        if (info.pointerType == PT_MOUSE || info.pointerType == PT_PEN)
        {
            return true;
        }
        return info.pointerType == PT_TOUCH && IS_POINTER_PRIMARY_WPARAM(wparam);
    }

    // -------------------------------------------------------------------------
    // Layer 0: Eyedropper (modal desktop pixel sampling)
    // -------------------------------------------------------------------------
    bool Overlay::EyedropperLayer::IsVisible() const noexcept
    {
        return owner->m_eyedropperActive;
    }

    bool Overlay::EyedropperLayer::HitTest(POINT) const
    {
        return true;
    }

    void Overlay::EyedropperLayer::OnHoverMove(POINT, ui::Invalidate& inv)
    {
        owner->m_toolbarController.ClearHover();
        inv |= ui::Invalidate::Paint;
    }

    ui::EventOutcome Overlay::EyedropperLayer::OnLeftDown(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        owner->m_eyedropperActive = false;
        owner->m_colorPicker.ResumeFromEyedropper();
        if (!owner->m_image.empty() && owner->m_image.pixels() != nullptr)
        {
            const int px = std::clamp(static_cast<int>(in.point.x), 0, owner->m_image.width() - 1);
            const int py = std::clamp(static_cast<int>(in.point.y), 0, owner->m_image.height() - 1);
            const BYTE* row = static_cast<const BYTE*>(owner->m_image.pixels())
                + static_cast<std::size_t>(py) * owner->m_image.stride()
                + static_cast<std::size_t>(px) * 4;
            const D2D1_COLOR_F picked = D2D1::ColorF(
                row[2] / 255.0f, row[1] / 255.0f, row[0] / 255.0f, 1.0f);
            owner->m_colorPicker.SetPickedPixelColor(picked, owner->m_device.dpi());
            owner->ApplyColorFromPicker(true);
            inv |= ui::Invalidate::Layout;
        }
        inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome Overlay::EyedropperLayer::OnRightUp(const ui::PointerInput&, ui::Invalidate& inv)
    {
        owner->m_eyedropperActive = false;
        owner->m_colorPicker.ResumeFromEyedropper();
        owner->m_annotController.ClearLiveEditSnapshot();
        inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome Overlay::EyedropperLayer::OnKeyDown(
        WPARAM key, KeyModifiers, ui::Invalidate& inv)
    {
        if (key == VK_ESCAPE)
        {
            owner->m_eyedropperActive = false;
            owner->m_colorPicker.ResumeFromEyedropper();
            owner->m_annotController.ClearLiveEditSnapshot();
            inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;
        }
        return ui::EventOutcome::Handled;
    }

    std::optional<LPCWSTR> Overlay::EyedropperLayer::CursorForPoint(POINT, bool) const
    {
        return IDC_CROSS;
    }

    // -------------------------------------------------------------------------
    // Layer 1: ColorPickerPopover
    // -------------------------------------------------------------------------
    bool Overlay::ColorPickerLayer::IsVisible() const noexcept
    {
        return owner->m_colorPicker.visible();
    }

    bool Overlay::ColorPickerLayer::HitTest(POINT point) const
    {
        return owner->m_colorPicker.HitTestPanel(point);
    }

    void Overlay::ColorPickerLayer::OnHoverMove(POINT point, ui::Invalidate& inv)
    {
        if (owner->m_colorPicker.UpdateHover(point))
        {
            inv |= ui::Invalidate::Paint;
        }
        inv |= ui::Invalidate::Cursor;
    }

    void Overlay::ColorPickerLayer::OnHoverLeave(ui::Invalidate& inv)
    {
        if (owner->m_colorPicker.visible())
        {
            owner->m_colorPicker.ClearHover();
            inv |= ui::Invalidate::Paint;
        }
    }

    ui::OutsideClickPolicy Overlay::ColorPickerLayer::OnOutsidePointer(
        ui::PointerButton button, POINT point, const ui::IHudLayer* hitBelow,
        ui::Invalidate& inv)
    {
        if (button == ui::PointerButton::Left && hitBelow == &owner->m_toolbarController)
        {
            const ToolbarHit th = HitTestToolbarTarget(owner->m_toolbarController.toolbar(), point);
            const bool keepPicker =
                (th.target == ToolbarHitTarget::SubButton && th.subAction == owner->m_pickerAnchorAction)
                || th.target == ToolbarHitTarget::Handle;
            if (keepPicker)
            {
                return ui::OutsideClickPolicy::Ignore;
            }
            owner->DismissColorPicker(true, inv);
            return ui::OutsideClickPolicy::DismissAndPass;
        }

        owner->DismissColorPicker(true, inv);
        return ui::OutsideClickPolicy::DismissAndAbsorb;
    }

    ui::EventOutcome Overlay::ColorPickerLayer::OnLeftDown(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        bool reqEyedropper = false;
        bool pickerChanged = false;
        if (!owner->m_colorPicker.OnLeftButtonDown(
            in.point, owner->m_device.dwriteFactory(), owner->m_device.textFormat(),
            owner->m_device.dpi(), reqEyedropper, pickerChanged, in.mods))
        {
            return ui::EventOutcome::Ignored;
        }

        if (reqEyedropper)
        {
            owner->m_colorPicker.SuspendForEyedropper();
            owner->m_eyedropperActive = true;
            owner->m_layerStack.ClearHover(inv);
            inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;
        }

        if (owner->m_colorPicker.dragging())
        {
            if (pickerChanged)
            {
                owner->ApplyColorFromPicker(false);
                inv |= ui::Invalidate::Layout;
            }
            inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
            return ui::EventOutcome::Capture;
        }

        if (pickerChanged)
        {
            owner->ApplyColorFromPicker(true);
            inv |= ui::Invalidate::Layout;
        }
        if (!owner->m_colorPicker.hasTextFocus())
        {
            owner->m_annotController.ClearLiveEditSnapshot();
        }
        inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
        return ui::EventOutcome::Handled;
    }

    void Overlay::ColorPickerLayer::OnCapturedMove(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        bool pickerChanged = false;
        if (owner->m_colorPicker.OnMouseMove(
            in.point, owner->m_device.dwriteFactory(), owner->m_device.textFormat(),
            owner->m_device.dpi(), pickerChanged)
            && pickerChanged)
        {
            owner->ApplyColorFromPicker(false);
            inv |= ui::Invalidate::Layout;
        }
        inv |= ui::Invalidate::Paint;
    }

    void Overlay::ColorPickerLayer::OnCapturedUp(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        bool pickerCommit = false;
        if (owner->m_colorPicker.OnLeftButtonUp(in.point, pickerCommit) && pickerCommit)
        {
            owner->ApplyColorFromPicker(true);
            inv |= ui::Invalidate::Layout;
        }
        if (!owner->m_colorPicker.hasTextFocus())
        {
            owner->m_annotController.ClearLiveEditSnapshot();
        }
        inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
    }

    void Overlay::ColorPickerLayer::OnCancelCapture(ui::Invalidate& inv)
    {
        owner->m_colorPicker.EndDrag();
        if (!owner->m_colorPicker.hasTextFocus() && owner->m_annotController.CommitLiveEdit())
        {
            inv |= ui::Invalidate::Layout;
        }
        inv |= ui::Invalidate::Paint;
    }

    ui::EventOutcome Overlay::ColorPickerLayer::OnRightUp(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        bool changed = false;
        bool commitUndo = false;
        owner->m_colorPicker.OnRightButtonUp(
            owner->m_window, in.point, owner->m_device.dwriteFactory(),
            owner->m_device.textFormat(), owner->m_device.dpi(), changed, commitUndo);
        if (changed)
        {
            owner->ApplyColorFromPicker(commitUndo);
            inv |= ui::Invalidate::Layout;
        }
        if (!owner->m_colorPicker.hasTextFocus())
        {
            owner->m_annotController.ClearLiveEditSnapshot();
        }
        inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome Overlay::ColorPickerLayer::OnWheel(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        bool changed = false;
        if (owner->m_colorPicker.OnWheel(in.point, in.wheelDelta, changed) && changed)
        {
            owner->ApplyColorFromPicker(true);
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
        }
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome Overlay::ColorPickerLayer::OnKeyDown(
        WPARAM key, KeyModifiers mods, ui::Invalidate& inv)
    {
        if (owner->m_colorPicker.hasTextFocus() || owner->m_colorPicker.isFormatMenuOpen())
        {
            bool changed = false;
            bool commitUndo = false;
            if (owner->m_colorPicker.OnKeyDown(owner->m_window, key, changed, commitUndo, mods))
            {
                if (changed)
                {
                    owner->ApplyColorFromPicker(commitUndo);
                    inv |= ui::Invalidate::Layout;
                }
                if (!owner->m_colorPicker.hasTextFocus())
                {
                    owner->m_annotController.ClearLiveEditSnapshot();
                    inv |= ui::Invalidate::Ime;
                }
                inv |= ui::Invalidate::Paint;
                return ui::EventOutcome::Handled;
            }
        }
        if (key == VK_ESCAPE)
        {
            owner->DismissColorPicker(false, inv);
            return ui::EventOutcome::Handled;
        }
        return ui::EventOutcome::Ignored;
    }

    ui::EventOutcome Overlay::ColorPickerLayer::OnChar(wchar_t ch, ui::Invalidate& inv)
    {
        if (!owner->m_colorPicker.hasTextFocus())
        {
            return ui::EventOutcome::Ignored;
        }
        bool changed = false;
        if (owner->m_colorPicker.OnChar(ch, changed))
        {
            if (changed)
            {
                owner->ApplyColorFromPicker(false);
                inv |= ui::Invalidate::Layout;
            }
            inv |= ui::Invalidate::Paint;
        }
        return ui::EventOutcome::Handled;
    }

    std::optional<LPCWSTR> Overlay::ColorPickerLayer::CursorForPoint(POINT point, bool) const
    {
        return owner->m_colorPicker.CursorForPoint(point);
    }

    ui::ImePolicy Overlay::ColorPickerLayer::QueryImePolicy() const noexcept
    {
        return owner->m_colorPicker.hasTextFocus() ? ui::ImePolicy::Disable : ui::ImePolicy::Pass;
    }

    // -------------------------------------------------------------------------
    // Layer 3: InlineTextEditor (below Toolbar, above Canvas)
    // -------------------------------------------------------------------------
    bool Overlay::TextEditorLayer::IsVisible() const noexcept
    {
        return owner->m_annotController.hasActiveTextEditor();
    }

    bool Overlay::TextEditorLayer::HitTest(POINT point) const
    {
        return owner->m_annotController.textEditor().HitTestInside(
            point, owner->m_device.dwriteFactory(), owner->m_device.dpi());
    }

    ui::OutsideClickPolicy Overlay::TextEditorLayer::OnOutsidePointer(
        ui::PointerButton, POINT, const ui::IHudLayer*, ui::Invalidate& inv)
    {
        owner->m_annotController.CommitTextEditor(owner->m_window);
        inv |= ui::Invalidate::Layout | ui::Invalidate::Ime | ui::Invalidate::Paint;
        return ui::OutsideClickPolicy::DismissAndAbsorb;
    }

    ui::EventOutcome Overlay::TextEditorLayer::OnLeftDown(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        bool syncTb = false;
        bool syncIme = false;
        if (!owner->m_annotController.HandleActiveTextEditorLeftDown(
            owner->m_window, in.point, owner->m_device.dwriteFactory(),
            owner->m_device.dpi(), syncTb, syncIme, in.mods))
        {
            return ui::EventOutcome::Ignored;
        }
        if (syncTb) inv |= ui::Invalidate::Layout;
        if (syncIme) inv |= ui::Invalidate::Ime;
        inv |= ui::Invalidate::Paint;
        return owner->m_annotController.isInteracting()
            ? ui::EventOutcome::Capture
            : ui::EventOutcome::Handled;
    }

    void Overlay::TextEditorLayer::OnCapturedMove(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        owner->m_annotController.OnMouseMove(
            owner->m_window, in.point, in.mods, owner->m_selection,
            owner->m_device.dwriteFactory(), owner->m_device.dpi());
        inv |= ui::Invalidate::Paint;
    }

    void Overlay::TextEditorLayer::OnCapturedUp(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        bool syncTb = false;
        owner->m_annotController.OnLeftButtonUp(
            in.point, owner->m_selection, owner->m_device.dwriteFactory(),
            owner->m_device.dpi(), syncTb);
        if (syncTb) inv |= ui::Invalidate::Layout;
        inv |= ui::Invalidate::Paint;
    }

    void Overlay::TextEditorLayer::OnCancelCapture(ui::Invalidate& inv)
    {
        owner->m_annotController.ClearTransientDragState();
        inv |= ui::Invalidate::Paint;
    }

    ui::EventOutcome Overlay::TextEditorLayer::OnRightUp(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        bool syncTb = false;
        bool syncIme = false;
        if (owner->m_annotController.OnRightButtonUp(
            owner->m_window, in.point, owner->m_device.dwriteFactory(),
            owner->m_device.dpi(), syncTb, syncIme))
        {
            if (syncTb) inv |= ui::Invalidate::Layout;
            if (syncIme) inv |= ui::Invalidate::Ime;
            inv |= ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;
        }
        return ui::EventOutcome::Ignored;
    }

    ui::EventOutcome Overlay::TextEditorLayer::OnKeyDown(
        WPARAM key, KeyModifiers mods, ui::Invalidate& inv)
    {
        bool syncTb = false;
        bool syncIme = false;
        if (owner->m_annotController.OnKeyDown(
            owner->m_window, key, owner->m_toolMode,
            owner->m_device.dwriteFactory(), owner->m_device.dpi(),
            syncTb, syncIme, mods))
        {
            if (syncTb) inv |= ui::Invalidate::Layout;
            if (syncIme) inv |= ui::Invalidate::Ime;
            inv |= ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;
        }
        return ui::EventOutcome::Ignored;
    }

    ui::EventOutcome Overlay::TextEditorLayer::OnChar(wchar_t ch, ui::Invalidate& inv)
    {
        if (owner->m_annotController.OnChar(
            owner->m_window, ch, owner->m_device.dwriteFactory(), owner->m_device.dpi()))
        {
            inv |= ui::Invalidate::Paint;
        }
        return ui::EventOutcome::Handled;
    }

    std::optional<LPCWSTR> Overlay::TextEditorLayer::CursorForPoint(POINT point, bool) const
    {
        return owner->m_annotController.CursorForActiveEditor(
            point, owner->m_device.dwriteFactory(), owner->m_device.dpi());
    }

    // -------------------------------------------------------------------------
    // Layer 4: OCR Text Panel
    // -------------------------------------------------------------------------
    bool Overlay::OcrLayer::IsVisible() const noexcept
    {
        return owner->m_hasSelection && owner->m_toolMode == ToolbarButton::TextRecognition;
    }

    bool Overlay::OcrLayer::HitTest(POINT point) const
    {
        return PtInRect(&owner->m_selection, point) != 0;
    }

    ui::EventOutcome Overlay::OcrLayer::OnLeftDown(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        bool needsRender = false;
        owner->m_ocrPanel.OnLeftButtonDown(
            owner->m_window, owner->m_selection, in.point, owner->m_device.dpi(),
            owner->m_device.dwriteFactory(), owner->m_device.textFormat(),
            needsRender, in.mods);
        if (needsRender)
        {
            inv |= ui::Invalidate::Paint;
        }
        return owner->m_ocrPanel.capturingInput()
            ? ui::EventOutcome::Capture
            : ui::EventOutcome::Handled;
    }

    void Overlay::OcrLayer::OnCapturedMove(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        if (owner->m_ocrPanel.OnMouseMove(
            owner->m_selection, in.point, owner->m_device.dpi(),
            owner->m_device.dwriteFactory(), owner->m_device.textFormat()))
        {
            inv |= ui::Invalidate::Paint;
        }
        else
        {
            inv |= ui::Invalidate::Cursor;
        }
    }

    void Overlay::OcrLayer::OnCapturedUp(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        if (owner->m_ocrPanel.OnLeftButtonUp(
            owner->m_window, owner->m_selection, in.point, owner->m_device.dpi(),
            owner->m_device.dwriteFactory(), owner->m_device.textFormat()))
        {
            inv |= ui::Invalidate::Paint;
        }
    }

    void Overlay::OcrLayer::OnCancelCapture(ui::Invalidate& inv)
    {
        owner->m_ocrPanel.CancelInteractions(owner->m_window);
        inv |= ui::Invalidate::Paint;
    }

    ui::EventOutcome Overlay::OcrLayer::OnRightUp(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        const RECT track = owner->m_ocrPanel.ScrollTrack(owner->m_selection, owner->m_device.dpi());
        if (owner->m_ocrPanel.HasScrollbar(owner->m_selection, owner->m_device.dpi(),
                                           owner->m_device.dwriteFactory(), owner->m_device.textFormat())
            && PtInRect(&track, in.point))
        {
            return ui::EventOutcome::Handled;
        }
        owner->m_ocrPanel.ShowContextMenu(
            owner->m_window, owner->m_selection, in.point, owner->m_device.dpi(),
            owner->m_device.dwriteFactory(), owner->m_device.textFormat());
        inv |= ui::Invalidate::Paint;
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome Overlay::OcrLayer::OnWheel(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        owner->m_ocrPanel.OnWheel(
            owner->m_selection, in.wheelDelta, owner->m_device.dpi(), kHudTextSizeDip,
            owner->m_device.dwriteFactory(), owner->m_device.textFormat());
        inv |= ui::Invalidate::Paint;
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome Overlay::OcrLayer::OnKeyDown(
        WPARAM key, KeyModifiers mods, ui::Invalidate& inv)
    {
        if (key == VK_ESCAPE)
        {
            owner->Cancel();
            return ui::EventOutcome::Handled;
        }
        bool shouldClose = false;
        bool needsRender = false;
        if (owner->m_ocrPanel.OnKeyDown(
            owner->m_window, owner->m_selection, key, owner->m_device.dpi(),
            owner->m_device.dwriteFactory(), owner->m_device.textFormat(),
            shouldClose, needsRender, mods))
        {
            if (shouldClose)
            {
                owner->Cancel();
            }
            else if (needsRender)
            {
                inv |= ui::Invalidate::Paint;
            }
            return ui::EventOutcome::Handled;
        }
        return ui::EventOutcome::Ignored;
    }

    ui::EventOutcome Overlay::OcrLayer::OnChar(wchar_t, ui::Invalidate&)
    {
        return ui::EventOutcome::Handled;
    }

    std::optional<LPCWSTR> Overlay::OcrLayer::CursorForPoint(POINT point, bool) const
    {
        const RECT track = owner->m_ocrPanel.ScrollTrack(owner->m_selection, owner->m_device.dpi());
        if (owner->m_ocrPanel.HasScrollbar(owner->m_selection, owner->m_device.dpi(),
                                           owner->m_device.dwriteFactory(), owner->m_device.textFormat())
            && PtInRect(&track, point))
        {
            return IDC_ARROW;
        }
        return IDC_IBEAM;
    }

    // -------------------------------------------------------------------------
    // Layer 5: Canvas (selection handles, UIA smart picking, annotations)
    // -------------------------------------------------------------------------
    void Overlay::CanvasLayer::OnHoverMove(POINT point, ui::Invalidate& inv)
    {
        if (owner->m_smart)
        {
            owner->RefreshSuggestion(point);
        }
        inv |= ui::Invalidate::Paint;
    }

    ui::EventOutcome Overlay::CanvasLayer::OnLeftDown(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        if (owner->m_toolMode == ToolbarButton::TextRecognition)
        {
            return ui::EventOutcome::Handled;
        }

        const bool wasFollowing = owner->m_smart;
        owner->StopSelectionAnimation();
        owner->m_displaySelection = owner->m_selection;
        owner->m_smart = false;
        owner->m_depth = 0;

        if (wasFollowing)
        {
            owner->m_canvasDrag = CanvasDrag::NewRectangle;
            owner->m_dragOrigin = in.point;
            owner->m_dragStart = owner->m_selection;
            owner->m_pendingConfirm = owner->m_hasSelection;
            owner->m_reselectPending = false;
            if (!owner->m_hasSelection)
            {
                owner->m_toolbarController.Hide();
            }
            return ui::EventOutcome::Capture;
        }

        if (owner->m_hasSelection)
        {
            const ULONGLONG now = GetTickCount64();
            const bool isDoubleClick = (now - owner->m_lastClickMs <= GetDoubleClickTime())
                && std::abs(in.point.x - owner->m_lastClickPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
                && std::abs(in.point.y - owner->m_lastClickPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
            owner->m_lastClickMs = now;
            owner->m_lastClickPt = in.point;

            const float annotHandleDip = (in.kind == ui::PointerKind::Touch) ? 18.0f : 8.0f;
            if (owner->m_annotController.TryBeginSelectedHandleDrag(
                in.point, owner->m_toolMode, owner->m_device.dwriteFactory(),
                owner->m_device.dpi(), annotHandleDip))
            {
                owner->m_canvasDrag = CanvasDrag::Annotation;
                inv |= ui::Invalidate::Paint;
                return ui::EventOutcome::Capture;
            }

            const SelectionPart selPart = HitTestSelection(
                owner->m_selection, in.point, owner->EffectiveHandleSize(in.kind));
            if (selPart != SelectionPart::None && selPart != SelectionPart::Body)
            {
                owner->m_canvasDrag = CanvasDrag::MoveOrResize;
                owner->m_dragPart = selPart;
                owner->m_dragOrigin = in.point;
                owner->m_dragStart = owner->m_selection;
                owner->m_toolbarController.Hide();
                inv |= ui::Invalidate::Paint;
                return ui::EventOutcome::Capture;
            }

            if (owner->m_toolMode == ToolbarButton::EditSelection)
            {
                if (selPart == SelectionPart::Body)
                {
                    owner->m_canvasDrag = CanvasDrag::MoveOrResize;
                    owner->m_dragPart = SelectionPart::Body;
                    owner->m_dragOrigin = in.point;
                    owner->m_dragStart = owner->m_selection;
                    owner->m_toolbarController.Hide();
                    inv |= ui::Invalidate::Paint;
                    return ui::EventOutcome::Capture;
                }
                if (owner->m_annotController.empty())
                {
                    owner->m_canvasDrag = CanvasDrag::NewRectangle;
                    owner->m_dragOrigin = in.point;
                    owner->m_dragStart = owner->m_selection;
                    owner->m_reselectPending = true;
                    return ui::EventOutcome::Capture;
                }
                return ui::EventOutcome::Handled;
            }

            bool canvasSyncTb = false;
            bool canvasSyncIme = false;
            owner->m_annotController.OnCanvasLeftButtonDown(
                owner->m_window, in.point, owner->m_toolMode, owner->m_selection, in.mods,
                isDoubleClick, owner->m_device.dwriteFactory(), owner->m_device.dpi(),
                canvasSyncTb, canvasSyncIme);
            if (owner->m_annotController.ConsumeStepNumberEditRequest())
            {
                const RECT curBox = owner->m_toolbarController.StepNumberItemRect();
                if (canvasSyncTb || IsRectEmpty(&curBox))
                {
                    owner->RunLayout();
                }
                owner->m_toolbarController.BeginStepNumberEdit(inv);
                return ui::EventOutcome::Handled;
            }
            if (canvasSyncTb) inv |= ui::Invalidate::Layout;
            if (canvasSyncIme) inv |= ui::Invalidate::Ime;
            inv |= ui::Invalidate::Paint;
            if (owner->m_annotController.isInteracting())
            {
                owner->m_canvasDrag = CanvasDrag::Annotation;
                return ui::EventOutcome::Capture;
            }
            return ui::EventOutcome::Handled;
        }

        if (owner->m_toolMode == ToolbarButton::EditSelection)
        {
            owner->m_depth = 0;
            owner->m_toolbarController.Hide();
            owner->m_canvasDrag = CanvasDrag::NewRectangle;
            owner->m_dragOrigin = in.point;
            owner->m_hasSelection = false;
            owner->m_pendingConfirm = false;
            owner->m_reselectPending = false;
            return ui::EventOutcome::Capture;
        }

        return ui::EventOutcome::Handled;
    }

    void Overlay::CanvasLayer::OnCapturedMove(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        switch (owner->m_canvasDrag)
        {
        case CanvasDrag::Annotation:
            owner->m_annotController.OnMouseMove(
                owner->m_window, in.point, in.mods, owner->m_selection,
                owner->m_device.dwriteFactory(), owner->m_device.dpi());
            break;

        case CanvasDrag::NewRectangle:
        {
            if (owner->m_pendingConfirm || owner->m_reselectPending)
            {
                const int dx = std::abs(static_cast<int>(in.point.x - owner->m_dragOrigin.x));
                const int dy = std::abs(static_cast<int>(in.point.y - owner->m_dragOrigin.y));
                if (dx < kMinimumDrag && dy < kMinimumDrag)
                {
                    break;
                }
                owner->m_pendingConfirm = false;
                owner->m_reselectPending = false;
                owner->m_hasSelection = false;
                owner->m_toolbarController.Hide();
            }
            const RECT bounds = owner->ClientBounds();
            RECT candidate = RectFromPoints(owner->m_dragOrigin, in.point);
            RECT clipped{};
            if (IntersectRect(&clipped, &candidate, &bounds))
            {
                owner->m_selection = clipped;
                owner->m_hasSelection = !IsEmptyRect(clipped);
            }
            break;
        }

        case CanvasDrag::MoveOrResize:
            owner->m_selection = DragSelection(
                owner->m_dragStart, owner->m_dragPart, owner->m_dragOrigin,
                in.point, owner->ClientBounds());
            owner->m_hasSelection = true;
            break;

        case CanvasDrag::None:
        default:
            break;
        }
        inv |= ui::Invalidate::Paint;
    }

    void Overlay::CanvasLayer::OnCapturedUp(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        const CanvasDrag drag = owner->m_canvasDrag;
        owner->m_canvasDrag = CanvasDrag::None;
        owner->m_dragPart = SelectionPart::None;

        if (drag == CanvasDrag::Annotation)
        {
            bool syncTb = false;
            owner->m_annotController.OnLeftButtonUp(
                in.point, owner->m_selection, owner->m_device.dwriteFactory(),
                owner->m_device.dpi(), syncTb);
            if (syncTb) inv |= ui::Invalidate::Layout;
            inv |= ui::Invalidate::Paint;
            return;
        }

        if (drag == CanvasDrag::NewRectangle || drag == CanvasDrag::MoveOrResize)
        {
            if (owner->m_reselectPending)
            {
                owner->m_reselectPending = false;
                inv |= ui::Invalidate::Paint;
                return;
            }
            if (owner->m_pendingConfirm)
            {
                owner->m_pendingConfirm = false;
                owner->m_smart = false;
                owner->m_depth = 0;
                owner->Settle();
                inv |= ui::Invalidate::Paint;
                return;
            }
            if (drag == CanvasDrag::NewRectangle)
            {
                const RECT& r = owner->m_selection;
                if (!owner->m_hasSelection || IsEmptyRect(r)
                    || (r.right - r.left) < kMinimumDrag || (r.bottom - r.top) < kMinimumDrag)
                {
                    owner->m_hasSelection = false;
                    owner->m_smart = true;
                    owner->m_depth = 0;
                    owner->RefreshSuggestion(in.point);
                    inv |= ui::Invalidate::Paint;
                    return;
                }
            }
            owner->Settle();
            inv |= ui::Invalidate::Paint;
        }
    }

    void Overlay::CanvasLayer::OnCancelCapture(ui::Invalidate& inv)
    {
        const CanvasDrag prev = owner->m_canvasDrag;
        owner->m_canvasDrag = CanvasDrag::None;
        owner->m_dragPart = SelectionPart::None;
        owner->m_pendingConfirm = false;
        owner->m_reselectPending = false;
        owner->m_annotController.ClearTransientDragState();
        if (prev == CanvasDrag::MoveOrResize || (prev == CanvasDrag::NewRectangle && owner->m_hasSelection))
        {
            owner->Settle();
        }
        inv |= ui::Invalidate::Paint;
    }

    void Overlay::CanvasLayer::OnCapturedModifiersChanged(
        POINT point, KeyModifiers mods, ui::Invalidate& inv)
    {
        if (owner->m_canvasDrag == CanvasDrag::Annotation
            && owner->m_annotController.OnModifiersChanged(
                point, mods, owner->m_selection,
                owner->m_device.dwriteFactory(), owner->m_device.dpi()))
        {
            inv |= ui::Invalidate::Paint;
        }
    }

    ui::EventOutcome Overlay::CanvasLayer::OnRightUp(const ui::PointerInput&, ui::Invalidate&)
    {
        owner->Cancel();
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome Overlay::CanvasLayer::OnWheel(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        if (owner->m_toolMode == ToolbarButton::TextRecognition)
        {
            return ui::EventOutcome::Handled;
        }

        if (owner->m_smart)
        {
            const int steps = owner->m_canvasWheelAccum.Feed(in.wheelDelta, 1u);
            if (steps == 0)
            {
                return ui::EventOutcome::Handled;
            }
            const int previousDepth = owner->m_depth;
            owner->m_depth -= steps;
            owner->m_depth = std::clamp(owner->m_depth, -8, 0);

            const POINT screen{ in.point.x + owner->m_bounds.left, in.point.y + owner->m_bounds.top };
            if (owner->m_picker.Ladder(screen, owner->m_ladder, 6) < 0)
            {
                owner->m_depth = previousDepth;
                return ui::EventOutcome::Handled;
            }
            owner->m_lastQuery = GetTickCount64();
            owner->ApplyLadderLevel();
            inv |= ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;
        }

        if (owner->m_toolMode == ToolbarButton::EditSelection && owner->m_hasSelection)
        {
            const int steps = owner->m_canvasWheelAccum.Feed(in.wheelDelta, 2u);
            if (steps == 0)
            {
                return ui::EventOutcome::Handled;
            }
            const RECT bounds = owner->ClientBounds();
            RECT adjusted = owner->m_selection;
            if (steps > 0)
            {
                adjusted.left = std::max(bounds.left, adjusted.left - steps);
                adjusted.top = std::max(bounds.top, adjusted.top - steps);
                adjusted.right = std::min(bounds.right, adjusted.right + steps);
                adjusted.bottom = std::min(bounds.bottom, adjusted.bottom + steps);
            }
            else
            {
                const int shrink = -steps;
                if (adjusted.right - adjusted.left <= shrink * 2
                    || adjusted.bottom - adjusted.top <= shrink * 2)
                {
                    return ui::EventOutcome::Handled;
                }
                adjusted.left += shrink;
                adjusted.top += shrink;
                adjusted.right -= shrink;
                adjusted.bottom -= shrink;
            }
            if (!EqualRect(&adjusted, &owner->m_selection))
            {
                owner->m_selection = adjusted;
                owner->m_displaySelection = adjusted;
                owner->Settle();
                inv |= ui::Invalidate::Paint;
            }
        }
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome Overlay::CanvasLayer::OnKeyDown(
        WPARAM key, KeyModifiers mods, ui::Invalidate& inv)
    {
        if (key == VK_ESCAPE)
        {
            owner->Cancel();
            return ui::EventOutcome::Handled;
        }

        if (!owner->m_smart && owner->m_hasSelection)
        {
            bool syncTb = false;
            bool syncIme = false;
            if (owner->m_annotController.OnKeyDown(
                owner->m_window, key, owner->m_toolMode,
                owner->m_device.dwriteFactory(), owner->m_device.dpi(),
                syncTb, syncIme, mods))
            {
                if (owner->m_colorPicker.isOpen()
                    && (key == 'Z' || key == 'Y' || key == VK_DELETE || key == VK_BACK))
                {
                    owner->DismissColorPicker(false, inv);
                }
                if (syncTb) inv |= ui::Invalidate::Layout;
                if (syncIme) inv |= ui::Invalidate::Ime;
                inv |= ui::Invalidate::Paint;
                return ui::EventOutcome::Handled;
            }
        }

        if (key == VK_RETURN && !owner->m_smart && owner->m_hasSelection)
        {
            owner->Invoke(ToolbarButton::Copy, inv);
            return ui::EventOutcome::Handled;
        }
        if (key == 'C' && mods.ctrl && !owner->m_smart && owner->m_hasSelection)
        {
            owner->Invoke(ToolbarButton::Copy, inv);
            return ui::EventOutcome::Handled;
        }

        if (key == VK_LEFT || key == VK_UP || key == VK_RIGHT || key == VK_DOWN)
        {
            POINT cursor{};
            if (GetCursorPos(&cursor))
            {
                switch (key)
                {
                case VK_LEFT:  --cursor.x; break;
                case VK_UP:    --cursor.y; break;
                case VK_RIGHT: ++cursor.x; break;
                case VK_DOWN:  ++cursor.y; break;
                }
                SetCursorPos(cursor.x, cursor.y);
            }
            return ui::EventOutcome::Handled;
        }
        return ui::EventOutcome::Ignored;
    }

    std::optional<LPCWSTR> Overlay::CanvasLayer::CursorForPoint(POINT point, bool captured) const
    {
        if (captured)
        {
            switch (owner->m_canvasDrag)
            {
            case CanvasDrag::MoveOrResize:
                return CursorForPart(owner->m_dragPart);
            case CanvasDrag::NewRectangle:
                return IDC_CROSS;
            case CanvasDrag::Annotation:
                if (const LPCWSTR hc = owner->m_annotController.CursorForSelectedHandle(
                    point, owner->m_toolMode, owner->m_device.dwriteFactory(), owner->m_device.dpi()))
                {
                    return hc;
                }
                return (owner->m_toolMode == ToolbarButton::SelectObject || QueryCurrentKeyModifiers().ctrl)
                    ? IDC_ARROW
                    : IDC_CROSS;
            case CanvasDrag::None:
            default:
                break;
            }
        }

        if (owner->m_toolMode == ToolbarButton::TextRecognition)
        {
            return IDC_ARROW;
        }
        if (owner->m_smart)
        {
            return IDC_CROSS;
        }
        if (owner->m_hasSelection)
        {
            if (const LPCWSTR hc = owner->m_annotController.CursorForSelectedHandle(
                point, owner->m_toolMode, owner->m_device.dwriteFactory(), owner->m_device.dpi()))
            {
                return hc;
            }
            const SelectionPart selPart = HitTestSelection(owner->m_selection, point, owner->m_handleSize);
            if (selPart != SelectionPart::None && selPart != SelectionPart::Body)
            {
                return CursorForPart(selPart);
            }
            if (owner->m_toolMode == ToolbarButton::EditSelection)
            {
                if (selPart == SelectionPart::Body)
                {
                    return IDC_SIZEALL;
                }
                return owner->m_annotController.empty() ? IDC_CROSS : IDC_ARROW;
            }
            return owner->m_annotController.CursorForCanvasPoint(
                point, owner->m_toolMode, QueryCurrentKeyModifiers(),
                owner->m_device.dwriteFactory(), owner->m_device.dpi());
        }
        return (owner->m_toolMode == ToolbarButton::EditSelection) ? IDC_CROSS : IDC_ARROW;
    }

    // -------------------------------------------------------------------------
    // Memory / VRAM pressure trimming & DirectComposition frame rendering
    // -------------------------------------------------------------------------
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
        m_annotController.DiscardDeviceResources();
        m_colorPicker.DiscardDeviceResources();
        m_colorPicker.InvalidateLayouts();
        m_toolbarController.InvalidateLayouts();
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

        const HRESULT beginHr = m_device.BeginFrameDraw(m_width, m_height);
        if (FAILED(beginHr))
        {
            return beginHr;
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

        // Pass 1: Full-canvas annotation primitives underneath the dim mask
        const RECT activeSelection = m_smart ? m_displaySelection : m_selection;
        if (m_toolMode != ToolbarButton::TextRecognition
            && (!m_annotController.empty() || m_annotController.isInteracting()))
        {
            m_annotController.DrawCanvasContent(
                m_device.d2dFactory(),
                context,
                m_device.roundStroke(),
                m_device.dwriteFactory(),
                m_device.background(),
                m_device.dpi());
        }

        // Pass 2: Dim mask outside activeSelection
        m_renderer.DrawDim(context, m_width, m_height, m_hasSelection, activeSelection,
                           m_device.brushes());

        // Pass 3: Selection border/handles & unclipped annotation chrome / OCR panel
        if (m_hasSelection)
        {
            if (m_toolMode == ToolbarButton::TextRecognition)
            {
                m_ocrPanel.Draw(context, m_device.dwriteFactory(), m_device.textFormat(),
                                m_selection, m_device.dpi(), m_device.brushes());
            }
            else
            {
                m_renderer.DrawSelection(
                    m_device, m_width, m_height, activeSelection, m_smart, m_handleSize, true);
                m_annotController.DrawOverlayChrome(
                    m_device.d2dFactory(),
                    context,
                    m_device.dwriteFactory(),
                    m_device.dpi(),
                    &m_device.brushes());
            }
        }

        // Pass 4: Floating HUD overlays (Toolbar, ColorPicker, Tooltip, PixelMagnifier)
        if (m_toolbarController.IsVisible())
        {
            const bool allowTooltip = !m_layerStack.hasCapture() && !m_colorPicker.visible();
            m_toolbarController.Draw(m_renderer, m_device, m_toolMode, allowTooltip);
            if (m_colorPicker.visible())
            {
                m_colorPicker.Draw(
                    context, m_device.dwriteFactory(), m_device.textFormat(),
                    m_device.brushes(), m_device.dpi(),
                    m_renderer.EyedropperSvg(context));
            }
        }

        const ui::IHudLayer* topLayer = m_layerStack.TopmostHit(m_pointer);
        const bool pointerOverUi = !m_eyedropperActive
            && (topLayer == &m_toolbarController || topLayer == &m_colorPickerLayer);
        const bool draggingCanvasSel =
            (m_canvasDrag == CanvasDrag::NewRectangle || m_canvasDrag == CanvasDrag::MoveOrResize);
        if ((m_eyedropperActive
             || (m_toolMode == ToolbarButton::EditSelection
                 && !m_toolbarController.isDraggingMove()
                 && (!m_toolbarController.visible()
                     || (m_hasSelection && PtInRect(&m_selection, m_pointer))
                     || draggingCanvasSel)))
            && !pointerOverUi)
        {
            m_renderer.DrawPixelMagnifier(
                m_device, m_width, m_height, m_image, m_bounds,
                m_selection, m_pointer, m_canvasDrag == CanvasDrag::MoveOrResize,
                m_dragPart, draggingCanvasSel);
        }
        m_stats.Draw(m_device, m_width);

        abortFrame.release();
        const HRESULT endHr = m_device.EndFrameDraw();
        if (collectStats && SUCCEEDED(endHr))
        {
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
            if (m_ocrPanel.HandleResultMessage(lparam) && m_toolMode == ToolbarButton::TextRecognition)
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
            m_colorPicker.InvalidateLayouts();
            m_toolbarController.InvalidateLayouts();
            m_renderer.InvalidateLayouts();
            m_device.OnDpiChanged(dpi);
            if (m_toolbarController.visible())
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
                || (m_toolMode == ToolbarButton::TextRecognition && m_ocrPanel.pending()))
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
                return 0;
            }

            const ui::PointerKind pkind = (info.pointerType == PT_TOUCH)
                ? ui::PointerKind::Touch
                : ((info.pointerType == PT_PEN) ? ui::PointerKind::Pen : ui::PointerKind::Mouse);

            ui::Invalidate inv = ui::Invalidate::None;
            if ((info.pointerFlags & POINTER_FLAG_CANCELED) != 0)
            {
                m_layerStack.CancelCapture(inv);
                m_layerStack.ClearHover(inv);
                FlushInvalidation(inv);
                return 0;
            }

            const ui::PointerInput in{ point, QueryCurrentKeyModifiers(), 0, pkind };

            if (info.ButtonChangeType == POINTER_CHANGE_FIRSTBUTTON_DOWN
                || (message == WM_POINTERDOWN && info.ButtonChangeType == POINTER_CHANGE_NONE))
            {
                m_pointer = point;
                m_layerStack.DispatchLeftDown(in, inv);
                FlushInvalidation(inv);
                return 0;
            }

            if (info.ButtonChangeType == POINTER_CHANGE_FIRSTBUTTON_UP
                || (message == WM_POINTERUP && info.ButtonChangeType == POINTER_CHANGE_NONE))
            {
                m_pointer = point;
                m_layerStack.DispatchLeftUp(in, inv);
                if (pkind == ui::PointerKind::Touch)
                {
                    m_layerStack.ClearHover(inv);
                }
                FlushInvalidation(inv);
                return 0;
            }

            if (info.ButtonChangeType == POINTER_CHANGE_SECONDBUTTON_UP)
            {
                m_pointer = point;
                m_layerStack.DispatchRightUp(in, inv);
                FlushInvalidation(inv);
                return 0;
            }

            // Touch has no non-contact hover cursor.
            if (pkind == ui::PointerKind::Touch
                && !m_layerStack.hasCapture()
                && (info.pointerFlags & POINTER_FLAG_INCONTACT) == 0)
            {
                return 0;
            }

            if (point.x != m_pointer.x || point.y != m_pointer.y)
            {
                m_pointer = point;
                m_layerStack.DispatchMove(in, inv);
                FlushInvalidation(inv);
            }
            return 0;
        }

        case WM_POINTERENTER:
        {
            POINTER_INFO info{};
            POINT point{};
            if (!PointerEvent(wparam, info, point))
            {
                return 0;
            }
            if (info.pointerType == PT_TOUCH)
            {
                return 0;
            }
            if (point.x != m_pointer.x || point.y != m_pointer.y)
            {
                m_pointer = point;
                const ui::PointerKind pkind =
                    (info.pointerType == PT_PEN) ? ui::PointerKind::Pen : ui::PointerKind::Mouse;
                ui::Invalidate inv = ui::Invalidate::None;
                m_layerStack.DispatchMove(
                    ui::PointerInput{ point, QueryCurrentKeyModifiers(), 0, pkind }, inv);
                FlushInvalidation(inv);
            }
            return 0;
        }

        case WM_POINTERLEAVE:
        {
            ui::Invalidate inv = ui::Invalidate::None;
            m_layerStack.ClearHover(inv);
            FlushInvalidation(inv);
            return 0;
        }

        case WM_POINTERWHEEL:
        {
            POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(hwnd, &point);
            const RECT bounds = ClientBounds();
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (delta != 0 && PtInRect(&bounds, point))
            {
                ui::Invalidate inv = ui::Invalidate::None;
                m_layerStack.DispatchWheel(
                    ui::PointerInput{ point, QueryCurrentKeyModifiers(), delta }, inv);
                FlushInvalidation(inv);
            }
            return 0;
        }

        case WM_POINTERHWHEEL:
            return 0;

        case WM_POINTERCAPTURECHANGED:
        case WM_CANCELMODE:
        {
            ui::Invalidate inv = ui::Invalidate::Paint;
            m_layerStack.CancelCapture(inv);
            if (m_eyedropperActive)
            {
                m_eyedropperActive = false;
                m_colorPicker.ResumeFromEyedropper();
                inv |= ui::Invalidate::Ime;
            }
            FlushInvalidation(inv);
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lparam) != HTCLIENT)
            {
                break;
            }
            ApplyCursor();
            return TRUE;

        case WM_IME_SETCONTEXT:
            lparam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
            return DefWindowProcW(hwnd, message, wparam, lparam);

        case WM_IME_STARTCOMPOSITION:
            if (m_layerStack.FocusedImeLayer() == &m_textEditorLayer
                && m_annotController.OnImeStartComposition(m_window, m_device.dwriteFactory(), m_device.dpi()))
            {
                RequestRender();
                return 0;
            }
            break;

        case WM_IME_COMPOSITION:
            if (m_layerStack.FocusedImeLayer() == &m_textEditorLayer
                && m_annotController.OnImeComposition(m_window, lparam, m_device.dwriteFactory(), m_device.dpi()))
            {
                RequestRender();
                return 0;
            }
            break;

        case WM_IME_ENDCOMPOSITION:
            if (m_layerStack.FocusedImeLayer() == &m_textEditorLayer
                && m_annotController.OnImeEndComposition(m_window))
            {
                RequestRender();
                return 0;
            }
            break;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wparam == VK_SHIFT || wparam == VK_CONTROL || wparam == VK_MENU)
            {
                ui::Invalidate inv = ui::Invalidate::None;
                m_layerStack.DispatchModifiersChanged(m_pointer, QueryCurrentKeyModifiers(), inv);
                FlushInvalidation(inv);
                if (message == WM_SYSKEYUP && wparam == VK_MENU)
                {
                    return 0;
                }
            }
            break;

        case WM_SYSKEYDOWN:
            if (wparam == VK_MENU)
            {
                ui::Invalidate inv = ui::Invalidate::None;
                m_layerStack.DispatchModifiersChanged(m_pointer, QueryCurrentKeyModifiers(), inv);
                FlushInvalidation(inv);
                return 0;
            }
            break;

        case WM_KEYDOWN:
        {
            if (wparam == VK_SHIFT || wparam == VK_CONTROL || wparam == VK_MENU)
            {
                ui::Invalidate inv = ui::Invalidate::None;
                m_layerStack.DispatchModifiersChanged(m_pointer, QueryCurrentKeyModifiers(), inv);
                FlushInvalidation(inv);
                return 0;
            }
            ui::Invalidate inv = ui::Invalidate::None;
            if (m_layerStack.DispatchKeyDown(wparam, QueryCurrentKeyModifiers(), inv))
            {
                FlushInvalidation(inv);
                return 0;
            }
            break;
        }

        case WM_CHAR:
        {
            ui::Invalidate inv = ui::Invalidate::None;
            if (m_layerStack.DispatchChar(static_cast<wchar_t>(wparam), inv))
            {
                FlushInvalidation(inv);
                return 0;
            }
            break;
        }

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
