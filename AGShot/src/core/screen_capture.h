#pragma once

#include <windows.h>

#include <cstddef>

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
    //
    // The pixels are the bits of a DIB section this object owns, so a capture
    // costs one BitBlt and nothing else. Copying them into a buffer of our own
    // would be a second full-frame allocation and a full-frame copy per capture,
    // and would buy nothing: the DIB section's bits stay put until the section is
    // deleted, which is exactly the lifetime this class already has.
    class ScreenImage
    {
    public:
        ScreenImage() = default;
        ~ScreenImage() noexcept;

        // The pixels belong to one DIB section, so an image cannot be copied. It
        // can be moved, which is what keeps it usable as a member.
        ScreenImage(const ScreenImage&) = delete;
        ScreenImage& operator=(const ScreenImage&) = delete;
        ScreenImage(ScreenImage&& other) noexcept;
        ScreenImage& operator=(ScreenImage&& other) noexcept;

        // Reads a screen region into memory. The region is in screen pixels and
        // may use negative coordinates on a multi-monitor desktop.
        // Throws Error when the screen cannot be read.
        void Capture(const RECT& region);

        // Releases the captured DIB section immediately.
        void Clear() noexcept;

        bool empty() const noexcept { return m_width <= 0 || m_height <= 0; }
        const RECT& region() const noexcept { return m_region; }
        int width() const noexcept { return m_width; }
        int height() const noexcept { return m_height; }
        int stride() const noexcept { return m_width * 4; }
        const void* pixels() const noexcept { return m_bits; }

        // Puts part of the frozen picture on the clipboard as a device-independent
        // bitmap, which is what every other application expects to paste.
        // Throws Error when the region misses the capture or the clipboard
        // refuses the data.
        void CopyToClipboard(HWND owner, const RECT& region) const;

    private:
        void Release() noexcept;

        RECT m_region{};
        int m_width{};
        int m_height{};
        // The DIB section holds the picture; m_bits points into it and is valid
        // for exactly as long as m_bitmap is. BGRA, top-down.
        HBITMAP m_bitmap{};
        BYTE* m_bits{};
    };
}
