#include "pch.h"

#include "core/error.h"
#include "core/screen_capture.h"

#include <cstring>

namespace agshot
{
    namespace
    {
        // Releases a GDI object on every exit path. The failure branches below
        // would otherwise leak them one capture at a time.
        //
        // Three of them rather than one template: ReleaseDC, DeleteDC and
        // DeleteObject do not share a signature, so a template would have to be
        // told which is which at every use.
        struct ScreenDc
        {
            HDC handle{};

            ScreenDc() = default;
            explicit ScreenDc(HDC value) : handle{ value } {}
            ~ScreenDc()
            {
                if (handle != nullptr)
                {
                    ReleaseDC(nullptr, handle);
                }
            }

            ScreenDc(const ScreenDc&) = delete;
            ScreenDc& operator=(const ScreenDc&) = delete;
        };

        struct MemoryDc
        {
            HDC handle{};

            MemoryDc() = default;
            explicit MemoryDc(HDC value) : handle{ value } {}
            ~MemoryDc()
            {
                if (handle != nullptr)
                {
                    DeleteDC(handle);
                }
            }

            MemoryDc(const MemoryDc&) = delete;
            MemoryDc& operator=(const MemoryDc&) = delete;
        };

        struct Bitmap
        {
            HBITMAP handle{};

            Bitmap() = default;
            explicit Bitmap(HBITMAP value) : handle{ value } {}
            ~Bitmap()
            {
                if (handle != nullptr)
                {
                    DeleteObject(handle);
                }
            }

            Bitmap(const Bitmap&) = delete;
            Bitmap& operator=(const Bitmap&) = delete;
        };

        // Opens the clipboard and closes it again whatever happens: a clipboard
        // left open by a failure would lock every other application out of it.
        struct ClipboardLock
        {
            bool open{};

            explicit ClipboardLock(HWND owner) : open{ OpenClipboard(owner) != 0 } {}
            ~ClipboardLock()
            {
                if (open)
                {
                    CloseClipboard();
                }
            }

            ClipboardLock(const ClipboardLock&) = delete;
            ClipboardLock& operator=(const ClipboardLock&) = delete;
        };
    }

    void ScreenImage::Capture(const RECT& region)
    {
        const int width = region.right - region.left;
        const int height = region.bottom - region.top;
        if (width <= 0 || height <= 0)
        {
            throw Error{ Failure{ L"ScreenImage::Capture", L"the region is empty" } };
        }

        const ScreenDc screen{ GetDC(nullptr) };
        AGSHOT_CHECK_WIN32(screen.handle != nullptr);

        // A top-down 32-bit DIB section: BitBlt writes straight into memory in
        // the order the rest of this file wants to read it, with no second pass.
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;   // negative: top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        const Bitmap bitmap{ CreateDIBSection(screen.handle, &info, DIB_RGB_COLORS, &bits, nullptr, 0) };
        AGSHOT_CHECK_WIN32(bitmap.handle != nullptr);
        AGSHOT_CHECK_WIN32(bits != nullptr);

        const MemoryDc memory{ CreateCompatibleDC(screen.handle) };
        AGSHOT_CHECK_WIN32(memory.handle != nullptr);

        HGDIOBJ previous = SelectObject(memory.handle, bitmap.handle);
        AGSHOT_CHECK_WIN32(previous != nullptr && previous != HGDI_ERROR);

        // CAPTUREBLT so that layered windows - which is most modern UI - are part
        // of the picture rather than holes in it.
        AGSHOT_CHECK_WIN32(BitBlt(memory.handle, 0, 0, width, height,
                                  screen.handle, region.left, region.top,
                                  SRCCOPY | CAPTUREBLT) != 0);

        SelectObject(memory.handle, previous);

        // The bits belong to the bitmap and go away with it, so the picture is
        // taken out before this function returns.
        const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
        m_pixels.assign(static_cast<const BYTE*>(bits), static_cast<const BYTE*>(bits) + bytes);
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
        const std::size_t pixels = static_cast<std::size_t>(width) * height * 4;

        // One block holding the header and the pixels together, because that is
        // the shape CF_DIB has on the clipboard.
        HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels);
        AGSHOT_CHECK_WIN32(block != nullptr);

        BYTE* target = static_cast<BYTE*>(GlobalLock(block));
        if (target == nullptr)
        {
            GlobalFree(block);
            AGSHOT_CHECK_WIN32(false);
        }

        BITMAPINFOHEADER header{};
        header.biSize = sizeof(BITMAPINFOHEADER);
        header.biWidth = width;
        header.biHeight = height;   // positive: bottom-up, which is what CF_DIB means
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        std::memcpy(target, &header, sizeof(header));

        // The picture is top-down and CF_DIB is bottom-up, so the rows are walked
        // backwards on the way out.
        BYTE* out = target + sizeof(BITMAPINFOHEADER);
        for (int row = 0; row < height; ++row)
        {
            const int sourceRow = clipped.top - m_region.top + (height - 1 - row);
            const BYTE* source = m_pixels.data()
                + (static_cast<std::size_t>(sourceRow) * m_width + (clipped.left - m_region.left)) * 4;
            std::memcpy(out + static_cast<std::size_t>(row) * width * 4, source,
                        static_cast<std::size_t>(width) * 4);
        }

        GlobalUnlock(block);

        {
            const ClipboardLock lock{ owner };
            AGSHOT_CHECK_WIN32(lock.open);
            AGSHOT_CHECK_WIN32(EmptyClipboard() != 0);

            if (SetClipboardData(CF_DIB, block) == nullptr)
            {
                const DWORD error = GetLastError();
                GlobalFree(block);          // still ours: the clipboard did not take it
                throw Error{ DescribeWin32(L"SetClipboardData", error, __FILEW__, __LINE__) };
            }
            // The clipboard owns the block from here on.
        }
    }
}
