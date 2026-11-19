#pragma once

#include "hud/hud_theme.h"
#include "hud/text_edit_utils.h"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/base.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace agshot::ui
{
    // Lightweight single-line text box geometry, hover state, and cached short
    // DirectWrite layouts (prefix, suffix, and display text) so unfocused boxes
    // carry zero TextEditBuffer/undo overhead and zero per-frame layout rebuilds.
    struct TextFieldSlot
    {
        RECT box{};
        D2D1_RECT_F inner{};
        const wchar_t* prefix{};
        const wchar_t* suffix{};
        bool hovered{};

        void Arrange(const RECT& boxRect, int dpi,
                     const wchar_t* prefixLabel, const wchar_t* suffixLabel) noexcept;
        void Clear() noexcept;
        bool HitTest(POINT point) const noexcept { return !IsRectEmpty(&box) && PtInRect(&box, point) != 0; }
        void InvalidateLayout() noexcept;

        IDWriteTextLayout* EnsureTextLayout(
            IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
            std::wstring_view displayText) const;
        IDWriteTextLayout* EnsureAffixLayout(
            IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
            const wchar_t* affix, float w, float h, bool isPrefix) const;

    private:
        mutable IDWriteFactory* m_factory{};
        mutable IDWriteTextFormat* m_format{};
        mutable float m_textW{ -1.0f };
        mutable float m_textH{ -1.0f };
        mutable std::wstring m_cachedText;
        mutable winrt::com_ptr<IDWriteTextLayout> m_textLayout;

        mutable const wchar_t* m_cachedPrefix{};
        mutable float m_prefixW{ -1.0f };
        mutable winrt::com_ptr<IDWriteTextLayout> m_prefixLayout;

        mutable const wchar_t* m_cachedSuffix{};
        mutable float m_suffixW{ -1.0f };
        mutable winrt::com_ptr<IDWriteTextLayout> m_suffixLayout;
    };

    // Single shared editing session controller for a group of TextFieldSlots where
    // at most one slot has keyboard focus at any time.
    class SingleLineInputState
    {
    public:
        const std::wstring& text() const noexcept { return m_buf.text; }
        std::size_t caret() const noexcept { return m_buf.caret; }
        std::size_t anchor() const noexcept { return m_buf.anchor; }
        bool HasSelection() const noexcept { return m_buf.HasSelection(); }
        std::pair<std::size_t, std::size_t> SelectionRange() const noexcept { return m_buf.SelectionRange(); }

        void Clear() noexcept;
        void ResetClicks() noexcept;
        void EndDrag() noexcept { m_selectAllPendingOnDrag = false; }
        void SetText(std::wstring value, bool selectAll) noexcept;
        void SelectAll() noexcept { m_buf.SelectAll(); }
        void CollapseTo(std::size_t pos, bool extendSelection) noexcept { m_buf.CollapseTo(pos, extendSelection); }

        void ReplaceSelectionBounded(std::wstring_view replacement, std::size_t maxLength,
                                     bool coalesceSingleChar = false);

        static std::size_t HitTestCaret(POINT point, const TextFieldSlot& slot,
                                        std::wstring_view displayText,
                                        IDWriteFactory* dwrite,
                                        IDWriteTextFormat* textFormat) noexcept;

        bool OnLeftButtonDown(POINT point, bool wasAlreadyFocused,
                              const TextFieldSlot& slot,
                              IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
                              KeyModifiers mods) noexcept;

        void OnMouseMove(POINT point, const TextFieldSlot& slot,
                         IDWriteFactory* dwrite, IDWriteTextFormat* textFormat) noexcept;

        bool HandleEditingKey(WPARAM key, KeyModifiers mods, bool& outMutated);

        void DrawSlot(ID2D1DeviceContext* context, IDWriteFactory* dwrite,
                      IDWriteTextFormat* textFormat, const HudBrushes& brushes,
                      int dpi, const TextFieldSlot& slot, float pillRadius,
                      bool focused, std::wstring_view displayText) const;

    private:
        TextEditBuffer m_buf;
        MultiClickTracker m_clickTracker{};
        bool m_selectAllPendingOnDrag{};
        LONG m_dragStartX{};
        std::size_t m_dragOriginPos{};
        mutable std::vector<DWRITE_HIT_TEST_METRICS> m_hitScratch;
    };
}
