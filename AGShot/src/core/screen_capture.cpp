#include "pch.h"

#include "core/clipboard.h"
#include "core/error.h"
#include "core/screen_capture.h"

namespace agshot
{
    namespace
    {
        void BlitScreenToBitmap(HDC screen, HBITMAP target, const RECT& region, int width, int height)
        {
            const wil::unique_hdc memory{ CreateCompatibleDC(screen) };
            AGSHOT_CHECK_WIN32(memory != nullptr);

            HGDIOBJ previous = SelectObject(memory.get(), target);
            AGSHOT_CHECK_WIN32(previous != nullptr && previous != HGDI_ERROR);
            auto restoreBitmap = wil::scope_exit([&] { SelectObject(memory.get(), previous); });

            // CAPTUREBLT so that layered windows - which is most modern UI - are part
            // of the picture rather than holes in it.
            const BOOL blitted = BitBlt(memory.get(), 0, 0, width, height,
                                        screen, region.left, region.top,
                                        SRCCOPY | CAPTUREBLT);
            AGSHOT_CHECK_WIN32(blitted != 0);

            // GDI is allowed to hold the blit, and the bits are about to be read as
            // ordinary memory rather than through the DC.
            GdiFlush();
        }
    }

    ScreenImage::~ScreenImage() noexcept
    {
        Release();
    }

    ScreenImage::ScreenImage(ScreenImage&& other) noexcept
    {
        *this = std::move(other);
    }

    ScreenImage& ScreenImage::operator=(ScreenImage&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_region = other.m_region;
            m_width = other.m_width;
            m_height = other.m_height;
            m_bitmap = std::move(other.m_bitmap);
            m_bits = other.m_bits;
            other.m_bits = nullptr;
            other.m_region = RECT{};
            other.m_width = 0;
            other.m_height = 0;
        }
        return *this;
    }

    void ScreenImage::Clear() noexcept
    {
        Release();
    }

    void ScreenImage::Release() noexcept
    {
        m_bitmap.reset();
        m_bits = nullptr;
        m_region = RECT{};
        m_width = 0;
        m_height = 0;
    }

    void ScreenImage::Capture(const RECT& region)
    {
        const int width = region.right - region.left;
        const int height = region.bottom - region.top;
        if (width <= 0 || height <= 0)
        {
            throw Error{ Failure{ L"ScreenImage::Capture", L"the region is empty" } };
        }

        const auto screen = wil::GetDC(nullptr);
        AGSHOT_CHECK_WIN32(screen != nullptr);

        // Reuse the warm DIB section when dimensions have not changed so a
        // repeat capture pays only one BitBlt with zero page allocation.
        if (m_bitmap && m_bits != nullptr && m_width == width && m_height == height)
        {
            BlitScreenToBitmap(screen.get(), m_bitmap.get(), region, width, height);
            m_region = region;
            return;
        }

        // A top-down 32-bit DIB section: BitBlt writes straight into memory in
        // the order the rest of this file wants to read it, and that memory is
        // the picture this object hands out, with no second pass.
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;   // negative: top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        wil::unique_hbitmap bitmap{
            CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0) };
        AGSHOT_CHECK_WIN32(bitmap != nullptr);
        AGSHOT_CHECK_WIN32(bits != nullptr);

        BlitScreenToBitmap(screen.get(), bitmap.get(), region, width, height);

        // Everything that can fail has happened, so the section takes over as the
        // owner of the pixels from here. Until this point the local keeps them,
        // which is what makes a failure part way through leave the object as it
        // was.
        Release();
        m_bitmap = std::move(bitmap);
        m_bits = static_cast<BYTE*>(bits);
        m_region = region;
        m_width = width;
        m_height = height;
    }

    void ScreenImage::CopyToClipboard(HWND owner, const RECT& region) const
    {
        RECT clipped{};
        if (empty()
            || !IntersectRect(&clipped, &region, &m_region)
            || IsRectEmpty(&clipped))
        {
            throw Error{ Failure{ L"ScreenImage::CopyToClipboard",
                                  L"the region is not inside the picture that was taken" } };
        }

        const int width = clipped.right - clipped.left;
        const int height = clipped.bottom - clipped.top;
        const std::size_t rowPitch = static_cast<std::size_t>(m_width) * 4;
        const BYTE* topLeft = m_bits
            + static_cast<std::size_t>(clipped.top - m_region.top) * rowPitch
            + static_cast<std::size_t>(clipped.left - m_region.left) * 4;
        CopyBgraToClipboard(owner, topLeft, width, height, rowPitch);
    }
}
