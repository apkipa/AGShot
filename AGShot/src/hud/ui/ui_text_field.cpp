#include "pch.h"

#include "hud/ui/ui_text_field.h"

namespace agshot::ui
{
    void TextFieldSlot::Arrange(
        const RECT& boxRect, int dpi,
        const wchar_t* prefixLabel, const wchar_t* suffixLabel) noexcept
    {
        box = boxRect;
        prefix = prefixLabel;
        suffix = suffixLabel;
        const float leftPad = (prefixLabel != nullptr)
            ? ScaleByDpi(15.0f, dpi)
            : ScaleByDpi((suffixLabel != nullptr) ? 4.5f : 5.0f, dpi);
        const float rightPad = (suffixLabel != nullptr)
            ? ScaleByDpi(14.5f, dpi)
            : ScaleByDpi((prefixLabel != nullptr) ? 5.0f : 4.0f, dpi);
        inner = D2D1::RectF(
            static_cast<float>(boxRect.left) + leftPad,
            static_cast<float>(boxRect.top),
            std::max(static_cast<float>(boxRect.left) + leftPad + 4.0f,
                     static_cast<float>(boxRect.right) - rightPad),
            static_cast<float>(boxRect.bottom));
    }

    void TextFieldSlot::Clear() noexcept
    {
        box = RECT{};
        inner = D2D1_RECT_F{};
        prefix = nullptr;
        suffix = nullptr;
        hovered = false;
        InvalidateLayout();
    }

    void TextFieldSlot::InvalidateLayout() noexcept
    {
        m_textLayout = nullptr;
        m_prefixLayout = nullptr;
        m_suffixLayout = nullptr;
        m_cachedText.clear();
        m_cachedPrefix = nullptr;
        m_cachedSuffix = nullptr;
        m_textW = -1.0f;
        m_textH = -1.0f;
        m_prefixW = -1.0f;
        m_suffixW = -1.0f;
    }

    IDWriteTextLayout* TextFieldSlot::EnsureTextLayout(
        IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
        std::wstring_view displayText) const
    {
        if (dwrite == nullptr || textFormat == nullptr)
        {
            return nullptr;
        }
        const float iw = std::max(1.0f, inner.right - inner.left);
        const float ih = std::max(1.0f, inner.bottom - inner.top);

        if (!m_textLayout
            || m_factory != dwrite
            || m_format != textFormat
            || std::fabs(m_textW - iw) > 0.25f
            || std::fabs(m_textH - ih) > 0.25f
            || m_cachedText != displayText)
        {
            m_textLayout = nullptr;
            if (FAILED(dwrite->CreateTextLayout(
                displayText.data(), static_cast<UINT32>(displayText.size()),
                textFormat, iw, ih, m_textLayout.put())))
            {
                return nullptr;
            }
            m_textLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_textLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_textLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            m_factory = dwrite;
            m_format = textFormat;
            m_textW = iw;
            m_textH = ih;
            m_cachedText.assign(displayText);
        }
        return m_textLayout.get();
    }

    IDWriteTextLayout* TextFieldSlot::EnsureAffixLayout(
        IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
        const wchar_t* affix, float w, float h, bool isPrefix) const
    {
        if (dwrite == nullptr || textFormat == nullptr || affix == nullptr)
        {
            return nullptr;
        }
        auto& layout = isPrefix ? m_prefixLayout : m_suffixLayout;
        auto& cachedStr = isPrefix ? m_cachedPrefix : m_cachedSuffix;
        auto& cachedW = isPrefix ? m_prefixW : m_suffixW;
        const float cw = std::max(1.0f, w);
        const float ch = std::max(1.0f, h);

        if (!layout
            || m_factory != dwrite
            || m_format != textFormat
            || cachedStr != affix
            || std::fabs(cachedW - cw) > 0.25f)
        {
            layout = nullptr;
            if (SUCCEEDED(dwrite->CreateTextLayout(
                affix, static_cast<UINT32>(wcslen(affix)),
                textFormat, cw, ch, layout.put())))
            {
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                cachedStr = affix;
                cachedW = cw;
                m_factory = dwrite;
                m_format = textFormat;
            }
        }
        return layout.get();
    }

    void SingleLineInputState::Clear() noexcept
    {
        m_buf.Clear();
        m_selectAllPendingOnDrag = false;
        m_dragStartX = 0;
        m_dragOriginPos = 0;
    }

    void SingleLineInputState::ResetClicks() noexcept
    {
        m_clickTracker.Reset();
        m_selectAllPendingOnDrag = false;
    }

    void SingleLineInputState::SetText(std::wstring value, bool selectAll) noexcept
    {
        m_buf.SetText(std::move(value), selectAll);
        m_selectAllPendingOnDrag = false;
    }

    void SingleLineInputState::ReplaceSelectionBounded(
        std::wstring_view replacement, std::size_t maxLength, bool coalesceSingleChar)
    {
        const auto [start, end] = m_buf.SelectionRange();
        const std::size_t keepLen = m_buf.text.size() - (end - start);
        const std::size_t avail = (maxLength > keepLen) ? (maxLength - keepLen) : 0u;
        m_buf.ReplaceSelection(replacement.substr(0, avail), true, coalesceSingleChar);
    }

    std::size_t SingleLineInputState::HitTestCaret(
        POINT point, const TextFieldSlot& slot, std::wstring_view displayText,
        IDWriteFactory* dwrite, IDWriteTextFormat* textFormat) noexcept
    {
        if (displayText.empty())
        {
            return 0;
        }
        IDWriteTextLayout* layout = slot.EnsureTextLayout(dwrite, textFormat, displayText);
        if (layout == nullptr)
        {
            return displayText.size();
        }
        BOOL isTrailing = FALSE;
        BOOL isInside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        const float lx = static_cast<float>(point.x) - slot.inner.left;
        const float ly = (slot.inner.bottom - slot.inner.top) * 0.5f;
        if (SUCCEEDED(layout->HitTestPoint(lx, ly, &isTrailing, &isInside, &metrics)))
        {
            return std::min(displayText.size(), static_cast<std::size_t>(
                metrics.textPosition + (isTrailing ? metrics.length : 0)));
        }
        return displayText.size();
    }

    bool SingleLineInputState::OnLeftButtonDown(
        POINT point, bool wasAlreadyFocused, const TextFieldSlot& slot,
        IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
        KeyModifiers mods) noexcept
    {
        const int clicks = m_clickTracker.RegisterClick(point);
        if (!wasAlreadyFocused)
        {
            m_dragOriginPos = HitTestCaret(point, slot, m_buf.text, dwrite, textFormat);
            m_dragStartX = point.x;
            m_selectAllPendingOnDrag = true;
            return true;
        }
        if (clicks >= 2)
        {
            m_buf.SelectAll();
            m_selectAllPendingOnDrag = false;
            return false;
        }
        const std::size_t pos = HitTestCaret(point, slot, m_buf.text, dwrite, textFormat);
        m_buf.CollapseTo(pos, mods.shift);
        m_selectAllPendingOnDrag = false;
        return true;
    }

    void SingleLineInputState::OnMouseMove(
        POINT point, const TextFieldSlot& slot,
        IDWriteFactory* dwrite, IDWriteTextFormat* textFormat) noexcept
    {
        if (m_selectAllPendingOnDrag)
        {
            if (std::abs(static_cast<int>(point.x - m_dragStartX)) < 2)
            {
                return;
            }
            m_selectAllPendingOnDrag = false;
            m_buf.BreakUndoCoalescing();
            m_buf.anchor = std::min(m_dragOriginPos, m_buf.text.size());
        }
        m_buf.BreakUndoCoalescing();
        m_buf.caret = HitTestCaret(point, slot, m_buf.text, dwrite, textFormat);
    }

    bool SingleLineInputState::HandleEditingKey(WPARAM key, KeyModifiers mods, bool& outMutated)
    {
        outMutated = false;
        if (mods.ctrl && key == 'Z')
        {
            outMutated = mods.shift ? m_buf.Redo() : m_buf.Undo();
            return true;
        }
        if (mods.ctrl && key == 'Y')
        {
            outMutated = m_buf.Redo();
            return true;
        }
        if (mods.ctrl && key == 'A')
        {
            m_buf.SelectAll();
            return true;
        }

        switch (key)
        {
        case VK_BACK:
            outMutated = m_buf.DeleteBackward(mods.ctrl);
            return true;
        case VK_DELETE:
            outMutated = m_buf.DeleteForward(mods.ctrl);
            return true;
        case VK_LEFT:
            if (!mods.shift && m_buf.HasSelection())
            {
                m_buf.CollapseTo(m_buf.SelectionRange().first, false);
            }
            else
            {
                const std::size_t dst = mods.ctrl
                    ? PrevWordOffset(m_buf.text, m_buf.caret)
                    : PrevCodeUnitOffset(m_buf.text, m_buf.caret);
                m_buf.CollapseTo(dst, mods.shift);
            }
            return true;
        case VK_RIGHT:
            if (!mods.shift && m_buf.HasSelection())
            {
                m_buf.CollapseTo(m_buf.SelectionRange().second, false);
            }
            else
            {
                const std::size_t dst = mods.ctrl
                    ? NextWordOffset(m_buf.text, m_buf.caret)
                    : NextCodeUnitOffset(m_buf.text, m_buf.caret);
                m_buf.CollapseTo(dst, mods.shift);
            }
            return true;
        case VK_HOME:
            m_buf.CollapseTo(0, mods.shift);
            return true;
        case VK_END:
            m_buf.CollapseTo(m_buf.text.size(), mods.shift);
            return true;
        default:
            return false;
        }
    }

    void SingleLineInputState::DrawSlot(
        ID2D1DeviceContext* context, IDWriteFactory* dwrite,
        IDWriteTextFormat* textFormat, const HudBrushes& brushes,
        int dpi, const TextFieldSlot& slot, float pillRadius,
        bool focused, std::wstring_view displayText) const
    {
        context->FillRoundedRectangle(
            ToRoundedRect(slot.box, pillRadius),
            (slot.hovered && !focused) ? brushes.hover.get() : brushes.tooltip.get());
        context->DrawRoundedRectangle(
            ToRoundedRect(slot.box, pillRadius),
            focused ? brushes.accent.get() : brushes.panelEdge.get(),
            focused ? ScaleByDpi(1.5f, dpi) : 1.0f);

        if (slot.prefix != nullptr)
        {
            const float left = static_cast<float>(slot.box.left) + ScaleByDpi(4.0f, dpi);
            const float top = static_cast<float>(slot.box.top);
            if (IDWriteTextLayout* pre = slot.EnsureAffixLayout(
                dwrite, textFormat, slot.prefix,
                slot.inner.left - left, static_cast<float>(slot.box.bottom) - top, true))
            {
                context->DrawTextLayout(D2D1::Point2F(left, top), pre, brushes.disabledIcon.get());
            }
        }
        if (slot.suffix != nullptr)
        {
            const float left = slot.inner.right;
            const float top = static_cast<float>(slot.box.top);
            const float right = static_cast<float>(slot.box.right) - ScaleByDpi(2.5f, dpi);
            if (IDWriteTextLayout* suf = slot.EnsureAffixLayout(
                dwrite, textFormat, slot.suffix,
                right - left, static_cast<float>(slot.box.bottom) - top, false))
            {
                context->DrawTextLayout(D2D1::Point2F(left, top), suf, brushes.disabledIcon.get());
            }
        }

        IDWriteTextLayout* layout = slot.EnsureTextLayout(dwrite, textFormat, displayText);
        if (layout == nullptr)
        {
            return;
        }

        bool appliedEffect = false;
        DWRITE_TEXT_RANGE selRange{};
        if (focused && m_buf.HasSelection())
        {
            const auto [selStart, selEnd] = m_buf.SelectionRange();
            selRange = DWRITE_TEXT_RANGE{
                static_cast<UINT32>(selStart),
                static_cast<UINT32>(selEnd - selStart)
            };
            UINT32 hitCount = 0;
            layout->HitTestTextRange(
                selRange.startPosition, selRange.length,
                slot.inner.left, slot.inner.top, nullptr, 0, &hitCount);
            if (hitCount > 0)
            {
                m_hitScratch.resize(hitCount);
                if (SUCCEEDED(layout->HitTestTextRange(
                    selRange.startPosition, selRange.length,
                    slot.inner.left, slot.inner.top, m_hitScratch.data(), hitCount, &hitCount)))
                {
                    ID2D1SolidColorBrush* selBrush = brushes.WithColor(D2D1::ColorF(0.18f, 0.49f, 0.94f, 0.85f));
                    for (UINT32 hi = 0; hi < hitCount; ++hi)
                    {
                        context->FillRectangle(
                            D2D1::RectF(m_hitScratch[hi].left, m_hitScratch[hi].top,
                                        m_hitScratch[hi].left + m_hitScratch[hi].width,
                                        m_hitScratch[hi].top + m_hitScratch[hi].height),
                            selBrush);
                    }
                }
            }
            layout->SetDrawingEffect(brushes.handleFill.get(), selRange);
            appliedEffect = true;
        }

        if (!displayText.empty())
        {
            context->DrawTextLayout(
                D2D1::Point2F(slot.inner.left, slot.inner.top),
                layout, brushes.icon.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        if (appliedEffect)
        {
            layout->SetDrawingEffect(nullptr, selRange);
        }

        if (focused)
        {
            float cx = 0.0f;
            float cy = 0.0f;
            DWRITE_HIT_TEST_METRICS cm{};
            if (SUCCEEDED(layout->HitTestTextPosition(
                static_cast<UINT32>(m_buf.caret), FALSE, &cx, &cy, &cm)))
            {
                const float ch = (cm.height > 1.0f) ? cm.height : ScaleByDpi(14.0f, dpi);
                const float caretW = std::max(1.25f, ScaleByDpi(1.5f, dpi));
                context->FillRectangle(
                    D2D1::RectF(slot.inner.left + cx, slot.inner.top + cy,
                                slot.inner.left + cx + caretW, slot.inner.top + cy + ch),
                    brushes.icon.get());
            }
        }
    }
}
