#pragma once

#include "hud/hud_theme.h"
#include "hud/ui/ui_layout.h"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/base.h>

#include <string>
#include <vector>

namespace agshot::ui
{
    // Reusable floating dropdown menu anchored to a trigger rect, clamped to viewport.
    class DropdownMenu
    {
    public:
        void SetItems(std::vector<std::wstring> items);
        void SetSelectedIndex(int index) noexcept { m_selected = index; }
        void SetMinWidthDip(int widthDip) noexcept { m_minWidthDip = widthDip > 48 ? widthDip : 48; }
        void SetPreferBelow(bool preferBelow) noexcept { m_preferBelow = preferBelow; }
        void SetLeftOffsetDip(int offsetDip) noexcept { m_leftOffsetDip = offsetDip; }

        bool isOpen() const noexcept { return m_open; }
        int selectedIndex() const noexcept { return m_selected; }
        int hoveredIndex() const noexcept { return m_hovered; }
        const RECT& rect() const noexcept { return m_rect; }
        RECT itemRect(int index) const noexcept;

        SIZE Measure(int dpi) const noexcept;
        void Arrange(const RECT& anchor, const LayoutEnv& env) noexcept;

        void Open(const RECT& anchor, const LayoutEnv& env) noexcept;
        void Open() noexcept { m_open = true; m_hovered = -1; }
        bool Close() noexcept;

        bool HitTest(POINT point) const noexcept;
        bool UpdateHover(POINT point) noexcept;
        void ClearHover() noexcept { m_hovered = -1; }
        LPCWSTR CursorForPoint(POINT point) const noexcept;

        // Returns true if point fell inside the menu; sets outChosenIndex >= 0 if an item was clicked.
        bool OnLeftDown(POINT point, int& outChosenIndex) noexcept;
        bool OnKeyDown(WPARAM key, int& outChosenIndex) noexcept;

        void InvalidateLayouts() noexcept { m_layouts.clear(); }

        void Draw(ID2D1DeviceContext* context, IDWriteFactory* dwrite,
                  IDWriteTextFormat* textFormat, const HudBrushes& brushes,
                  int dpi) const;

    private:
        void EnsureLayouts(IDWriteFactory* dwrite, IDWriteTextFormat* textFormat, int dpi) const;

        std::vector<std::wstring> m_items;
        bool m_open{};
        int m_selected{ 0 };
        int m_hovered{ -1 };
        int m_dpi{ 96 };
        int m_minWidthDip{ 64 };
        int m_leftOffsetDip{ 0 };
        bool m_preferBelow{};
        RECT m_rect{};

        mutable IDWriteFactory* m_layoutFactory{};
        mutable IDWriteTextFormat* m_layoutFormat{};
        mutable int m_layoutDpi{};
        mutable float m_layoutWidth{ -1.0f };
        mutable float m_layoutHeight{ -1.0f };
        mutable std::vector<winrt::com_ptr<IDWriteTextLayout>> m_layouts;
    };
}
