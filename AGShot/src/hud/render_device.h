#pragma once

#include "core/screen_capture.h"
#include "hud/hud_theme.h"

#include <windows.h>

#include <d2d1_1.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_4.h>

#include <winrt/base.h>

namespace agshot
{
    // Owns Direct3D11, Direct2D, DirectComposition, DirectWrite resources, the
    // warm background bitmap, and DXGI VRAM pressure watching.
    // Owned directly as a value member of Overlay on the STA UI thread.
    class RenderDevice
    {
    public:
        RenderDevice() = default;
        ~RenderDevice() noexcept { Destroy(); }

        RenderDevice(const RenderDevice&) = delete;
        RenderDevice& operator=(const RenderDevice&) = delete;

        void Initialize(HWND window, int dpi, UINT vramMsg);
        void Destroy() noexcept;

        void OnDpiChanged(int dpi);
        HRESULT ResizeSurface(UINT width, UINT height);
        HRESULT Recover(UINT& outWidth, UINT& outHeight) noexcept;
        void DiscardDeviceResources() noexcept;
        HRESULT UpdateBackgroundBitmap(const ScreenImage& image);

        HRESULT BeginFrameDraw(UINT width, UINT height);
        HRESULT EndFrameDraw();

        bool IsVideoMemoryTight() const noexcept;
        void TrimSurface() noexcept;
        void TrimGpuCaches() noexcept;

        static bool IsDeviceLost(HRESULT hr) noexcept;

#ifdef AGSHOT_TOOLBAR_PROOF
        void EnsureProofContext();
#endif

        int dpi() const noexcept { return m_dpi; }
        bool hasDeviceContext() const noexcept { return static_cast<bool>(m_d2dContext); }
        bool hasComposition() const noexcept { return static_cast<bool>(m_composition); }
        bool hasSurface() const noexcept { return static_cast<bool>(m_surface); }
        bool hasBackground() const noexcept { return static_cast<bool>(m_background); }
        bool surfaceTrimmed() const noexcept { return m_surfaceTrimmed; }

        ID2D1Factory1* d2dFactory() const noexcept { return m_d2dFactory.get(); }
        ID2D1Device* d2dDevice() const noexcept { return m_d2dDevice.get(); }
        ID2D1DeviceContext* d2dContext() const noexcept { return m_d2dContext.get(); }
        ID2D1Bitmap* background() const noexcept { return m_background.get(); }
        ID2D1Layer* magnifierLayer() const noexcept { return m_magnifierLayer.get(); }
        IDWriteFactory* dwriteFactory() const noexcept { return m_dwriteFactory.get(); }
        IDWriteTextFormat* textFormat() const noexcept { return m_textFormat.get(); }
        IDWriteTextFormat* magnifierTextFormat() const noexcept { return m_magnifierTextFormat.get(); }
        IDWriteTextFormat* statsTextFormat() const noexcept { return m_statsTextFormat.get(); }
        ID2D1StrokeStyle* roundStroke() const noexcept { return m_roundStroke.get(); }
        ID2D1StrokeStyle* flatStroke() const noexcept { return m_flatStroke.get(); }
        HudBrushes& brushes() noexcept { return m_brushes; }
        const HudBrushes& brushes() const noexcept { return m_brushes; }

    private:
        void CreateDeviceIndependentResources();
        void CreateDeviceResources();
        void CreateTextFormats();
        void StartVramWatch() noexcept;
        void StopVramWatch() noexcept;

        static VOID CALLBACK VramWaitCallback(PVOID context, BOOLEAN);

        HWND m_window{};
        UINT m_vramMessage{};
        int m_dpi{ 96 };

        winrt::com_ptr<IDXGIDevice> m_dxgiDevice;
        winrt::com_ptr<IDXGIAdapter3> m_dxgiAdapter3;
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
        // The text-recognition mark has flat-ended bars and rounded brackets.
        winrt::com_ptr<ID2D1StrokeStyle> m_flatStroke;

        winrt::com_ptr<ID2D1Layer> m_magnifierLayer;
        winrt::com_ptr<IDCompositionDevice> m_composition;
        winrt::com_ptr<IDCompositionTarget> m_target;
        winrt::com_ptr<IDCompositionVisual> m_visual;
        winrt::com_ptr<IDCompositionVirtualSurface> m_surface;
        winrt::com_ptr<ID2D1Bitmap1> m_frameTile;
        winrt::com_ptr<ID2D1Bitmap> m_background;
        HudBrushes m_brushes;

        HANDLE m_vramBudgetEvent{};
        DWORD m_vramBudgetCookie{};
        HANDLE m_vramBudgetWait{};
        bool m_surfaceTrimmed{};
    };
}
