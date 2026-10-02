#include "pch.h"

#include "hud/overlay.h"

#include "core/error.h"
#include "hud/toolbar_icon.h"
#include "core/screen_capture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

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
        constexpr int kButtonRadius = 4;

        // One stroke weight for every icon in the toolbar, so the row reads as a
        // single set. It is a ceiling as much as a choice: the text-recognition
        // mark's three bars are only 5.38 reference units apart, and a heavier
        // stroke closes that gap until the bars merge into one block.
        constexpr float kIconStroke = 1.8f;

        // The size every mark is matched to, measured across the ink rather than
        // across the geometry. The marks are different shapes (a square, brackets
        // and bars, an X, two overlapping squares), so matching their centre lines
        // leaves them looking different sizes; each is laid out so that the
        // painted extent comes out this wide.
        constexpr float kIconInk = 16.0f;

        // The text-recognition scrollbar. At 3 DIP its track was barely three
        // pixels wide at 1x and read as a hairline rather than something you
        // could grab, so it is laid out as a pill of this width instead.
        constexpr int kOcrScrollWidth = 6;
        constexpr int kOcrScrollInset = 5;
        // Room the text leaves free on the right so it cannot run under the bar.
        constexpr int kOcrScrollGutter = 10;

        // A drag that never reaches this size is a click that wobbled, not a
        // selection, and goes back to following the pointer.
        constexpr int kMinimumDrag = 3;
        constexpr UINT_PTR kOcrSelectionTimer = 0xA651;
        constexpr UINT kOcrSelectionTimerMs = 30;

        // UIA is a cross-process call. The pointer moves far more often than a
        // suggestion can usefully change, so the call is throttled.
        constexpr ULONGLONG kQueryIntervalMs = 30;
        constexpr ULONGLONG kSelectionAnimationMs = 101;

        LONGLONG PerformanceCounterFrequency() noexcept
        {
            static const LONGLONG frequency = []
            {
                LARGE_INTEGER value{};
                QueryPerformanceFrequency(&value);
                return value.QuadPart;
            }();
            return frequency;
        }
        constexpr UINT kSelectionAnimationFrameMessage = WM_APP + 0x4A;
        constexpr DWORD kCompositorClockTimeoutMs = 50;
        constexpr DWORD kClockRetryDelayMs = 50;

        using WaitForCompositorClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

        WaitForCompositorClockFn CompositorClockWaiter() noexcept
        {
            static const auto wait = reinterpret_cast<WaitForCompositorClockFn>(
                GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionWaitForCompositorClock"));
            return wait;
        }

        class CompositorClockService
        {
        public:
            winrt::event_token Add(winrt::delegate<void()> callback)
            {
                winrt::event_token token{};
                {
                    std::lock_guard lock(m_waitMutex);
                    token = m_clockTick.add(callback);
                }
                m_wake.notify_one();
                return token;
            }

            void Remove(winrt::event_token token)
            {
                {
                    std::lock_guard lock(m_waitMutex);
                    m_clockTick.remove(token);
                }
                m_wake.notify_one();
            }

        private:
            CompositorClockService()
            {
                std::thread([this] { Run(); }).detach();
            }

            void Run() noexcept
            {
                const auto waitForClock = CompositorClockWaiter();
                for (;;)
                {
                    {
                        std::unique_lock lock(m_waitMutex);
                        m_wake.wait(lock, [this] { return static_cast<bool>(m_clockTick); });
                    }

                    if (waitForClock == nullptr)
                    {
                        Sleep(16);
                        m_clockTick();
                        continue;
                    }

                    const DWORD status = waitForClock(0, nullptr, kCompositorClockTimeoutMs);
                    if (status == ERROR_SUCCESS)
                    {
                        m_clockTick();
                    }
                    else if (status != WAIT_TIMEOUT)
                    {
                        Sleep(kClockRetryDelayMs);
                    }
                }
            }

            std::mutex m_waitMutex;
            std::condition_variable m_wake;
            winrt::event<winrt::delegate<void()>> m_clockTick;

            friend CompositorClockService& GlobalCompositorClock();
        };

        CompositorClockService& GlobalCompositorClock()
        {
            static auto* service = new CompositorClockService();
            return *service;
        }

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

        struct FlagReset
        {
            bool& flag;
            ~FlagReset() { flag = false; }
        };

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
        m_dpi = 96;
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

        m_dpi = static_cast<int>(GetDpiForWindow(m_window));
        if (m_dpi <= 0)
        {
            m_dpi = 96;
        }
        m_handleSize = MulDiv(kHandleSize, m_dpi, 96);

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
        StopSelectionAnimation();
        StopAnimationClock();
        StopOcrSelection();
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
        m_magnifierLayer = nullptr;
        m_copyIconGeometry = nullptr;
        m_copyLocalStroke = 0.0f;
        m_roundStroke = nullptr;
        m_flatStroke = nullptr;
        m_textFormat = nullptr;
        m_magnifierTextFormat = nullptr;
        m_dwriteFactory = nullptr;
        m_icon = nullptr;
        m_cancel = nullptr;
        m_tooltip = nullptr;
        m_textScrollThumb = nullptr;
        m_textScrollTrack = nullptr;
        m_hover = nullptr;
        m_panelEdge = nullptr;
        m_panel = nullptr;
        m_handleEdge = nullptr;
        m_handleFill = nullptr;
        m_accent = nullptr;
        m_shadow = nullptr;
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
        AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            Scaled(MeasuredPixels(18.0f)),
            L"en-US",
            m_magnifierTextFormat.put()));

        // Round caps, so the cross of the cancel button reads as a drawn mark
        // rather than four clipped ends.
        const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 1.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        AGSHOT_CHECK_HR(m_d2dFactory->CreateStrokeStyle(properties, nullptr, 0, m_roundStroke.put()));

        // The text-recognition mark is traced from a reference image. Its bars
        // stop square - a round cap would add half a stroke to every end - while
        // its bracket corners are rounded, so it needs its own style.
        const D2D1_STROKE_STYLE_PROPERTIES tracedProperties = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
            D2D1_LINE_JOIN_ROUND, 1.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        AGSHOT_CHECK_HR(m_d2dFactory->CreateStrokeStyle(
            tracedProperties, nullptr, 0, m_flatStroke.put()));
    }

    void Overlay::CreateCopyIconGeometry(float localStroke)
    {
        constexpr float size = 18.0f;
        constexpr float radius = 2.0f;
        constexpr float curve = radius * 0.55228475f;
        constexpr float backRight = size * 0.62f;
        constexpr float backBottom = size * 0.72f;
        constexpr float frontLeft = size * 0.30f;
        constexpr float frontTop = size * 0.28f;

        // The back square, as a single open path whose ends stop on the front
        // square's centre line, where the merge below hides them.
        winrt::com_ptr<ID2D1PathGeometry> back;
        AGSHOT_CHECK_HR(m_d2dFactory->CreatePathGeometry(back.put()));
        winrt::com_ptr<ID2D1GeometrySink> backSink;
        AGSHOT_CHECK_HR(back->Open(backSink.put()));
        backSink->SetFillMode(D2D1_FILL_MODE_WINDING);
        backSink->BeginFigure(D2D1::Point2F(frontLeft, backBottom), D2D1_FIGURE_BEGIN_HOLLOW);
        backSink->AddLine(D2D1::Point2F(radius, backBottom));
        backSink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(radius - curve, backBottom),
            D2D1::Point2F(0.0f, backBottom - radius + curve),
            D2D1::Point2F(0.0f, backBottom - radius)));
        backSink->AddLine(D2D1::Point2F(0.0f, radius));
        backSink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(0.0f, radius - curve),
            D2D1::Point2F(radius - curve, 0.0f),
            D2D1::Point2F(radius, 0.0f)));
        backSink->AddLine(D2D1::Point2F(backRight - radius, 0.0f));
        backSink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(backRight - radius + curve, 0.0f),
            D2D1::Point2F(backRight, radius - curve),
            D2D1::Point2F(backRight, radius)));
        backSink->AddLine(D2D1::Point2F(backRight, frontTop));
        backSink->EndFigure(D2D1_FIGURE_END_OPEN);
        AGSHOT_CHECK_HR(backSink->Close());

        // The front square, closed.
        winrt::com_ptr<ID2D1PathGeometry> front;
        AGSHOT_CHECK_HR(m_d2dFactory->CreatePathGeometry(front.put()));
        winrt::com_ptr<ID2D1GeometrySink> frontSink;
        AGSHOT_CHECK_HR(front->Open(frontSink.put()));
        frontSink->SetFillMode(D2D1_FILL_MODE_WINDING);
        frontSink->BeginFigure(D2D1::Point2F(frontLeft + radius, frontTop), D2D1_FIGURE_BEGIN_HOLLOW);
        frontSink->AddLine(D2D1::Point2F(size - radius, frontTop));
        frontSink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(size - radius + curve, frontTop),
            D2D1::Point2F(size, frontTop + radius - curve),
            D2D1::Point2F(size, frontTop + radius)));
        frontSink->AddLine(D2D1::Point2F(size, size - radius));
        frontSink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(size, size - radius + curve),
            D2D1::Point2F(size - radius + curve, size),
            D2D1::Point2F(size - radius, size)));
        frontSink->AddLine(D2D1::Point2F(frontLeft + radius, size));
        frontSink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(frontLeft + radius - curve, size),
            D2D1::Point2F(frontLeft, size - radius + curve),
            D2D1::Point2F(frontLeft, size - radius)));
        frontSink->AddLine(D2D1::Point2F(frontLeft, frontTop + radius));
        frontSink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(frontLeft, frontTop + radius - curve),
            D2D1::Point2F(frontLeft + radius - curve, frontTop),
            D2D1::Point2F(frontLeft + radius, frontTop)));
        frontSink->EndFigure(D2D1_FIGURE_END_CLOSED);
        AGSHOT_CHECK_HR(frontSink->Close());

        // Turn both outlines into the area their stroke would cover, then merge
        // the two areas into one geometry. Stroking the outlines separately
        // paints the junction twice, and the second pass composites over the
        // first one's antialiased edge, which leaves a pale seam there. A single
        // merged area is filled in one pass, so the junction stays solid.
        const auto widen = [&](ID2D1Geometry* source, winrt::com_ptr<ID2D1PathGeometry>& out)
        {
            AGSHOT_CHECK_HR(m_d2dFactory->CreatePathGeometry(out.put()));
            winrt::com_ptr<ID2D1GeometrySink> sink;
            AGSHOT_CHECK_HR(out->Open(sink.put()));
            sink->SetFillMode(D2D1_FILL_MODE_WINDING);
            AGSHOT_CHECK_HR(source->Widen(localStroke, m_roundStroke.get(), nullptr, sink.get()));
            AGSHOT_CHECK_HR(sink->Close());
        };

        winrt::com_ptr<ID2D1PathGeometry> backArea;
        winrt::com_ptr<ID2D1PathGeometry> frontArea;
        widen(back.get(), backArea);
        widen(front.get(), frontArea);

        AGSHOT_CHECK_HR(m_d2dFactory->CreatePathGeometry(m_copyIconGeometry.put()));
        winrt::com_ptr<ID2D1GeometrySink> sink;
        AGSHOT_CHECK_HR(m_copyIconGeometry->Open(sink.put()));
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        AGSHOT_CHECK_HR(backArea->CombineWithGeometry(
            frontArea.get(), D2D1_COMBINE_MODE_UNION, nullptr, sink.get()));
        AGSHOT_CHECK_HR(sink->Close());

        m_copyLocalStroke = localStroke;
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
        AGSHOT_CHECK_HR(m_d2dContext->CreateLayer(nullptr, m_magnifierLayer.put()));

        AGSHOT_CHECK_HR(DCompositionCreateDevice(
            m_dxgiDevice.get(), __uuidof(IDCompositionDevice), m_composition.put_void()));
        AGSHOT_CHECK_HR(m_composition->CreateTargetForHwnd(m_window, TRUE, m_target.put()));
        AGSHOT_CHECK_HR(m_composition->CreateVisual(m_visual.put()));

        const auto brush = [this](float r, float g, float b, float a, winrt::com_ptr<ID2D1SolidColorBrush>& out)
        {
            AGSHOT_CHECK_HR(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), out.put()));
        };

        brush(0.0f, 0.0f, 0.0f, 0.45f, m_dim);          // everything outside the selection
        brush(0.0f, 0.0f, 0.0f, 0.1f, m_shadow);        // magnifier card drop shadow
        brush(0.18f, 0.49f, 0.94f, 1.0f, m_accent);      // the selection border
        brush(1.0f, 1.0f, 1.0f, 1.0f, m_handleFill);
        brush(0.10f, 0.10f, 0.12f, 1.0f, m_handleEdge);
        brush(0.14f, 0.15f, 0.18f, 0.97f, m_panel);
        brush(1.0f, 1.0f, 1.0f, 0.14f, m_panelEdge);
        brush(1.0f, 1.0f, 1.0f, 0.13f, m_hover);
        brush(0.96f, 0.97f, 0.99f, 1.0f, m_icon);
        brush(0.94f, 0.35f, 0.35f, 1.0f, m_cancel);    // the one destructive mark
        brush(0.06f, 0.06f, 0.08f, 0.97f, m_tooltip);
        brush(0.90f, 0.91f, 0.93f, 1.0f, m_textScrollTrack);
        brush(0.48f, 0.51f, 0.56f, 1.0f, m_textScrollThumb);
    }

    void Overlay::CreateSizeDependentResources()
    {
        if (m_width == 0 || m_height == 0)
        {
            return;
        }

        // The copy mark's hidden ends are pulled back by half a stroke, so its
        // geometry has to be rebuilt whenever the DPI changes the stroke.
        const float localStroke =
            Scaled(kIconStroke) * 18.0f / (std::max(1.0f, Scaled(kIconInk)) - Scaled(kIconStroke));
        if (m_copyLocalStroke != localStroke)
        {
            CreateCopyIconGeometry(localStroke);
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

    float Overlay::Scaled(float value) const noexcept
    {
        return value * static_cast<float>(m_dpi) / 96.0f;
    }

    float Overlay::MeasuredPixels(float pixels) const noexcept
    {
        const float logicalSize = pixels * 96.0f / static_cast<float>(m_dpi);
        return Scaled(logicalSize);
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
        const FlagReset resetBeginning{ m_beginning };

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
        StopSelectionAnimation();
        StopAnimationClock();
        ClearSurface();

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

        m_selection = RECT{};
        m_displaySelection = RECT{};
        m_animationFrom = RECT{};
        m_animationTo = RECT{};
        m_animationStart = 0;
        m_hasSelection = false;
        m_smart = true;
        m_mode = OperationMode::EditSelection;
        m_ocrText.clear();
        m_ocrCacheValid = false;
        m_ocrCaret = 0;
        m_ocrAnchor = 0;
        m_ocrSelecting = false;
        m_ocrScrollDragging = false;
        m_ocrScrollY = 0.0f;
        m_depth = 0;
        m_dragMode = DragMode::None;
        m_dragPart = SelectionPart::None;
        m_pressedButton = ToolbarButton::None;
        m_pendingConfirm = false;
        m_toolbarVisible = false;
        m_toolbar.hovered = ToolbarButton::None;
        m_trackingLeave = false;

        // Resolve and draw the initial suggestion while hidden so the previous
        // capture cannot flash when the overlay is shown.
        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            ScreenToClient(m_window, &cursor);
            m_pointer = cursor;
            RefreshSuggestion(cursor, 6);
        }
        Render();

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
        StopAnimationClock();
        StopOcrSelection();
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

        if (EqualRect(&m_displaySelection, &target))
        {
            StopSelectionAnimation();
            return;
        }

        const bool wasAnimating = m_animatingSelection;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        m_animationFrom = m_displaySelection;
        m_animationTo = target;
        m_animationStart = now.QuadPart;
        m_animatingSelection = true;
        if (!wasAnimating && !StartAnimationClock())
        {
            m_displaySelection = target;
            StopSelectionAnimation();
        }
    }

    void Overlay::RequestRender()
    {
        if (m_window == nullptr || !IsWindowVisible(m_window))
        {
            Render();
            return;
        }
        if (!m_animationFrameSignal && !StartAnimationClock())
        {
            Render();
        }
    }

    bool Overlay::StartAnimationClock() noexcept
    {
        if (m_animationFrameSignal)
        {
            return true;
        }

        try
        {
            auto signal = std::make_shared<AnimationFrameSignal>();
            signal->window = m_window;
            m_animationFrameSignal = signal;
            m_animationClockToken = GlobalCompositorClock().Add(winrt::delegate<void()>{ [signal]()
            {
                std::lock_guard lock(signal->mutex);
                if (!signal->active || signal->posted)
                {
                    return;
                }

                signal->posted = true;
                if (!PostMessageW(signal->window, kSelectionAnimationFrameMessage, 0, 0))
                {
                    signal->posted = false;
                }
            } });
            return true;
        }
        catch (...)
        {
            m_animationFrameSignal.reset();
            return false;
        }
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

    void Overlay::StopAnimationClock() noexcept
    {
        if (m_animationFrameSignal)
        {
            {
                std::lock_guard lock(m_animationFrameSignal->mutex);
                m_animationFrameSignal->active = false;
            }
            GlobalCompositorClock().Remove(m_animationClockToken);
            m_animationClockToken = {};
            m_animationFrameSignal.reset();
        }

        if (m_window != nullptr)
        {
            MSG pending{};
            while (PeekMessageW(&pending, m_window,
                                kSelectionAnimationFrameMessage,
                                kSelectionAnimationFrameMessage, PM_REMOVE))
            {
            }
        }
    }

    void Overlay::StopSelectionAnimation() noexcept
    {
        if (m_animatingSelection)
        {
            m_animatingSelection = false;
            StopAnimationClock();
        }
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

        if (!m_hasSelection || !PtInRect(&m_selection, point))
        {
            return HitTarget::Canvas;
        }
        if (m_mode == OperationMode::TextRecognition)
        {
            const RECT track = OcrScrollTrack();
            return PtInRect(&track, point) ? HitTarget::OcrScrollbar : HitTarget::OcrText;
        }
        if (!m_smart && HitTestSelection(m_selection, point, m_handleSize) != SelectionPart::None)
        {
            return HitTarget::SelectionHandle;
        }
        return HitTarget::Selection;
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
        m_toolbarVisible = LayoutToolbar(m_toolbar, m_selection, ClientBounds(), m_dpi);
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

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) return;
        AppendMenuW(menu, MF_STRING, 1, L"Copy");
        AppendMenuW(menu, MF_STRING, 2, L"Select all");
        SetForegroundWindow(m_window);
        POINT screen = point;
        ClientToScreen(m_window, &screen);
        const int command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            screen.x, screen.y, 0, m_window, nullptr);
        PostMessageW(m_window, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (command == 1)
        {
            CopyOcrText(false);
        }
        else if (command == 2)
        {
            m_ocrAnchor = 0;
            m_ocrCaret = m_ocrText.size();
            EnsureOcrCaretVisible();
            RequestRender();
        }
    }

    void Overlay::UpdateOcrSelection(POINT point)
    {
        m_ocrSelectionPoint = point;
        const RECT box = OcrTextBox();
        const LONG inset = static_cast<LONG>(Scaled(9));
        const LONG top = box.top + inset;
        const LONG bottom = box.bottom - inset;
        const LONG left = box.left + inset;
        const LONG right = box.right - inset - static_cast<LONG>(Scaled(kOcrScrollGutter));
        const LONG clampedY = std::clamp(point.y, top, std::max(top, bottom - 1));
        const LONG clampedX = std::clamp(point.x, left, std::max(left, right));

        if (point.y < top || point.y >= bottom)
        {
            const float distance = static_cast<float>(point.y < top ? top - point.y : point.y - bottom + 1);
            const float step = std::max(Scaled(1), distance * 0.22f);
            m_ocrScrollY = std::clamp(
                m_ocrScrollY + (point.y < top ? -step : step), 0.0f, OcrScrollMaximum());
        }
        m_ocrCaret = OcrPositionFromPoint(POINT{ clampedX, clampedY });
        RequestRender();
    }

    void Overlay::StopOcrSelection() noexcept
    {
        m_ocrSelecting = false;
        if (m_window != nullptr)
        {
            KillTimer(m_window, kOcrSelectionTimer);
        }
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

        if (m_ocrScrollDragging)
        {
            const RECT track = OcrScrollTrack();
            const RECT thumb = OcrScrollThumb();
            const float travel = static_cast<float>(track.bottom - track.top)
                - static_cast<float>(thumb.bottom - thumb.top);
            if (travel > 0.0f)
            {
                const float maximum = OcrScrollMaximum();
                const float pointerDelta = static_cast<float>(point.y - m_ocrScrollDragOrigin.y);
                m_ocrScrollY = std::clamp(
                    m_ocrScrollDragStart + pointerDelta * maximum / travel, 0.0f, maximum);
            }
            RequestRender();
            return;
        }
        if (m_ocrSelecting)
        {
            UpdateOcrSelection(point);
            return;
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
        SetCapture(m_window);
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
                return;
            case ToolbarHitTarget::None:
                break;
            }
        }

        if (m_mode == OperationMode::TextRecognition)
        {
            if (hit == HitTarget::OcrText || hit == HitTarget::OcrScrollbar)
            {
                SetFocus(m_window);
                const float scrollMaximum = OcrScrollMaximum();
                const RECT scrollTrack = OcrScrollTrack();
                if (scrollMaximum > 0.0f && PtInRect(&scrollTrack, point))
                {
                    const RECT scrollThumb = OcrScrollThumb();
                    if (PtInRect(&scrollThumb, point))
                    {
                        m_ocrScrollDragging = true;
                        m_ocrScrollDragOrigin = point;
                        m_ocrScrollDragStart = m_ocrScrollY;
                    }
                    else
                    {
                        const RECT box = OcrTextBox();
                        const float page = std::max(1.0f,
                            static_cast<float>(box.bottom - box.top) - Scaled(27));
                        m_ocrScrollY = std::clamp(
                            m_ocrScrollY + (point.y < scrollThumb.top ? -page : page),
                            0.0f, scrollMaximum);
                        RequestRender();
                    }
                    return;
                }
                m_ocrCaret = OcrPositionFromPoint(point);
                m_ocrAnchor = m_ocrCaret;
                EnsureOcrCaretVisible();
                m_ocrSelecting = true;
                m_ocrSelectionPoint = point;
                SetTimer(m_window, kOcrSelectionTimer, kOcrSelectionTimerMs, nullptr);
                RequestRender();
            }
            return;
        }

        // From here on the pointer is the user's, not the suggestion's. Leaving
        // this set would let the next mouse move quietly replace a rectangle the
        // user had just drawn with whatever happens to be under the cursor.
        const bool wasFollowing = m_smart;
        StopSelectionAnimation();
        m_displaySelection = m_selection;
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
        if (GetCapture() == m_window)
        {
            ReleaseCapture();
        }

        if (m_ocrScrollDragging)
        {
            m_ocrScrollDragging = false;
            RequestRender();
            return;
        }
        if (m_ocrSelecting)
        {
            UpdateOcrSelection(point);
            StopOcrSelection();
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
            // The press never turned into a drag, so it was a click: the pointer
            // was sitting on a suggestion and the user said yes to it.
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
                // A click that drew nothing: go back to following the pointer
                // rather than leaving a sliver behind.
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
                UINT lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                const RECT box = OcrTextBox();
                const float page = std::max(1.0f, static_cast<float>(box.bottom - box.top)
                    - Scaled(18));
                const float lineHeight = Scaled(static_cast<float>(kTextSize + 3));
                const float amount = lines == WHEEL_PAGESCROLL
                    ? page * static_cast<float>(delta) / WHEEL_DELTA
                    : lineHeight * static_cast<float>(lines) * static_cast<float>(delta) / WHEEL_DELTA;
                m_ocrScrollY = std::clamp(m_ocrScrollY - amount, 0.0f, OcrScrollMaximum());
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

        // Each wheel message is one step; magnitude is intentionally ignored.
        // Level 0 is the innermost element under the pointer; up widens the
        // suggestion and down narrows it.
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
            m_ocrSelecting = false;
            m_ocrScrollDragging = false;
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

    void Overlay::EnterTextRecognitionMode()
    {
        if (!m_hasSelection)
        {
            return;
        }

        if (!m_ocrCacheValid || !EqualRect(&m_ocrCacheSelection, &m_selection))
        {
            m_ocrText = L"AGShot text recognition preview\r\n\r\nThis is mock OCR content for the selected area. Select part of this text with the mouse or keyboard, then copy it.\r\n\r\nThe recognized text stays cached while the selection remains unchanged.";
            m_ocrCacheSelection = m_selection;
            m_ocrCacheValid = true;
            m_ocrCaret = 0;
            m_ocrAnchor = 0;
            m_ocrScrollY = 0.0f;
        }

        m_mode = OperationMode::TextRecognition;
        m_smart = false;
        m_ocrSelecting = false;
        m_ocrScrollDragging = false;
        SetFocus(m_window);
        RequestRender();
    }

    void Overlay::CopyOcrText(bool closeAfterCopy)
    {
        const std::size_t first = std::min(m_ocrAnchor, m_ocrCaret);
        const std::size_t last = std::max(m_ocrAnchor, m_ocrCaret);
        const std::wstring text = first == last
            ? m_ocrText
            : m_ocrText.substr(first, last - first);

        if (!text.empty() && OpenClipboard(m_window))
        {
            if (EmptyClipboard())
            {
                const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
                HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (memory != nullptr)
                {
                    void* destination = GlobalLock(memory);
                    if (destination != nullptr)
                    {
                        std::memcpy(destination, text.c_str(), bytes);
                        GlobalUnlock(memory);
                        if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
                        {
                            GlobalFree(memory);
                        }
                    }
                    else
                    {
                        GlobalFree(memory);
                    }
                }
            }
            CloseClipboard();
        }

        if (closeAfterCopy)
        {
            Cancel();
        }
    }

    // TODO(editbox): the text-recognition box is a hand-rolled edit control.
    // Layout, hit testing, caret placement, selection painting, scrolling and
    // keyboard navigation are all maintained in this file. Evaluate building it
    // on ITextServices (the RichEdit text services object) instead, which would
    // supply the editing surface, caret, selection, scrolling, IME and
    // accessibility, and would own its own mouse and keyboard input rather than
    // routing everything through this window's message handlers.
    RECT Overlay::OcrTextBox() const noexcept
    {
        return m_selection;
    }

    RECT Overlay::OcrScrollTrack() const noexcept
    {
        const RECT box = OcrTextBox();
        const LONG width = box.right - box.left;
        const LONG height = box.bottom - box.top;
        const LONG verticalInset = std::min<LONG>(MulDiv(9, m_dpi, 96),
            std::max<LONG>(0, (height - 1) / 2));
        const LONG right = box.right - std::min<LONG>(MulDiv(kOcrScrollInset, m_dpi, 96),
            std::max<LONG>(0, width - 1));
        const LONG left = std::max(box.left, right - MulDiv(kOcrScrollWidth, m_dpi, 96));
        return RECT{ left, box.top + verticalInset, right, box.bottom - verticalInset };
    }

    RECT Overlay::OcrScrollThumb() const
    {
        const float maximum = OcrScrollMaximum();
        if (maximum <= 0.0f)
        {
            return RECT{};
        }

        const RECT track = OcrScrollTrack();
        const float trackHeight = static_cast<float>(track.bottom - track.top);
        const float viewportHeight = std::max(1.0f, trackHeight);
        const float contentHeight = viewportHeight + maximum;
        const float thumbHeight = std::min(trackHeight, std::max(Scaled(18),
            trackHeight * viewportHeight / contentHeight));
        const float thumbTop = track.top + (trackHeight - thumbHeight)
            * m_ocrScrollY / maximum;
        return RECT{ track.left, static_cast<LONG>(thumbTop), track.right,
                     static_cast<LONG>(thumbTop + thumbHeight) };
    }

    winrt::com_ptr<IDWriteTextLayout> Overlay::CreateOcrTextLayout() const
    {
        const RECT box = OcrTextBox();
        const float padding = Scaled(9);
        const float width = std::max(1.0f, static_cast<float>(box.right - box.left)
            - padding * 2.0f - Scaled(kOcrScrollGutter));
        winrt::com_ptr<IDWriteTextLayout> layout;
        AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextLayout(
            m_ocrText.c_str(), static_cast<UINT32>(m_ocrText.size()),
            m_textFormat.get(), width, 100000.0f, layout.put()));
        AGSHOT_CHECK_HR(layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP));
        return layout;
    }

    std::size_t Overlay::OcrPositionFromPoint(POINT point) const
    {
        if (m_ocrText.empty())
        {
            return 0;
        }

        const RECT box = OcrTextBox();
        const float padding = Scaled(9);
        const LONG textLeft = box.left + static_cast<LONG>(padding);
        const LONG textRight = box.right - static_cast<LONG>(padding + Scaled(kOcrScrollGutter));
        const LONG textTop = box.top + static_cast<LONG>(padding);
        const LONG textBottom = box.bottom - static_cast<LONG>(padding);
        const float x = std::max(0.0f, static_cast<float>(
            std::clamp(point.x, textLeft, std::max(textLeft, textRight)) - box.left) - padding);
        const float y = std::max(0.0f, static_cast<float>(
            std::clamp(point.y, textTop, std::max(textTop, textBottom - 1)) - box.top)
            - padding + m_ocrScrollY);
        auto layout = CreateOcrTextLayout();
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        if (SUCCEEDED(layout->HitTestPoint(x, y, &trailing, &inside, &metrics)))
        {
            const std::size_t position = metrics.textPosition + (trailing ? metrics.length : 0);
            return std::min(position, m_ocrText.size());
        }
        return m_ocrText.size();
    }

    float Overlay::OcrScrollMaximum() const
    {
        if (m_ocrText.empty())
        {
            return 0.0f;
        }
        auto layout = CreateOcrTextLayout();
        DWRITE_TEXT_METRICS metrics{};
        AGSHOT_CHECK_HR(layout->GetMetrics(&metrics));
        const RECT box = OcrTextBox();
        const float viewportHeight = std::max(1.0f,
            static_cast<float>(box.bottom - box.top) - Scaled(18));
        return std::max(0.0f, metrics.height - viewportHeight);
    }

    void Overlay::EnsureOcrCaretVisible()
    {
        auto layout = CreateOcrTextLayout();
        FLOAT x = 0.0f;
        FLOAT y = 0.0f;
        DWRITE_HIT_TEST_METRICS caret{};
        if (FAILED(layout->HitTestTextPosition(static_cast<UINT32>(m_ocrCaret), FALSE,
                                                &x, &y, &caret)))
        {
            return;
        }

        const RECT box = OcrTextBox();
        const float viewportHeight = std::max(1.0f,
            static_cast<float>(box.bottom - box.top) - Scaled(18));
        if (y < m_ocrScrollY)
        {
            m_ocrScrollY = y;
        }
        else if (y + caret.height > m_ocrScrollY + viewportHeight)
        {
            m_ocrScrollY = y + caret.height - viewportHeight;
        }
        m_ocrScrollY = std::clamp(m_ocrScrollY, 0.0f, OcrScrollMaximum());
    }

    void Overlay::MoveOcrCaret(WPARAM key)
    {
        const std::size_t length = m_ocrText.size();
        const bool extend = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        std::size_t next = m_ocrCaret;
        if (key == VK_LEFT || key == VK_RIGHT)
        {
            if (!extend && m_ocrAnchor != m_ocrCaret)
            {
                next = key == VK_LEFT ? std::min(m_ocrAnchor, m_ocrCaret)
                                      : std::max(m_ocrAnchor, m_ocrCaret);
            }
            else if (key == VK_LEFT && next > 0)
            {
                --next;
            }
            else if (key == VK_RIGHT && next < length)
            {
                ++next;
            }
        }
        else if (key == VK_HOME || key == VK_END)
        {
            const std::size_t lineStart = m_ocrText.rfind(L'\n', next == 0 ? 0 : next - 1);
            const std::size_t lineEnd = m_ocrText.find(L'\n', next);
            next = key == VK_HOME
                ? (lineStart == std::wstring::npos ? 0 : lineStart + 1)
                : (lineEnd == std::wstring::npos ? length : lineEnd);
        }
        else if (key == VK_UP || key == VK_DOWN)
        {
            auto layout = CreateOcrTextLayout();
            FLOAT x = 0.0f;
            FLOAT y = 0.0f;
            DWRITE_HIT_TEST_METRICS caret{};
            if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(m_ocrCaret), FALSE,
                                                       &x, &y, &caret)))
            {
                const float targetY = y + (key == VK_UP ? -caret.height : caret.height);
                if (targetY >= 0.0f)
                {
                    BOOL trailing = FALSE;
                    BOOL inside = FALSE;
                    DWRITE_HIT_TEST_METRICS target{};
                    if (SUCCEEDED(layout->HitTestPoint(x, targetY, &trailing, &inside, &target)))
                    {
                        next = std::min<std::size_t>(
                            target.textPosition + (trailing ? target.length : 0), length);
                    }
                }
            }
        }

        m_ocrCaret = next;
        if (!extend)
        {
            m_ocrAnchor = next;
        }
        EnsureOcrCaretVisible();
        RequestRender();
    }

    void Overlay::ClearSurface()
    {
        if (!m_surface || m_width == 0 || m_height == 0)
        {
            return;
        }

        AGSHOT_CHECK_HR(m_surface->Trim(nullptr, 0));
        AGSHOT_CHECK_HR(m_composition->Commit());
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
            if (m_mode == OperationMode::TextRecognition)
            {
                DrawTextRecognition();
            }
            else
            {
                DrawSelection();
            }
        }
        if (m_toolbarVisible)
        {
            DrawToolbar();
            DrawTooltip();
        }
        const bool pointerOverToolbar = m_toolbarVisible
            && HitTestToolbarTarget(m_toolbar, m_pointer) != ToolbarHitTarget::None;
        if (m_mode == OperationMode::EditSelection && !pointerOverToolbar
            && (!m_toolbarVisible || (m_hasSelection && PtInRect(&m_selection, m_pointer))))
        {
            DrawPixelMagnifier();
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
        const RECT& s = m_smart ? m_displaySelection : m_selection;
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
        const RECT& selection = m_smart ? m_displaySelection : m_selection;
        RECT border = selection;
        const int half = static_cast<int>(stroke / 2.0f);
        InflateRect(&border, -half, -half);

        m_d2dContext->DrawRectangle(ToRectF(border), m_accent.get(), stroke);

        if (!m_smart)
        {
            for (const RECT& handle : HandleRects(selection, m_handleSize))
            {
                const D2D1_RECT_F box = ToRectF(handle);
                m_d2dContext->FillRectangle(box, m_handleFill.get());
                m_d2dContext->DrawRectangle(box, m_handleEdge.get(), 1.0f);
            }
        }

        DrawSizeLabel();
    }

    void Overlay::DrawTextRecognition()
    {
        const RECT box = OcrTextBox();
        m_d2dContext->FillRectangle(ToRectF(box), m_handleFill.get());
        m_d2dContext->DrawRectangle(ToRectF(box), m_handleEdge.get(), Scaled(1));

        const float padding = Scaled(9);
        // The render target works a DIP per pixel, so a fractional scroll offset
        // would land the text on half pixels and make it shimmer as it moves.
        // Snapping the origin to whole pixels keeps every glyph on the grid; the
        // scroll position itself stays fractional so the motion is still smooth.
        const D2D1_POINT_2F origin{
            std::round(box.left + padding),
            std::round(box.top + padding - m_ocrScrollY) };
        const D2D1_RECT_F clip = D2D1::RectF(
            static_cast<float>(box.left), static_cast<float>(box.top),
            static_cast<float>(box.right), static_cast<float>(box.bottom));
        m_d2dContext->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        auto layout = CreateOcrTextLayout();

        const std::size_t first = std::min(m_ocrAnchor, m_ocrCaret);
        const std::size_t last = std::max(m_ocrAnchor, m_ocrCaret);
        if (last > first)
        {
            std::vector<DWRITE_HIT_TEST_METRICS> metrics(m_ocrText.size() + 1);
            UINT32 count = 0;
            if (SUCCEEDED(layout->HitTestTextRange(
                    static_cast<UINT32>(first), static_cast<UINT32>(last - first),
                    origin.x, origin.y, metrics.data(), static_cast<UINT32>(metrics.size()), &count)))
            {
                for (UINT32 i = 0; i < count; ++i)
                {
                    const auto& item = metrics[i];
                    m_d2dContext->FillRectangle(
                        D2D1::RectF(item.left, item.top, item.left + item.width, item.top + item.height),
                        m_accent.get());
                }
            }
            DWRITE_TEXT_RANGE range{ static_cast<UINT32>(first), static_cast<UINT32>(last - first) };
            AGSHOT_CHECK_HR(layout->SetDrawingEffect(m_handleFill.get(), range));
        }
        m_d2dContext->DrawTextLayout(origin, layout.get(), m_handleEdge.get());

        FLOAT caretX = 0.0f;
        FLOAT caretY = 0.0f;
        DWRITE_HIT_TEST_METRICS caret{};
        if (SUCCEEDED(layout->HitTestTextPosition(
                static_cast<UINT32>(m_ocrCaret), FALSE, &caretX, &caretY, &caret)))
        {
            // Snapped like the text, so the caret stays a crisp single column
            // rather than being blurred across two by a fractional origin.
            const float caretLeft = std::round(origin.x + caretX);
            const D2D1_POINT_2F top{ caretLeft, std::round(origin.y + caretY) };
            const D2D1_POINT_2F bottom{ caretLeft, top.y + std::max(1.0f, std::round(caret.height)) };
            m_d2dContext->DrawLine(top, bottom, m_accent.get(), Scaled(1));
        }
        m_d2dContext->PopAxisAlignedClip();

        if (OcrScrollMaximum() > 0.0f)
        {
            const RECT track = OcrScrollTrack();
            const RECT thumb = OcrScrollThumb();
            const float radius = Scaled(kOcrScrollWidth) * 0.5f;
            m_d2dContext->FillRoundedRectangle(
                ToRoundedRect(track, radius), m_textScrollTrack.get());
            m_d2dContext->FillRoundedRectangle(
                ToRoundedRect(thumb, radius), m_textScrollThumb.get());
        }
    }

    void Overlay::DrawSizeLabel()
    {
        wchar_t text[64]{};
        const RECT& selection = m_smart ? m_displaySelection : m_selection;
        swprintf_s(text, L"%d x %d",
                   selection.right - selection.left,
                   selection.bottom - selection.top);

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

        const float gap = Scaled(3);
        const float centeredX = std::clamp(
            static_cast<float>(selection.left), 0.0f,
            std::max(0.0f, static_cast<float>(m_width) - width));
        const float centeredY = std::clamp(
            (static_cast<float>(selection.top + selection.bottom) - height) * 0.5f, 0.0f,
            std::max(0.0f, static_cast<float>(m_height) - height));
        const D2D1_RECT_F candidates[] =
        {
            D2D1::RectF(centeredX, static_cast<float>(selection.top) - gap - height,
                        centeredX + width, static_cast<float>(selection.top) - gap),
            D2D1::RectF(centeredX, static_cast<float>(selection.bottom) + gap,
                        centeredX + width, static_cast<float>(selection.bottom) + gap + height),
            D2D1::RectF(static_cast<float>(selection.right) + gap, centeredY,
                        static_cast<float>(selection.right) + gap + width, centeredY + height),
            D2D1::RectF(static_cast<float>(selection.left) - gap - width, centeredY,
                        static_cast<float>(selection.left) - gap, centeredY + height),
        };

        D2D1_RECT_F box{};
        bool found = false;
        for (const D2D1_RECT_F& candidate : candidates)
        {
            if (candidate.left < 0.0f || candidate.top < 0.0f
                || candidate.right > static_cast<float>(m_width)
                || candidate.bottom > static_cast<float>(m_height))
            {
                continue;
            }
            if (candidate.right <= selection.left || candidate.left >= selection.right
                || candidate.bottom <= selection.top || candidate.top >= selection.bottom)
            {
                box = candidate;
                found = true;
                break;
            }
        }
        if (!found)
        {
            const float maxX = std::max(0.0f, static_cast<float>(m_width) - width);
            const float maxY = std::max(0.0f, static_cast<float>(m_height) - height);
            box = D2D1::RectF(
                std::clamp(static_cast<float>(selection.left), 0.0f, maxX),
                std::clamp(static_cast<float>(selection.top) - gap - height, 0.0f, maxY),
                0.0f, 0.0f);
            box.right = box.left + width;
            box.bottom = box.top + height;
        }

        const float x = box.left;
        const float y = box.top;

        m_d2dContext->FillRoundedRectangle(
            D2D1::RoundedRect(box, Scaled(4), Scaled(4)), m_panel.get());
        m_d2dContext->DrawTextLayout(
            D2D1::Point2F(x + padding, y + padding * 0.5f), layout.get(), m_icon.get());
    }

    void Overlay::DrawPixelMagnifier()
    {
        if (!m_background || m_image.empty())
        {
            return;
        }

        constexpr int sampleCells = 11;
        const float cell = MeasuredPixels(18.0f);
        const float previewSize = sampleCells * cell;
        const float cardWidth = MeasuredPixels(210.0f);
        const float cardHeight = MeasuredPixels(291.0f);
        const float gap = MeasuredPixels(16.0f);
        const float margin = MeasuredPixels(6.0f);

        POINT samplePoint = m_pointer;
        if (m_dragMode == DragMode::MoveOrResize)
        {
            switch (m_dragPart)
            {
            case SelectionPart::Left:
            case SelectionPart::TopLeft:
            case SelectionPart::BottomLeft:
                samplePoint.x = m_selection.left;
                break;
            case SelectionPart::Right:
            case SelectionPart::TopRight:
            case SelectionPart::BottomRight:
                samplePoint.x = m_selection.right - 1;
                break;
            default:
                break;
            }
            switch (m_dragPart)
            {
            case SelectionPart::Top:
            case SelectionPart::TopLeft:
            case SelectionPart::TopRight:
                samplePoint.y = m_selection.top;
                break;
            case SelectionPart::Bottom:
            case SelectionPart::BottomLeft:
            case SelectionPart::BottomRight:
                samplePoint.y = m_selection.bottom - 1;
                break;
            default:
                break;
            }
        }
        const int sampleX = std::clamp(static_cast<int>(samplePoint.x), 0, m_image.width() - 1);
        const int sampleY = std::clamp(static_cast<int>(samplePoint.y), 0, m_image.height() - 1);
        const int sourceLeft = std::clamp(sampleX - sampleCells / 2, 0,
            std::max(0, m_image.width() - sampleCells));
        const int sourceTop = std::clamp(sampleY - sampleCells / 2, 0,
            std::max(0, m_image.height() - sampleCells));
        const int selectedX = std::clamp(sampleX - sourceLeft, 0, sampleCells - 1);
        const int selectedY = std::clamp(sampleY - sourceTop, 0, sampleCells - 1);

        const auto* pixels = static_cast<const BYTE*>(m_image.pixels());
        const BYTE* color = pixels + sampleY * m_image.stride() + sampleX * 4;
        const BYTE blue = color[0];
        const BYTE green = color[1];
        const BYTE red = color[2];
        const float luminance = (red * 0.299f + green * 0.587f + blue * 0.114f) / 255.0f;

        float x = static_cast<float>(m_pointer.x) + gap;
        float y = static_cast<float>(m_pointer.y) + gap;
        if (x + cardWidth > static_cast<float>(m_width))
        {
            x = static_cast<float>(m_pointer.x) - gap - cardWidth;
        }
        if (y + cardHeight > static_cast<float>(m_height))
        {
            y = static_cast<float>(m_pointer.y) - gap - cardHeight;
        }
        x = std::clamp(x, 0.0f, std::max(0.0f, static_cast<float>(m_width) - cardWidth));
        y = std::clamp(y, 0.0f, std::max(0.0f, static_cast<float>(m_height) - cardHeight));

        const RECT card{ static_cast<LONG>(x), static_cast<LONG>(y),
                         static_cast<LONG>(x + cardWidth), static_cast<LONG>(y + cardHeight) };
        struct LayerPop
        {
            ID2D1DeviceContext* context{};
            ~LayerPop()
            {
                if (context != nullptr)
                {
                    context->PopLayer();
                }
            }
        } layerPop;
        if (m_dragMode == DragMode::NewRectangle || m_dragMode == DragMode::MoveOrResize)
        {
            const float layerExtent = MeasuredPixels(8.0f);
            const D2D1_RECT_F layerBounds = D2D1::RectF(
                x - layerExtent, y - layerExtent,
                x + cardWidth + layerExtent, y + cardHeight + layerExtent);
            const auto layer = D2D1::LayerParameters(
                layerBounds, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(), 0.55f);
            m_d2dContext->PushLayer(layer, m_magnifierLayer.get());
            layerPop.context = m_d2dContext.get();
        }

        for (int spread = 6; spread >= 1; --spread)
        {
            const float extent = MeasuredPixels(static_cast<float>(spread));
            const D2D1_RECT_F shadowBounds = D2D1::RectF(
                x - extent, y - extent + MeasuredPixels(2.0f),
                x + cardWidth + extent, y + cardHeight + extent + MeasuredPixels(2.0f));
            const float opacity = 0.015f * static_cast<float>(7 - spread);
            m_shadow->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, opacity));
            m_d2dContext->FillRoundedRectangle(
                D2D1::RoundedRect(shadowBounds, MeasuredPixels(7.0f) + extent,
                                  MeasuredPixels(7.0f) + extent), m_shadow.get());
        }
        m_shadow->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.1f));
        m_d2dContext->FillRoundedRectangle(ToRoundedRect(card, MeasuredPixels(7.0f)), m_handleFill.get());
        m_d2dContext->DrawRoundedRectangle(
            ToRoundedRect(card, MeasuredPixels(7.0f)), m_panelEdge.get(), MeasuredPixels(1.0f));

        const float previewLeft = x + (cardWidth - previewSize) * 0.5f;
        const float previewTop = y + margin;
        const D2D1_RECT_F destination = D2D1::RectF(
            previewLeft, previewTop, previewLeft + previewSize, previewTop + previewSize);
        const D2D1_RECT_F source = D2D1::RectF(
            static_cast<float>(sourceLeft), static_cast<float>(sourceTop),
            static_cast<float>(sourceLeft + sampleCells), static_cast<float>(sourceTop + sampleCells));
        m_d2dContext->DrawBitmap(m_background.get(), &destination, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &source);

        const float selectedLeft = previewLeft + selectedX * cell;
        const float selectedTop = previewTop + selectedY * cell;
        const RECT selectedCell{ static_cast<LONG>(selectedLeft), static_cast<LONG>(selectedTop),
                                 static_cast<LONG>(selectedLeft + cell), static_cast<LONG>(selectedTop + cell) };
        m_d2dContext->DrawRectangle(
            ToRectF(selectedCell), m_handleFill.get(), MeasuredPixels(2.0f));
        RECT inner = selectedCell;
        InflateRect(&inner, -1, -1);
        m_d2dContext->DrawRectangle(
            ToRectF(inner), m_handleEdge.get(), MeasuredPixels(1.0f));

        wchar_t coordinates[64]{};
        swprintf_s(coordinates, L"X: %d Y: %d",
                   samplePoint.x + m_bounds.left, samplePoint.y + m_bounds.top);
        wchar_t hex[16]{};
        swprintf_s(hex, L"%02X%02X%02X", red, green, blue);

        const auto drawCenteredText = [this](const wchar_t* text, const D2D1_RECT_F& bounds,
                                              ID2D1SolidColorBrush* brush)
        {
            winrt::com_ptr<IDWriteTextLayout> layout;
            AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextLayout(
                text, static_cast<UINT32>(wcslen(text)), m_magnifierTextFormat.get(),
                bounds.right - bounds.left, bounds.bottom - bounds.top, layout.put()));
            AGSHOT_CHECK_HR(layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
            AGSHOT_CHECK_HR(layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
            m_d2dContext->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top),
                                         layout.get(), brush);
        };

        const float coordinatesTop = previewTop + previewSize + MeasuredPixels(3.0f);
        drawCenteredText(coordinates, D2D1::RectF(
            x + margin, coordinatesTop, x + cardWidth - margin,
            coordinatesTop + MeasuredPixels(34.0f)),
            m_handleEdge.get());

        const float swatchInset = MeasuredPixels(5.0f);
        const float swatchTop = coordinatesTop + MeasuredPixels(42.0f);
        const RECT swatch{ static_cast<LONG>(x + swatchInset), static_cast<LONG>(swatchTop),
                           static_cast<LONG>(x + cardWidth - swatchInset),
                           static_cast<LONG>(y + cardHeight - MeasuredPixels(6.0f)) };
        m_accent->SetColor(D2D1::ColorF(red / 255.0f, green / 255.0f, blue / 255.0f));
        m_d2dContext->FillRoundedRectangle(
            ToRoundedRect(swatch, MeasuredPixels(5.0f)), m_accent.get());
        drawCenteredText(hex, ToRectF(swatch), luminance > 0.58f ? m_handleEdge.get() : m_icon.get());
        m_accent->SetColor(D2D1::ColorF(0.18f, 0.49f, 0.94f, 1.0f));
    }

    void Overlay::DrawToolbar()
    {
        const float radius = Scaled(kCornerRadius);

        m_d2dContext->FillRoundedRectangle(ToRoundedRect(m_toolbar.panel, radius), m_panel.get());
        m_d2dContext->DrawRoundedRectangle(ToRoundedRect(m_toolbar.panel, radius), m_panelEdge.get(), 1.0f);

        const float gripX = static_cast<float>(m_toolbar.handle.left + m_toolbar.handle.right) * 0.5f;
        const float gripY = static_cast<float>(m_toolbar.handle.top + m_toolbar.handle.bottom) * 0.5f;
        const float dotRadius = Scaled(1.25f);
        for (int row = -1; row <= 1; ++row)
        {
            for (int column = -1; column <= 1; column += 2)
            {
                const D2D1_POINT_2F center{
                    gripX + Scaled(static_cast<float>(column * 3)),
                    gripY + Scaled(static_cast<float>(row * 4)) };
                m_d2dContext->FillEllipse(
                    D2D1::Ellipse(center, dotRadius, dotRadius), m_icon.get());
            }
        }

        const bool editActive = m_mode == OperationMode::EditSelection;
        const bool textActive = m_mode == OperationMode::TextRecognition;
        const auto modeButton = [this](const RECT& rect, bool active, bool hovered)
        {
            if (active)
            {
                m_d2dContext->FillRoundedRectangle(
                    ToRoundedRect(rect, Scaled(kButtonRadius)), m_accent.get());
            }
            else if (hovered)
            {
                m_d2dContext->FillRoundedRectangle(
                    ToRoundedRect(rect, Scaled(kButtonRadius)), m_hover.get());
            }
        };
        modeButton(m_toolbar.editSelection, editActive,
                   m_toolbar.hovered == ToolbarButton::EditSelection);
        modeButton(m_toolbar.textRecognition, textActive,
                   m_toolbar.hovered == ToolbarButton::TextRecognition);
        m_d2dContext->FillRectangle(ToRectF(m_toolbar.divider), m_panelEdge.get());

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
        DrawIconEditSelection(Inset(m_toolbar.editSelection, inset),
                              editActive ? m_handleFill.get() : m_icon.get());
        DrawIconTextRecognition(Inset(m_toolbar.textRecognition, inset),
                                textActive ? m_handleFill.get() : m_icon.get());
        DrawIconCopy(Inset(m_toolbar.copy, inset));
        DrawIconCancel(Inset(m_toolbar.cancel, inset));
    }

#ifdef AGSHOT_TOOLBAR_PROOF
    bool Overlay::ExportToolbarProof(const wchar_t* path, int dpi, bool textRecognitionActive,
                                     float ocrScrollProbe)
    {
        m_ocrScrollProbe = ocrScrollProbe;
        m_dpi = dpi;
        m_handleSize = MulDiv(kHandleSize, m_dpi, 96);
        m_mode = textRecognitionActive ? OperationMode::TextRecognition
                                       : OperationMode::EditSelection;

        if (!m_d2dFactory)
        {
            CreateDeviceIndependentResources();
        }
        if (!m_d2dContext)
        {
            winrt::com_ptr<ID3D11Device> device;
            HRESULT hr = E_FAIL;
            for (D3D_DRIVER_TYPE driver : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP })
            {
                hr = D3D11CreateDevice(nullptr, driver, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED,
                    nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
                if (SUCCEEDED(hr))
                {
                    break;
                }
            }
            AGSHOT_CHECK_HR(hr);
            AGSHOT_CHECK_HR(device->QueryInterface(__uuidof(IDXGIDevice), m_dxgiDevice.put_void()));
            AGSHOT_CHECK_HR(m_d2dFactory->CreateDevice(m_dxgiDevice.get(), m_d2dDevice.put()));
            AGSHOT_CHECK_HR(m_d2dDevice->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.put()));
        }

        // Small enough that the mock text overflows and the scrollbar appears.
        const RECT selection = textRecognitionActive ? RECT{ 0, 0, 360, 240 }
                                                     : RECT{ 0, 0, 600, 400 };
        const RECT bounds{ 0, 0, 1920, 1080 };
        m_selection = selection;
        if (!LayoutToolbar(m_toolbar, selection, bounds, m_dpi))
        {
            return false;
        }
        m_toolbarVisible = true;

        if (textRecognitionActive)
        {
            m_ocrText = L"AGShot text recognition preview\r\n\r\nThis is mock OCR content for the selected area. Select part of this text with the mouse or keyboard, then copy it.\r\n\r\nThe recognized text stays cached while the selection remains unchanged.";
            m_ocrCacheValid = true;
            m_ocrCacheSelection = selection;
            // A sub-pixel scroll offset, so the render proves the text lands on
            // whole pixels rather than on softened half ones.
            m_ocrScrollY = m_ocrScrollProbe;
        }

        // The mark geometry depends on the DPI, and the normal startup path is
        // not the one that runs here.
        const float localStroke =
            Scaled(kIconStroke) * 18.0f / (std::max(1.0f, Scaled(kIconInk)) - Scaled(kIconStroke));
        if (m_copyLocalStroke != localStroke)
        {
            CreateCopyIconGeometry(localStroke);
        }

        const RECT area = textRecognitionActive
            ? RECT{ selection.left - 16, selection.top - 16,
                    selection.right + 16, selection.bottom + 16 }
            : RECT{ m_toolbar.panel.left - 16, m_toolbar.panel.top - 16,
                    m_toolbar.panel.right + 16, m_toolbar.panel.bottom + 16 };
        const UINT width = static_cast<UINT>(area.right - area.left);
        const UINT height = static_cast<UINT>(area.bottom - area.top);
        const D2D1_SIZE_U size = D2D1::SizeU(width, height);
        const UINT stride = width * 4;
        const UINT bytes = stride * height;

        winrt::com_ptr<ID2D1DeviceContext> target;
        AGSHOT_CHECK_HR(m_d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, target.put()));

        const D2D1_BITMAP_PROPERTIES1 targetProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(m_dpi), static_cast<float>(m_dpi));
        winrt::com_ptr<ID2D1Bitmap1> surface;
        AGSHOT_CHECK_HR(target->CreateBitmap(size, nullptr, 0, targetProperties, surface.put()));
        target->SetTarget(surface.get());

        const D2D1_BITMAP_PROPERTIES1 readProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(m_dpi), static_cast<float>(m_dpi));
        winrt::com_ptr<ID2D1Bitmap1> readable;
        AGSHOT_CHECK_HR(target->CreateBitmap(size, nullptr, 0, readProperties, readable.put()));

        winrt::com_ptr<ID2D1DeviceContext> saved = m_d2dContext;
        m_d2dContext = target;
        m_d2dContext->BeginDraw();
        m_d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const auto brush = [this](float r, float g, float b, float a,
                                  winrt::com_ptr<ID2D1SolidColorBrush>& out)
        {
            AGSHOT_CHECK_HR(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), out.put()));
        };
        brush(0.0f, 0.0f, 0.0f, 0.45f, m_dim);
        brush(0.0f, 0.0f, 0.0f, 0.1f, m_shadow);
        brush(0.18f, 0.49f, 0.94f, 1.0f, m_accent);
        brush(1.0f, 1.0f, 1.0f, 1.0f, m_handleFill);
        brush(0.10f, 0.10f, 0.12f, 1.0f, m_handleEdge);
        brush(0.14f, 0.15f, 0.18f, 0.97f, m_panel);
        brush(1.0f, 1.0f, 1.0f, 0.14f, m_panelEdge);
        brush(1.0f, 1.0f, 1.0f, 0.13f, m_hover);
        brush(0.96f, 0.97f, 0.99f, 1.0f, m_icon);
        brush(0.94f, 0.35f, 0.35f, 1.0f, m_cancel);    // the one destructive mark
        brush(0.90f, 0.91f, 0.93f, 1.0f, m_textScrollTrack);
        brush(0.48f, 0.51f, 0.56f, 1.0f, m_textScrollThumb);

        const D2D1_MATRIX_3X2_F shift = D2D1::Matrix3x2F::Translation(
            -static_cast<float>(area.left), -static_cast<float>(area.top));
        m_d2dContext->SetTransform(shift);
        if (textRecognitionActive)
        {
            // The text box and its scrollbar only exist in text-recognition mode,
            // so proving the scrollbar means proving this panel too.
            DrawTextRecognition();
        }
        DrawToolbar();
        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        AGSHOT_CHECK_HR(m_d2dContext->EndDraw());

        AGSHOT_CHECK_HR(readable->CopyFromBitmap(nullptr, surface.get(), nullptr));
        D2D1_MAPPED_RECT mapped{};
        AGSHOT_CHECK_HR(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped));

        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + bytes;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        bool ok = false;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"wb") == 0 && file != nullptr)
        {
            std::fwrite(&header, sizeof(header), 1, file);
            std::fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, file);
            for (UINT row = 0; row < height; ++row)
            {
                std::fwrite(mapped.bits + static_cast<size_t>(row) * mapped.pitch, 1, stride, file);
            }
            std::fclose(file);
            ok = true;
        }
        readable->Unmap();
        m_d2dContext = saved;
        return ok;
    }
#endif

    void Overlay::DrawIconEditSelection(const RECT& box, ID2D1SolidColorBrush* brush)
    {
        const float stroke = Scaled(kIconStroke);
        // Round caps reach half a stroke past each end, so the geometry is inset
        // by half of whatever the painted size leaves over.
        const float inset =
            (static_cast<float>(box.right - box.left) + stroke - Scaled(kIconInk)) * 0.5f;
        const float left = static_cast<float>(box.left) + inset;
        const float top = static_cast<float>(box.top) + inset;
        const float right = static_cast<float>(box.right) - inset;
        const float bottom = static_cast<float>(box.bottom) - inset;
        const float corner = std::min(Scaled(5), std::min(right - left, bottom - top) * 0.35f);
        const auto line = [this, brush, stroke](float x1, float y1, float x2, float y2)
        {
            m_d2dContext->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2),
                                   brush, stroke, m_roundStroke.get());
        };
        line(left, top + corner, left, top);
        line(left, top, left + corner, top);
        line(right - corner, top, right, top);
        line(right, top, right, top + corner);
        line(left, bottom - corner, left, bottom);
        line(left, bottom, left + corner, bottom);
        line(right - corner, bottom, right, bottom);
        line(right, bottom - corner, right, bottom);
    }

    void Overlay::DrawIconTextRecognition(const RECT& box, ID2D1SolidColorBrush* brush)
    {
        // The mark spans the same box the edit-selection mark does, inset by the
        // same amount and widened by its own stroke, so the two read as a pair
        // at every DPI instead of only where the rounding happens to agree.
        const float stroke = Scaled(kIconStroke);
        const TextRecognitionIcon icon =
            LayoutTextRecognitionIcon(box, Scaled(kIconInk), stroke);

        winrt::com_ptr<ID2D1PathGeometry> brackets;
        AGSHOT_CHECK_HR(m_d2dFactory->CreatePathGeometry(brackets.put()));
        winrt::com_ptr<ID2D1GeometrySink> sink;
        AGSHOT_CHECK_HR(brackets->Open(sink.put()));
        for (const auto& bracket : icon.brackets)
        {
            sink->BeginFigure(bracket.points[0], D2D1_FIGURE_BEGIN_HOLLOW);
            for (std::size_t i = 1; i < bracket.count; ++i)
            {
                sink->AddLine(bracket.points[i]);
            }
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        AGSHOT_CHECK_HR(sink->Close());

        // Flat ends: the reference art's bars are cut off square, and a round
        // cap would add half a stroke to each of the six bar ends.
        m_d2dContext->DrawGeometry(brackets.get(), brush, icon.stroke, m_flatStroke.get());
        for (int i = 0; i < 3; ++i)
        {
            m_d2dContext->DrawLine(
                icon.barFrom[i], icon.barTo[i], brush, icon.stroke, m_flatStroke.get());
        }
    }
    void Overlay::DrawIconCopy(const RECT& box)
    {
        D2D1_MATRIX_3X2_F original{};
        m_d2dContext->GetTransform(&original);
        // The mark is scaled so that the painted outline -- the 18-unit
        // geometry plus half a stroke on every side -- comes out at kIconInk,
        // the same visual size as the other three marks.
        const float boxSpan = static_cast<float>(box.right - box.left);
        const float scale = (Scaled(kIconInk) - Scaled(kIconStroke)) / 18.0f;
        const float scaleX = scale;
        const float scaleY = scale;
        const float offset = (boxSpan - Scaled(kIconInk) + Scaled(kIconStroke)) * 0.5f;
        const D2D1_MATRIX_3X2_F iconTransform =
            D2D1::Matrix3x2F::Scale(scaleX, scaleY)
            * D2D1::Matrix3x2F::Translation(
                static_cast<float>(box.left) + offset, static_cast<float>(box.top) + offset)
            * original;
        // The geometry is authored in an 18-unit square and scaled up to the
        // box, so the stroke is scaled back down to stay at kIconStroke DIPs
        // rather than growing with the icon.
        m_d2dContext->SetTransform(iconTransform);
        // The geometry already is the stroked area of both squares merged, so it
        // is filled straight, in a single pass.
        m_d2dContext->FillGeometry(m_copyIconGeometry.get(), m_icon.get());
        m_d2dContext->SetTransform(original);
    }

    void Overlay::DrawIconCancel(const RECT& box)
    {
        const float stroke = Scaled(kIconStroke);
        // A round cap is a semicircle, so it reaches stroke/2 past the endpoint
        // in every direction; the inset leaves exactly kIconInk of painted width.
        const float inset = (static_cast<float>(box.right - box.left)
                             + stroke - Scaled(kIconInk)) * 0.5f;

        const D2D1_POINT_2F topLeft{ static_cast<float>(box.left) + inset,
                                     static_cast<float>(box.top) + inset };
        const D2D1_POINT_2F bottomRight{ static_cast<float>(box.right) - inset,
                                         static_cast<float>(box.bottom) - inset };
        const D2D1_POINT_2F topRight{ static_cast<float>(box.right) - inset,
                                      static_cast<float>(box.top) + inset };
        const D2D1_POINT_2F bottomLeft{ static_cast<float>(box.left) + inset,
                                        static_cast<float>(box.bottom) - inset };

        m_d2dContext->DrawLine(topLeft, bottomRight, m_cancel.get(), stroke, m_roundStroke.get());
        m_d2dContext->DrawLine(topRight, bottomLeft, m_cancel.get(), stroke, m_roundStroke.get());
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
            : m_toolbar.hovered == ToolbarButton::Cancel
                ? m_toolbar.cancel
                : m_toolbar.hovered == ToolbarButton::EditSelection
                    ? m_toolbar.editSelection
                    : m_toolbar.textRecognition;

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
        case WM_DPICHANGED:
            m_dpi = HIWORD(wparam);
            if (m_dpi <= 0)
            {
                m_dpi = 96;
            }
            m_handleSize = MulDiv(kHandleSize, m_dpi, 96);
            if (m_textFormat)
            {
                m_textFormat = nullptr;
                AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextFormat(
                    L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    Scaled(kTextSize), L"en-US", m_textFormat.put()));
            }
            if (m_magnifierTextFormat)
            {
                m_magnifierTextFormat = nullptr;
                AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextFormat(
                    L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    Scaled(MeasuredPixels(18.0f)), L"en-US",
                    m_magnifierTextFormat.put()));
            }
            if (m_toolbarVisible)
            {
                Settle();
            }
            RequestRender();
            return 0;

        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED)
            {
                m_width = static_cast<UINT>(LOWORD(lparam));
                m_height = static_cast<UINT>(HIWORD(lparam));
                if (m_d2dContext && m_width > 0 && m_height > 0)
                {
                    CreateSizeDependentResources();
                    RequestRender();
                }
            }
            return 0;

        case WM_TIMER:
            if (wparam == kOcrSelectionTimer && m_ocrSelecting)
            {
                UpdateOcrSelection(m_ocrSelectionPoint);
                return 0;
            }
            break;

        case kSelectionAnimationFrameMessage:
        {
            if (m_animationFrameSignal)
            {
                std::lock_guard lock(m_animationFrameSignal->mutex);
                m_animationFrameSignal->posted = false;
            }
            if (m_animatingSelection)
            {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                AdvanceSelectionAnimation(now.QuadPart);
            }
            Render();
            if (!m_animatingSelection)
            {
                StopAnimationClock();
            }
            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            RequestRender();
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

        // TODO(pointer): this mouse handling is still the legacy WM_MOUSE* /
        // WM_LBUTTON* family. The process already opts into mouse-in-pointer, and
        // the system only keeps posting these because nothing here answers
        // WM_POINTER, so move the whole hit and click path onto WM_POINTERDOWN /
        // WM_POINTERUP / WM_POINTERUPDATE and keep the click count here.
        // CS_DBLCLKS is not the answer: WM_LBUTTONDBLCLK is the only click-count
        // message Windows defines, so it cannot express a triple click at all,
        // and it puts the count in the OS where this code cannot see it.
        // Blocked on that, double click selects the word and triple click the
        // paragraph in the text-recognition box; both belong in the pointer
        // handler above, not on a second code path of their own.
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
            RequestRender();
            return 0;

        case WM_SETCURSOR:
        {
            LPCWSTR shape = m_mode == OperationMode::TextRecognition ? IDC_ARROW : IDC_CROSS;
            const HitTarget hit = HitTest(m_pointer);
            if (m_dragMode == DragMode::MoveOrResize)
            {
                shape = CursorForPart(m_dragPart);
            }
            else if (m_dragMode == DragMode::ToolbarMove || hit == HitTarget::ToolbarHandle)
            {
                shape = IDC_SIZEALL;
            }
            else if (hit == HitTarget::ToolbarButton)
            {
                shape = IDC_HAND;
            }
            else if (hit == HitTarget::ToolbarPanel || hit == HitTarget::OcrScrollbar)
            {
                shape = IDC_ARROW;
            }
            else if (hit == HitTarget::OcrText)
            {
                shape = IDC_IBEAM;
            }
            else if (hit == HitTarget::SelectionHandle || hit == HitTarget::Selection)
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
            if (m_mode == OperationMode::TextRecognition)
            {
                if (wparam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000))
                {
                    // Ctrl+C is a done-with-this gesture: it takes the text and
                    // closes, the same as pressing the copy button.
                    CopyOcrText(true);
                    return 0;
                }
                if (wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
                {
                    m_ocrAnchor = 0;
                    m_ocrCaret = m_ocrText.size();
                    EnsureOcrCaretVisible();
                    RequestRender();
                    return 0;
                }
                if (wparam == VK_LEFT || wparam == VK_UP
                    || wparam == VK_RIGHT || wparam == VK_DOWN
                    || wparam == VK_HOME || wparam == VK_END)
                {
                    MoveOcrCaret(wparam);
                    return 0;
                }
                if (wparam == VK_PRIOR || wparam == VK_NEXT)
                {
                    const RECT box = OcrTextBox();
                    const float page = std::max(1.0f,
                        static_cast<float>(box.bottom - box.top) - Scaled(27));
                    m_ocrScrollY = std::clamp(
                        m_ocrScrollY + (wparam == VK_NEXT ? page : -page),
                        0.0f, OcrScrollMaximum());
                    RequestRender();
                    return 0;
                }
                if (wparam == VK_RETURN)
                {
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
            StopOcrSelection();
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;

        case WM_DESTROY:
            StopOcrSelection();
            return 0;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
