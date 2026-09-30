#include "pch.h"
#include "resource.h"
#include "core/config.h"
#include "core/error.h"
#include "core/tray_icon.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <string_view>

using namespace winrt;

namespace
{
    constexpr wchar_t kWindowClass[] = L"AGShot.MainWindow";
    constexpr wchar_t kWindowTitle[] = L"AGShot";
    constexpr std::wstring_view kGreeting = L"Hello, World!";

    constexpr UINT kClientWidth = 800;
    constexpr UINT kClientHeight = 450;
    constexpr float kFontSizeDip = 48.0f;

    HWND g_hwnd{};

    com_ptr<IDXGIDevice> g_dxgiDevice;
    com_ptr<ID2D1Factory1> g_d2dFactory;
    com_ptr<ID2D1Device> g_d2dDevice;
    com_ptr<ID2D1DeviceContext> g_d2dContext;
    com_ptr<ID2D1SolidColorBrush> g_textBrush;
    com_ptr<IDWriteFactory> g_dwriteFactory;
    com_ptr<IDWriteTextFormat> g_textFormat;
    com_ptr<IDCompositionDevice> g_dcompDevice;
    com_ptr<IDCompositionTarget> g_dcompTarget;
    com_ptr<IDCompositionVisual> g_dcompVisual;
    com_ptr<IDCompositionVirtualSurface> g_virtualSurface;

    UINT g_width{};
    UINT g_height{};

    // Set once a failure has been reported. Rendering stops and the app quits:
    // a half-built composition tree cannot be recovered from.
    bool g_fatal{};

    // Icons owned by the process. The large one belongs to the main window; the
    // small one is shared with the tray icon and must outlive it.
    HICON g_mainIcon{};
    HICON g_trayIcon{};

    // What the tray icon asks of the main window. The tray itself, including its
    // hidden owner window and its menu, lives in core/tray_icon.cpp.
    void ActivateMainWindow()
    {
        if (g_hwnd == nullptr)
        {
            return;
        }
        ShowWindow(g_hwnd, SW_SHOW);
        ShowWindow(g_hwnd, SW_RESTORE);
        // The shell only grants this to the foreground process some of the time;
        // a refusal just means the window is raised without taking focus.
        SetForegroundWindow(g_hwnd);
    }

    void ExitFromTray()
    {
        if (g_hwnd != nullptr)
        {
            DestroyWindow(g_hwnd);
        }
    }

    void FatalFromTray()
    {
        g_fatal = true;
        PostQuitMessage(1);
    }

    void CreateDeviceIndependentResources()
    {
        AGSHOT_CHECK_HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2dFactory.put()));

        AGSHOT_CHECK_HR(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(g_dwriteFactory.put_void())));

        AGSHOT_CHECK_HR(g_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            kFontSizeDip,
            L"en-US",
            g_textFormat.put()));

        // Centre the greeting inside whatever layout rect Render() hands over.
        AGSHOT_CHECK_HR(g_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
        AGSHOT_CHECK_HR(g_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
    }

    void CreateD3DDevice()
    {
        //const D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
        const D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_WARP };

        HRESULT hr = E_FAIL;
        com_ptr<ID3D11Device> device;
        for (D3D_DRIVER_TYPE driver : drivers)
        {
            hr = D3D11CreateDevice(
                nullptr,
                driver,
                nullptr,
                //D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
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

        AGSHOT_CHECK_HR(device->QueryInterface(__uuidof(IDXGIDevice), g_dxgiDevice.put_void()));
    }

    void CreateDeviceResources()
    {
        CreateD3DDevice();

        AGSHOT_CHECK_HR(g_d2dFactory->CreateDevice(g_dxgiDevice.get(), g_d2dDevice.put()));
        AGSHOT_CHECK_HR(g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, g_d2dContext.put()));
        AGSHOT_CHECK_HR(g_d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.94f, 0.94f, 0.96f), g_textBrush.put()));

        AGSHOT_CHECK_HR(DCompositionCreateDevice(
            g_dxgiDevice.get(), __uuidof(IDCompositionDevice), g_dcompDevice.put_void()));
        AGSHOT_CHECK_HR(g_dcompDevice->CreateTargetForHwnd(g_hwnd, TRUE, g_dcompTarget.put()));
        AGSHOT_CHECK_HR(g_dcompDevice->CreateVisual(g_dcompVisual.put()));
    }

    // Creates the DirectComposition virtual surface on first use, then resizes it in
    // place. A virtual surface is sparsely allocated and resizable, so there is no
    // swap chain and no back buffer to release when the window changes size.
    void CreateSizeDependentResources()
    {
        if (!g_virtualSurface)
        {
            AGSHOT_CHECK_HR(g_dcompDevice->CreateVirtualSurface(
                g_width,
                g_height,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_PREMULTIPLIED,
                g_virtualSurface.put()));

            AGSHOT_CHECK_HR(g_dcompVisual->SetContent(g_virtualSurface.get()));
        }
        else
        {
            AGSHOT_CHECK_HR(g_virtualSurface->Resize(g_width, g_height));
        }

        AGSHOT_CHECK_HR(g_dcompDevice->Commit());
    }

    void Render()
    {
        if (!g_virtualSurface || !g_d2dContext || g_width == 0 || g_height == 0)
        {
            return;
        }

        // Ask DirectComposition for a drawable tile covering the whole surface.
        const RECT update = { 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };

        com_ptr<IDXGISurface> updateSurface;
        POINT updateOffset{};
        AGSHOT_CHECK_HR(g_virtualSurface->BeginDraw(
            &update, __uuidof(IDXGISurface), updateSurface.put_void(), &updateOffset));

        // Everything below is in DIPs; scale converts surface pixels to DIPs.
        const UINT dpi = GetDpiForWindow(g_hwnd);
        const float fDpi = static_cast<float>(dpi != 0 ? dpi : 96);
        const float scale = 96.0f / fDpi;

        g_d2dContext->SetDpi(fDpi, fDpi);

        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            fDpi,
            fDpi);

        com_ptr<ID2D1Bitmap1> tile;
        AGSHOT_CHECK_HR(g_d2dContext->CreateBitmapFromDxgiSurface(updateSurface.get(), &props, tile.put()));
        g_d2dContext->SetTarget(tile.get());

        const D2D1_RECT_F surfaceRect = D2D1::RectF(
            0.0f,
            0.0f,
            static_cast<float>(g_width) * scale,
            static_cast<float>(g_height) * scale);

        g_d2dContext->BeginDraw();

        // BeginDraw hands back a tile out of DirectComposition's atlas, not a view of
        // the surface itself, and updateOffset is where the surface's origin lands
        // inside that tile. Shift the surface coordinate space onto the tile.
        g_d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x) * scale,
            static_cast<float>(updateOffset.y) * scale));

        g_d2dContext->Clear(D2D1::ColorF(0.11f, 0.11f, 0.13f));

        // winuser.h defines DrawText as a macro (DrawText -> DrawTextW), so d2d1.h
        // already spelled ID2D1RenderTarget::DrawText as DrawTextW when it was parsed.
        // Calling it by that name is what the interface actually exposes.
        g_d2dContext->DrawTextW(
            kGreeting.data(),
            static_cast<UINT32>(kGreeting.size()),
            g_textFormat.get(),
            surfaceRect,
            g_textBrush.get());

        g_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        AGSHOT_CHECK_HR(g_d2dContext->EndDraw());

        // D2D has to let go of the tile before DirectComposition can complete the update.
        g_d2dContext->SetTarget(nullptr);
        tile = nullptr;

        AGSHOT_CHECK_HR(g_virtualSurface->EndDraw());

        // Surface updates only become visible once the composition tree is committed.
        AGSHOT_CHECK_HR(g_dcompDevice->Commit());
    }


    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED)
            {
                g_width = LOWORD(lparam);
                g_height = HIWORD(lparam);
                if (g_width > 0 && g_height > 0 && g_d2dContext)
                {
                    CreateSizeDependentResources();
                    Render();
                }
            }
            return 0;

        case WM_DPICHANGED:
        {
            // The process is Per-Monitor V2 aware (see AGShot.exe.manifest), so the
            // system supplies a DPI-appropriate size and position for the new monitor.
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            if (!SetWindowPos(
                    hwnd,
                    nullptr,
                    suggested->left,
                    suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE))
            {
                // The window keeps its old size, which is wrong on the new monitor
                // but perfectly recoverable by dragging it. Worth saying so; not
                // worth terminating over.
                agshot::ShowWarning(
                    agshot::DescribeWin32(L"SetWindowPos", GetLastError(), __FILEW__, __LINE__),
                    L"AGShot could not resize itself for the new display scaling.");
            }
            return 0;
        }

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

        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE)
            {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // DispatchMessageW has no handler above it, so an exception escaping a
    // message handler would not unwind: the CRT would call abort() and the user
    // would get a runtime dialog saying nothing useful. Catch it here instead.
    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (g_fatal)
        {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        try
        {
            return HandleMessage(hwnd, message, wparam, lparam);
        }
        catch (...)
        {
            // Report once only: the dialog pumps messages, so a failing repaint
            // would otherwise stack up one dialog per paint.
            if (!g_fatal)
            {
                g_fatal = true;
                agshot::ReportCurrentException(__FILEW__, __LINE__);
                PostQuitMessage(1);
            }
            return 0;
        }
    }
}

int RunApp(HINSTANCE instance, int showCommand)
{
    EnableMouseInPointer(true);

    init_apartment(apartment_type::single_threaded);

    // Settings live outside the build output so they survive a rebuild. The
    // first run writes the file out; later runs read whatever is there.
    const auto configPath = agshot::ConfigPath();
    const auto config = agshot::LoadConfig(configPath);

    // Shown at the bottom of every report, so it is obvious which file was in play.
    agshot::SetDiagnosticContext(L"Config: " + configPath.wstring());

    if (!config.exists || config.migrated)
    {
        try
        {
            agshot::SaveConfig(config.config, configPath);
        }
        catch (const std::exception& e)
        {
            // Not fatal, but settings silently failing to save is exactly the
            // kind of thing that wastes an afternoon.
            auto warning = agshot::DescribeException(e, __FILEW__, __LINE__);
            warning.detail = configPath.wstring() + L"\n\n" + warning.detail;
            agshot::ShowWarning(std::move(warning), L"AGShot could not save its settings file.");
        }
    }
    else if (!config.ok)
    {
        agshot::ShowWarning(
            agshot::Failure{ {}, configPath.wstring() + L"\n" + config.error },
            L"AGShot could not read its settings file, so it is using the defaults.");
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    // Take the exact size the shell asks for out of the multi-size .ico, so the
    // taskbar and title bar get crisp pixels instead of a rescaled 32px frame.
    g_mainIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_AGSHOT), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    AGSHOT_CHECK_WIN32(g_mainIcon != nullptr);
    wc.hIcon = g_mainIcon;
    // The small size is shared with the tray, so load it once and keep it.
    g_trayIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_AGSHOT), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    AGSHOT_CHECK_WIN32(g_trayIcon != nullptr);
    wc.hIconSm = g_trayIcon;
    wc.lpszClassName = kWindowClass;
    AGSHOT_CHECK_WIN32(RegisterClassExW(&wc) != 0);

    // Size the window so its client area is kClientWidth x kClientHeight at the
    // primary monitor DPI, then centre it on the work area.
    const UINT dpi = GetDpiForSystem();
    RECT rc{ 0, 0, MulDiv(kClientWidth, dpi, 96), MulDiv(kClientHeight, dpi, 96) };

    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP;

    AGSHOT_CHECK_WIN32(AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, dpi));

    RECT workArea{};
    AGSHOT_CHECK_WIN32(SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0));

    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;

    g_hwnd = CreateWindowExW(
        exStyle, kWindowClass, kWindowTitle, style,
        x, y, width, height, nullptr, nullptr, instance, nullptr);
    AGSHOT_CHECK_WIN32(g_hwnd != nullptr);

    CreateDeviceIndependentResources();
    CreateDeviceResources();

    RECT client{};
    AGSHOT_CHECK_WIN32(GetClientRect(g_hwnd, &client));
    g_width = static_cast<UINT>(client.right - client.left);
    g_height = static_cast<UINT>(client.bottom - client.top);
    CreateSizeDependentResources();

    // The virtual surface is the visual's content; show the composed result.
    AGSHOT_CHECK_HR(g_dcompTarget->SetRoot(g_dcompVisual.get()));
    AGSHOT_CHECK_HR(g_dcompDevice->Commit());
    Render();

    ShowWindow(g_hwnd, showCommand == 0 ? SW_SHOWDEFAULT : showCommand);
    AGSHOT_CHECK_WIN32(UpdateWindow(g_hwnd));

    const agshot::TrayCallbacks trayCallbacks{ ActivateMainWindow, ExitFromTray, FatalFromTray };
    if (!agshot::StartTrayIcon(instance, g_trayIcon, trayCallbacks))
    {
        // Not fatal: the window still works, but say so rather than leaving the
        // user wondering why the icon never showed up.
        agshot::ShowWarning(
            agshot::DescribeWin32(L"StartTrayIcon", GetLastError(), __FILEW__, __LINE__),
            L"AGShot could not add its notification-area icon.");
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // The loop can also end through the fatal path, where WM_DESTROY never ran.
    agshot::StopTrayIcon();
    if (g_trayIcon != nullptr)
    {
        DestroyIcon(g_trayIcon);
        g_trayIcon = nullptr;
    }
    if (g_mainIcon != nullptr)
    {
        DestroyIcon(g_mainIcon);
        g_mainIcon = nullptr;
    }

    return static_cast<int>(msg.wParam);
}

int __stdcall wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
    // The one place that catches everything escaping startup or the message
    // loop, so no failure can leave a silently dead process behind.
    try
    {
        return RunApp(instance, showCommand);
    }
    catch (...)
    {
        agshot::ReportCurrentException(__FILEW__, __LINE__);
        return 1;
    }
}
