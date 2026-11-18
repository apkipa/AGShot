#include "pch.h"

#include "hud/render_device.h"

#include "core/error.h"

#include <d3d11.h>

namespace agshot
{
    namespace
    {
        void CreateD3DAndD2DDevice(ID2D1Factory1* factory,
                                   winrt::com_ptr<IDXGIDevice>& dxgiDevice,
                                   winrt::com_ptr<ID2D1Device>& d2dDevice,
                                   winrt::com_ptr<ID2D1DeviceContext>& d2dContext,
                                   bool preventInternalThreading)
        {
            const D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
            if (preventInternalThreading)
            {
                flags |= D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
            }

            HRESULT hr = E_FAIL;
            winrt::com_ptr<ID3D11Device> device;
            for (D3D_DRIVER_TYPE driver : drivers)
            {
                hr = D3D11CreateDevice(
                    nullptr, driver, nullptr, flags, nullptr, 0,
                    D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
                if (SUCCEEDED(hr))
                {
                    break;
                }
            }
            AGSHOT_CHECK_HR(hr);
            AGSHOT_CHECK_HR(device->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void()));
            AGSHOT_CHECK_HR(factory->CreateDevice(dxgiDevice.get(), d2dDevice.put()));
            AGSHOT_CHECK_HR(d2dDevice->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext.put()));
        }
    }

    void RenderDevice::Initialize(HWND window, int dpi, UINT vramMsg)
    {
        m_window = window;
        m_dpi = dpi > 0 ? dpi : 96;
        m_vramMessage = vramMsg;

        CreateDeviceIndependentResources();
        CreateDeviceResources();
    }

    void RenderDevice::Destroy() noexcept
    {
        DiscardDeviceResources();
        m_roundStroke = nullptr;
        m_flatStroke = nullptr;
        m_textFormat = nullptr;
        m_magnifierTextFormat = nullptr;
        m_statsTextFormat = nullptr;
        m_dwriteFactory = nullptr;
        m_d2dFactory = nullptr;
        m_window = nullptr;
    }

    void RenderDevice::OnDpiChanged(int dpi)
    {
        m_dpi = dpi > 0 ? dpi : 96;
        CreateTextFormats();
    }

    void RenderDevice::CreateTextFormats()
    {
        if (!m_dwriteFactory)
        {
            return;
        }

        m_textFormat = nullptr;
        AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            ScaleByDpi(kHudTextSizeDip, m_dpi), L"en-US", m_textFormat.put()));

        m_magnifierTextFormat = nullptr;
        AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            ScaleByDpi(MeasuredPixelsForDpi(18.0f, m_dpi), m_dpi), L"en-US",
            m_magnifierTextFormat.put()));

        m_statsTextFormat = nullptr;
        AGSHOT_CHECK_HR(m_dwriteFactory->CreateTextFormat(
            L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            ScaleByDpi(11, m_dpi), L"en-US", m_statsTextFormat.put()));
    }

    void RenderDevice::CreateDeviceIndependentResources()
    {
        AGSHOT_CHECK_HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.put()));
        AGSHOT_CHECK_HR(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.put_void())));

        CreateTextFormats();

        const D2D1_STROKE_STYLE_PROPERTIES roundProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 1.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        AGSHOT_CHECK_HR(m_d2dFactory->CreateStrokeStyle(roundProps, nullptr, 0, m_roundStroke.put()));

        const D2D1_STROKE_STYLE_PROPERTIES flatProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
            D2D1_LINE_JOIN_ROUND, 1.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        AGSHOT_CHECK_HR(m_d2dFactory->CreateStrokeStyle(flatProps, nullptr, 0, m_flatStroke.put()));
    }

    void RenderDevice::CreateDeviceResources()
    {
        CreateD3DAndD2DDevice(m_d2dFactory.get(), m_dxgiDevice, m_d2dDevice, m_d2dContext, true);
        AGSHOT_CHECK_HR(m_d2dContext->CreateLayer(nullptr, m_magnifierLayer.put()));

        AGSHOT_CHECK_HR(DCompositionCreateDevice(
            m_dxgiDevice.get(), __uuidof(IDCompositionDevice), m_composition.put_void()));
        AGSHOT_CHECK_HR(m_composition->CreateTargetForHwnd(m_window, TRUE, m_target.put()));
        AGSHOT_CHECK_HR(m_composition->CreateVisual(m_visual.put()));

        CreateHudBrushes(m_d2dContext.get(), m_brushes);
        StartVramWatch();
    }

    bool RenderDevice::IsDeviceLost(HRESULT hr) noexcept
    {
        return hr == D2DERR_RECREATE_TARGET
            || hr == DXGI_ERROR_DEVICE_REMOVED
            || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DEVICE_HUNG
            || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }

    void RenderDevice::DiscardDeviceResources() noexcept
    {
        StopVramWatch();
        if (m_d2dContext)
        {
            m_d2dContext->SetTarget(nullptr);
        }

        m_background = nullptr;
        m_brushes.Release();
        m_magnifierLayer = nullptr;
        m_frameTile = nullptr;
        m_surface = nullptr;
        m_surfaceTrimmed = false;
        m_visual = nullptr;
        m_target = nullptr;
        m_composition = nullptr;
        m_d2dContext = nullptr;
        m_d2dDevice = nullptr;
        m_dxgiDevice = nullptr;
    }

    HRESULT RenderDevice::ResizeSurface(UINT width, UINT height)
    {
        if (width == 0 || height == 0)
        {
            return S_FALSE;
        }
        if (!m_composition || !m_target || !m_visual)
        {
            return E_UNEXPECTED;
        }

        HRESULT hr = S_OK;
        if (!m_surface)
        {
            hr = m_composition->CreateVirtualSurface(
                width, height,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_PREMULTIPLIED,
                m_surface.put());
            if (SUCCEEDED(hr))
            {
                hr = m_visual->SetContent(m_surface.get());
                if (FAILED(hr))
                {
                    m_surface = nullptr;
                }
            }
        }
        else
        {
            hr = m_surface->Resize(width, height);
        }

        if (FAILED(hr))
        {
            return hr;
        }

        hr = m_target->SetRoot(m_visual.get());
        if (FAILED(hr))
        {
            return hr;
        }
        m_surfaceTrimmed = false;
        return m_composition->Commit();
    }

    HRESULT RenderDevice::UpdateBackgroundBitmap(const ScreenImage& image)
    {
        if (!m_d2dContext || image.empty())
        {
            m_background = nullptr;
            return S_FALSE;
        }

        const UINT32 width = static_cast<UINT32>(image.width());
        const UINT32 height = static_cast<UINT32>(image.height());
        const UINT32 stride = static_cast<UINT32>(image.stride());

        if (m_background)
        {
            const D2D1_SIZE_U existing = m_background->GetPixelSize();
            if (existing.width == width && existing.height == height)
            {
                const HRESULT hr = m_background->CopyFromMemory(nullptr, image.pixels(), stride);
                if (SUCCEEDED(hr) || IsDeviceLost(hr))
                {
                    return hr;
                }
            }
            m_background = nullptr;
        }

        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f,
            96.0f);
        return m_d2dContext->CreateBitmap(
            D2D1::SizeU(width, height),
            image.pixels(),
            stride,
            properties,
            m_background.put());
    }

    HRESULT RenderDevice::Recover(UINT& outWidth, UINT& outHeight) noexcept
    {
        if (m_window == nullptr)
        {
            return E_UNEXPECTED;
        }

        const auto abandon = [this](HRESULT failure) noexcept
        {
            DiscardDeviceResources();
            return failure;
        };

        try
        {
            DiscardDeviceResources();

            RECT client{};
            if (GetClientRect(m_window, &client))
            {
                outWidth = static_cast<UINT>(client.right - client.left);
                outHeight = static_cast<UINT>(client.bottom - client.top);
            }

            CreateDeviceResources();

            const HRESULT hr = ResizeSurface(outWidth, outHeight);
            if (FAILED(hr))
            {
                return abandon(hr);
            }

            return S_OK;
        }
        catch (...)
        {
            return abandon(E_FAIL);
        }
    }

    HRESULT RenderDevice::BeginFrameDraw(UINT width, UINT height)
    {
        m_frameTile = nullptr;
        const RECT update{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        winrt::com_ptr<IDXGISurface> updateSurface;
        POINT offset{};
        HRESULT hr = m_surface->BeginDraw(
            &update, __uuidof(IDXGISurface), updateSurface.put_void(), &offset);
        if (FAILED(hr))
        {
            return hr;
        }

        m_d2dContext->SetDpi(96.0f, 96.0f);
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f);

        hr = m_d2dContext->CreateBitmapFromDxgiSurface(
            updateSurface.get(), &properties, m_frameTile.put());
        if (FAILED(hr))
        {
            m_surface->EndDraw();
            return hr;
        }

        m_d2dContext->SetTarget(m_frameTile.get());
        m_d2dContext->BeginDraw();
        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(offset.x), static_cast<float>(offset.y)));
        m_d2dContext->PushAxisAlignedClip(
            D2D1::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
            D2D1_ANTIALIAS_MODE_ALIASED);
        return S_OK;
    }

    void RenderDevice::CloseFrameDraw(HRESULT& outDrawHr, HRESULT& outSurfaceHr) noexcept
    {
        outDrawHr = S_OK;
        outSurfaceHr = S_OK;
        if (m_d2dContext)
        {
            m_d2dContext->PopAxisAlignedClip();
            m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
            outDrawHr = m_d2dContext->EndDraw();
            m_d2dContext->SetTarget(nullptr);
        }
        m_frameTile = nullptr;
        if (m_surface)
        {
            outSurfaceHr = m_surface->EndDraw();
        }
    }

    HRESULT RenderDevice::EndFrameDraw()
    {
        HRESULT drawHr = S_OK;
        HRESULT surfaceHr = S_OK;
        CloseFrameDraw(drawHr, surfaceHr);
        if (FAILED(drawHr))
        {
            return drawHr;
        }
        if (FAILED(surfaceHr))
        {
            return surfaceHr;
        }
        m_surfaceTrimmed = false;
        return m_composition->Commit();
    }

    void RenderDevice::AbortFrameDraw() noexcept
    {
        HRESULT drawHr = S_OK;
        HRESULT surfaceHr = S_OK;
        CloseFrameDraw(drawHr, surfaceHr);
        if (FAILED(drawHr) || FAILED(surfaceHr))
        {
            DiscardDeviceResources();
        }
    }

    VOID CALLBACK RenderDevice::VramWaitCallback(PVOID context, BOOLEAN)
    {
        auto* self = static_cast<RenderDevice*>(context);
        if (self != nullptr && self->m_window != nullptr && self->m_vramMessage != 0)
        {
            PostMessageW(self->m_window, self->m_vramMessage, 0, 0);
        }
    }

    void RenderDevice::StartVramWatch() noexcept
    {
        StopVramWatch();
        if (m_window == nullptr || !m_dxgiDevice)
        {
            return;
        }

        winrt::com_ptr<IDXGIAdapter> adapter;
        if (FAILED(m_dxgiDevice->GetAdapter(adapter.put())) || !adapter)
        {
            return;
        }
        m_dxgiAdapter3 = adapter.try_as<IDXGIAdapter3>();
        if (!m_dxgiAdapter3)
        {
            return;
        }

        m_vramBudgetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_vramBudgetEvent == nullptr)
        {
            m_dxgiAdapter3 = nullptr;
            return;
        }

        if (FAILED(m_dxgiAdapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(
                m_vramBudgetEvent, &m_vramBudgetCookie)))
        {
            CloseHandle(m_vramBudgetEvent);
            m_vramBudgetEvent = nullptr;
            m_dxgiAdapter3 = nullptr;
            return;
        }

        if (!RegisterWaitForSingleObject(
                &m_vramBudgetWait,
                m_vramBudgetEvent,
                &RenderDevice::VramWaitCallback,
                this,
                INFINITE,
                WT_EXECUTEDEFAULT))
        {
            m_dxgiAdapter3->UnregisterVideoMemoryBudgetChangeNotification(m_vramBudgetCookie);
            m_vramBudgetCookie = 0;
            CloseHandle(m_vramBudgetEvent);
            m_vramBudgetEvent = nullptr;
            m_dxgiAdapter3 = nullptr;
        }
    }

    void RenderDevice::StopVramWatch() noexcept
    {
        if (m_vramBudgetWait != nullptr)
        {
            UnregisterWaitEx(m_vramBudgetWait, INVALID_HANDLE_VALUE);
            m_vramBudgetWait = nullptr;
        }
        if (m_dxgiAdapter3 && m_vramBudgetCookie != 0)
        {
            m_dxgiAdapter3->UnregisterVideoMemoryBudgetChangeNotification(m_vramBudgetCookie);
            m_vramBudgetCookie = 0;
        }
        if (m_vramBudgetEvent != nullptr)
        {
            CloseHandle(m_vramBudgetEvent);
            m_vramBudgetEvent = nullptr;
        }
        m_dxgiAdapter3 = nullptr;
    }

    bool RenderDevice::IsVideoMemoryTight() const noexcept
    {
        if (!m_dxgiAdapter3)
        {
            return false;
        }

        for (DXGI_MEMORY_SEGMENT_GROUP group :
             { DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL })
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(m_dxgiAdapter3->QueryVideoMemoryInfo(0, group, &info))
                && info.Budget > 0
                && info.CurrentUsage >= (info.Budget / 100) * 85)
            {
                return true;
            }
        }
        return false;
    }

    void RenderDevice::TrimSurface() noexcept
    {
        if (!m_surfaceTrimmed && m_surface && m_composition)
        {
            if (SUCCEEDED(m_surface->Trim(nullptr, 0)))
            {
                m_composition->Commit();
            }
            m_surfaceTrimmed = true;
        }
    }

    void RenderDevice::TrimGpuCaches() noexcept
    {
        m_background = nullptr;
        TrimSurface();
        if (auto dxgiDevice3 = m_dxgiDevice.try_as<IDXGIDevice3>())
        {
            dxgiDevice3->Trim();
        }
    }

#ifdef AGSHOT_TOOLBAR_PROOF
    void RenderDevice::EnsureProofContext()
    {
        if (!m_d2dFactory)
        {
            CreateDeviceIndependentResources();
        }
        if (!m_d2dContext)
        {
            CreateD3DAndD2DDevice(m_d2dFactory.get(), m_dxgiDevice, m_d2dDevice, m_d2dContext, false);
        }
    }
#endif
}
