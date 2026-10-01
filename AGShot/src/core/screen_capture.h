#pragma once

#include <windows.h>

#include <cstddef>
#include <vector>

namespace agshot
{
    // A picture of the screen, taken once and then never touched again.
    //
    // Freezing matters for more than tidiness. Everything the overlay does after
    // this point - the dimming, the selection, the toolbar - is drawn over a
    // still image, so nothing can move under the user mid-selection, and the
    // copy is a crop of memory rather than a second look at a screen that has
    // since changed. It also means the overlay never has to get out of the way
    // to take its own picture.
    class ScreenImage
    {
    public:
        // Reads a screen region into memory. The region is in screen pixels and
        // may use negative coordinates on a multi-monitor desktop.
        // Throws Error when the screen cannot be read.
        void Capture(const RECT& region);

        bool empty() const noexcept { return m_width <= 0 || m_height <= 0; }
        const RECT& region() const noexcept { return m_region; }
        int width() const noexcept { return m_width; }
        int height() const noexcept { return m_height; }
        int stride() const noexcept { return m_width * 4; }
        const void* pixels() const noexcept { return m_pixels.data(); }

        // Puts part of the frozen picture on the clipboard as a device-independent
        // bitmap, which is what every other application expects to paste.
        // Throws Error when the region misses the capture or the clipboard
        // refuses the data.
        void CopyToClipboard(HWND owner, const RECT& region) const;

    private:
        RECT m_region{};
        int m_width{};
        int m_height{};
        std::vector<BYTE> m_pixels;   // BGRA, top-down
    };
}
