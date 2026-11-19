#include "pch.h"

#include "hud/ui/ui_dropdown.h"

namespace agshot::ui
{
    namespace
    {
        constexpr int kMenuPadDip = 4;
        constexpr int kItemHeightDip = 26;
        constexpr int kItemGapDip = 2;
        constexpr int kAnchorGapDip = 4;
    }

    void DropdownMenu::SetItems(std::vector<std::wstring> items)
    {
        m_items = std::move(items);
        InvalidateLayouts();
    }

    SIZE DropdownMenu::Measure(int dpi) const noexcept
    {
        const int useDpi = dpi > 0 ? dpi : 96;
        const int count = static_cast<int>(m_items.size());
        const int menuW = MulDiv(m_minWidthDip, useDpi, 96);
        const int pad = MulDiv(kMenuPadDip, useDpi, 96);
        const int itemH = MulDiv(kItemHeightDip, useDpi, 96);
        const int itemGap = MulDiv(kItemGapDip, useDpi, 96);
        const int menuH = (count > 0)
            ? (pad * 2 + itemH * count + itemGap * (count - 1))
            : pad * 2;
        return SIZE{ menuW, menuH };
    }

    void DropdownMenu::Arrange(const RECT& anchor, const LayoutEnv& env) noexcept
    {
        m_dpi = env.dpi > 0 ? env.dpi : 96;
        const SIZE sz = Measure(m_dpi);
        const int gap = MulDiv(kAnchorGapDip, m_dpi, 96);
        const int leftOff = MulDiv(m_leftOffsetDip, m_dpi, 96);
        m_rect = PlaceAnchoredMenu(sz, anchor, env.viewport, gap, leftOff, m_preferBelow);
    }

    void DropdownMenu::Open(const RECT& anchor, const LayoutEnv& env) noexcept
    {
        Arrange(anchor, env);
        m_open = true;
        m_hovered = -1;
    }

    bool DropdownMenu::Close() noexcept
    {
        if (!m_open)
        {
            return false;
        }
        m_open = false;
        m_hovered = -1;
        return true;
    }

    RECT DropdownMenu::itemRect(int index) const noexcept
    {
        if (index < 0 || index >= static_cast<int>(m_items.size()))
        {
            return RECT{};
        }
        const int useDpi = m_dpi > 0 ? m_dpi : 96;
        const int pad = MulDiv(kMenuPadDip, useDpi, 96);
        const int itemH = MulDiv(kItemHeightDip, useDpi, 96);
        const int itemGap = MulDiv(kItemGapDip, useDpi, 96);
        const int top = static_cast<int>(m_rect.top) + pad + index * (itemH + itemGap);
        return RECT{ m_rect.left + pad, top, m_rect.right - pad, top + itemH };
    }

    bool DropdownMenu::HitTest(POINT point) const noexcept
    {
        return m_open && PtInRect(&m_rect, point) != 0;
    }

    bool DropdownMenu::UpdateHover(POINT point) noexcept
    {
        const int prev = m_hovered;
        m_hovered = -1;
        if (HitTest(point))
        {
            for (int i = 0; i < static_cast<int>(m_items.size()); ++i)
            {
                const RECT ir = itemRect(i);
                if (PtInRect(&ir, point))
                {
                    m_hovered = i;
                    break;
                }
            }
        }
        return m_hovered != prev;
    }

    LPCWSTR DropdownMenu::CursorForPoint(POINT point) const noexcept
    {
        if (!HitTest(point))
        {
            return IDC_ARROW;
        }
        for (int i = 0; i < static_cast<int>(m_items.size()); ++i)
        {
            const RECT ir = itemRect(i);
            if (PtInRect(&ir, point))
            {
                return IDC_HAND;
            }
        }
        return IDC_ARROW;
    }

    bool DropdownMenu::OnLeftDown(POINT point, int& outChosenIndex) noexcept
    {
        outChosenIndex = -1;
        if (!HitTest(point))
        {
            return false;
        }
        for (int i = 0; i < static_cast<int>(m_items.size()); ++i)
        {
            const RECT ir = itemRect(i);
            if (PtInRect(&ir, point))
            {
                m_selected = i;
                outChosenIndex = i;
                Close();
                break;
            }
        }
        return true;
    }

    bool DropdownMenu::OnKeyDown(WPARAM key, int& outChosenIndex) noexcept
    {
        outChosenIndex = -1;
        if (!m_open || m_items.empty())
        {
            return false;
        }
        const int count = static_cast<int>(m_items.size());
        if (key == VK_ESCAPE)
        {
            Close();
            return true;
        }
        if (key == VK_UP || key == VK_DOWN)
        {
            const int base = (m_hovered >= 0) ? m_hovered : m_selected;
            m_hovered = (base + (key == VK_DOWN ? 1 : count - 1)) % count;
            return true;
        }
        if (key == VK_RETURN)
        {
            const int pick = (m_hovered >= 0) ? m_hovered : m_selected;
            m_selected = std::clamp(pick, 0, count - 1);
            outChosenIndex = m_selected;
            Close();
            return true;
        }
        return false;
    }

    void DropdownMenu::EnsureLayouts(
        IDWriteFactory* dwrite, IDWriteTextFormat* textFormat, int dpi) const
    {
        if (dwrite == nullptr || textFormat == nullptr || m_items.empty())
        {
            return;
        }
        const RECT sample = itemRect(0);
        const float rowPadX = ScaleByDpi(8.0f, dpi);
        const float w = std::max(4.0f, static_cast<float>(sample.right - sample.left) - rowPadX * 2.0f);
        const float h = std::max(1.0f, static_cast<float>(sample.bottom - sample.top));

        if (m_layouts.size() == m_items.size()
            && m_layoutFactory == dwrite
            && m_layoutFormat == textFormat
            && m_layoutDpi == dpi
            && std::fabs(m_layoutWidth - w) < 0.25f
            && std::fabs(m_layoutHeight - h) < 0.25f)
        {
            return;
        }

        m_layouts.clear();
        m_layouts.resize(m_items.size());
        for (std::size_t i = 0; i < m_items.size(); ++i)
        {
            if (SUCCEEDED(dwrite->CreateTextLayout(
                m_items[i].c_str(), static_cast<UINT32>(m_items[i].size()),
                textFormat, w, h, m_layouts[i].put())))
            {
                m_layouts[i]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_layouts[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_layouts[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        m_layoutFactory = dwrite;
        m_layoutFormat = textFormat;
        m_layoutDpi = dpi;
        m_layoutWidth = w;
        m_layoutHeight = h;
    }

    void DropdownMenu::Draw(
        ID2D1DeviceContext* context, IDWriteFactory* dwrite,
        IDWriteTextFormat* textFormat, const HudBrushes& brushes, int dpi) const
    {
        if (!m_open || context == nullptr)
        {
            return;
        }

        const float pillR = ScaleByDpi(4.0f, dpi);
        const float menuR = ScaleByDpi(6.0f, dpi);
        const float shadowPadX = ScaleByDpi(1.5f, dpi);
        const float shadowOffY = ScaleByDpi(2.5f, dpi);
        const D2D1_RECT_F shadowRect = D2D1::RectF(
            static_cast<float>(m_rect.left) - shadowPadX,
            static_cast<float>(m_rect.top),
            static_cast<float>(m_rect.right) + shadowPadX,
            static_cast<float>(m_rect.bottom) + shadowOffY);

        context->FillRoundedRectangle(
            D2D1::RoundedRect(shadowRect, ScaleByDpi(7.0f, dpi), ScaleByDpi(7.0f, dpi)),
            brushes.WithColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.42f)));
        context->FillRoundedRectangle(
            ToRoundedRect(m_rect, menuR),
            brushes.WithColor(D2D1::ColorF(0.18f, 0.19f, 0.23f, 0.99f)));
        context->DrawRoundedRectangle(
            ToRoundedRect(m_rect, menuR),
            brushes.WithColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f)), 1.0f);

        EnsureLayouts(dwrite, textFormat, dpi);
        const float rowPadX = ScaleByDpi(8.0f, dpi);
        for (int i = 0; i < static_cast<int>(m_items.size()); ++i)
        {
            const RECT itemR = itemRect(i);
            const bool isActive = (i == m_selected);
            const bool isHovered = (i == m_hovered);
            if (isActive)
            {
                context->FillRoundedRectangle(
                    ToRoundedRect(itemR, pillR),
                    brushes.WithColor(D2D1::ColorF(0.18f, 0.49f, 0.94f, 0.30f)));
            }
            else if (isHovered)
            {
                context->FillRoundedRectangle(ToRoundedRect(itemR, pillR), brushes.hover.get());
            }

            if (i < static_cast<int>(m_layouts.size()) && m_layouts[static_cast<std::size_t>(i)])
            {
                context->DrawTextLayout(
                    D2D1::Point2F(static_cast<float>(itemR.left) + rowPadX, static_cast<float>(itemR.top)),
                    m_layouts[static_cast<std::size_t>(i)].get(),
                    (isActive || isHovered) ? brushes.handleFill.get() : brushes.icon.get());
            }
        }
    }
}
