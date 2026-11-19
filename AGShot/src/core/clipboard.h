#pragma once

#include "core/error.h"

#include <windows.h>
#include <ole2.h>
#include <wil/resource.h>

#include <cstddef>
#include <cstring>
#include <string_view>

namespace agshot
{
    // Places UTF-16 text onto the Win32 clipboard (CF_UNICODETEXT).
    inline bool CopyTextToClipboard(HWND owner, std::wstring_view text) noexcept
    {
        if (text.empty())
        {
            return false;
        }
        const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        wil::unique_hglobal mem{ GlobalAlloc(GMEM_MOVEABLE, bytes) };
        if (!mem)
        {
            return false;
        }
        {
            wil::unique_hglobal_locked dst{ mem.get() };
            if (!dst)
            {
                return false;
            }
            std::memcpy(dst.get(), text.data(), text.size() * sizeof(wchar_t));
            static_cast<wchar_t*>(dst.get())[text.size()] = L'\0';
        }
        auto clipboard = wil::open_clipboard(owner);
        if (!clipboard || EmptyClipboard() == 0 || SetClipboardData(CF_UNICODETEXT, mem.get()) == nullptr)
        {
            return false;
        }
        mem.release();
        return true;
    }

    // Places a top-down 32bpp BGRA pixel buffer onto the clipboard as bottom-up CF_DIB.
    inline void CopyBgraToClipboard(HWND owner, const BYTE* topDownBits,
                                    int width, int height, std::size_t rowPitch)
    {
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
        const std::size_t pixelBytes = rowBytes * static_cast<std::size_t>(height);

        wil::unique_hglobal block{ GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes) };
        AGSHOT_CHECK_WIN32(block != nullptr);

        {
            wil::unique_hglobal_locked locked{ block.get() };
            AGSHOT_CHECK_WIN32(locked != nullptr);

            BYTE* target = static_cast<BYTE*>(locked.get());
            BITMAPINFOHEADER header{};
            header.biSize = sizeof(BITMAPINFOHEADER);
            header.biWidth = width;
            header.biHeight = height; // positive: bottom-up CF_DIB
            header.biPlanes = 1;
            header.biBitCount = 32;
            header.biCompression = BI_RGB;
            std::memcpy(target, &header, sizeof(header));

            BYTE* out = target + sizeof(BITMAPINFOHEADER);
            for (int row = 0; row < height; ++row)
            {
                const int sourceRow = height - 1 - row;
                const BYTE* source = topDownBits + static_cast<std::size_t>(sourceRow) * rowPitch;
                std::memcpy(out + static_cast<std::size_t>(row) * rowBytes, source, rowBytes);
            }
        }

        auto clipboard = wil::open_clipboard(owner);
        AGSHOT_CHECK_WIN32(static_cast<bool>(clipboard));
        AGSHOT_CHECK_WIN32(EmptyClipboard() != 0);
        AGSHOT_CHECK_WIN32(SetClipboardData(CF_DIB, block.get()) != nullptr);
        block.release();
    }
}
