#include "pch.h"

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

    void CheckWin32(BOOL ok)
    {
        if (!ok)
        {
            check_hresult(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    void CreateDeviceIndependentResources()
    {
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2dFactory.put()));

        check_hresult(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(g_dwriteFactory.put_void())));

        check_hresult(g_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            kFontSizeDip,
            L"en-US",
            g_textFormat.put()));

        // Centre the greeting inside whatever layout rect Render() hands over.
        check_hresult(g_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
        check_hresult(g_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
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
        check_hresult(hr);

        check_hresult(device->QueryInterface(__uuidof(IDXGIDevice), g_dxgiDevice.put_void()));
    }

    void CreateDeviceResources()
    {
        CreateD3DDevice();

        check_hresult(g_d2dFactory->CreateDevice(g_dxgiDevice.get(), g_d2dDevice.put()));
        check_hresult(g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, g_d2dContext.put()));
        check_hresult(g_d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.94f, 0.94f, 0.96f), g_textBrush.put()));

        check_hresult(DCompositionCreateDevice(
            g_dxgiDevice.get(), __uuidof(IDCompositionDevice), g_dcompDevice.put_void()));
        check_hresult(g_dcompDevice->CreateTargetForHwnd(g_hwnd, TRUE, g_dcompTarget.put()));
        check_hresult(g_dcompDevice->CreateVisual(g_dcompVisual.put()));
    }

    // Creates the DirectComposition virtual surface on first use, then resizes it in
    // place. A virtual surface is sparsely allocated and resizable, so there is no
    // swap chain and no back buffer to release when the window changes size.
    void CreateSizeDependentResources()
    {
        if (!g_virtualSurface)
        {
            check_hresult(g_dcompDevice->CreateVirtualSurface(
                g_width,
                g_height,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_PREMULTIPLIED,
                g_virtualSurface.put()));

            check_hresult(g_dcompVisual->SetContent(g_virtualSurface.get()));
        }
        else
        {
            check_hresult(g_virtualSurface->Resize(g_width, g_height));
        }

        check_hresult(g_dcompDevice->Commit());
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
        check_hresult(g_virtualSurface->BeginDraw(
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
        check_hresult(g_d2dContext->CreateBitmapFromDxgiSurface(updateSurface.get(), &props, tile.put()));
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
        check_hresult(g_d2dContext->EndDraw());

        // D2D has to let go of the tile before DirectComposition can complete the update.
        g_d2dContext->SetTarget(nullptr);
        tile = nullptr;

        check_hresult(g_virtualSurface->EndDraw());

        // Surface updates only become visible once the composition tree is committed.
        check_hresult(g_dcompDevice->Commit());
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
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
            SetWindowPos(
                hwnd,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
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
}

int __stdcall wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
    EnableMouseInPointer(true);

    init_apartment(apartment_type::single_threaded);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    CheckWin32(RegisterClassExW(&wc) != 0);

    // Size the window so its client area is kClientWidth x kClientHeight at the
    // primary monitor DPI, then centre it on the work area.
    const UINT dpi = GetDpiForSystem();
    RECT rc{ 0, 0, MulDiv(kClientWidth, dpi, 96), MulDiv(kClientHeight, dpi, 96) };

    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP;

    CheckWin32(AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, dpi));

    RECT workArea{};
    CheckWin32(SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0));

    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;

    g_hwnd = CreateWindowExW(
        exStyle, kWindowClass, kWindowTitle, style,
        x, y, width, height, nullptr, nullptr, instance, nullptr);
    CheckWin32(g_hwnd != nullptr);

    CreateDeviceIndependentResources();
    CreateDeviceResources();

    RECT client{};
    CheckWin32(GetClientRect(g_hwnd, &client));
    g_width = static_cast<UINT>(client.right - client.left);
    g_height = static_cast<UINT>(client.bottom - client.top);
    CreateSizeDependentResources();

    // The virtual surface is the visual's content; show the composed result.
    check_hresult(g_dcompTarget->SetRoot(g_dcompVisual.get()));
    check_hresult(g_dcompDevice->Commit());
    Render();

    ShowWindow(g_hwnd, showCommand == 0 ? SW_SHOWDEFAULT : showCommand);
    CheckWin32(UpdateWindow(g_hwnd));

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
