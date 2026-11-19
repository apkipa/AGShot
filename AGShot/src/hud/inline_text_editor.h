#pragma once

#include "hud/annotations.h"
#include "hud/hud_theme.h"
#include "hud/text_edit_utils.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <d2d1_1.h>
#include <dwrite.h>
#include <string>
#include <vector>

#include <winrt/base.h>

namespace agshot
{
    enum class TextEditorZone
    {
        Outside,
        InsideText,
        Border,
    };

    // DirectWrite + Imm32 inline text editor for Text annotations.
    class InlineTextEditor
    {
    public:
        bool active() const noexcept { return m_active; }
        bool dragging() const noexcept { return m_draggingBox || m_selectingText; }
        std::uint64_t editingId() const noexcept { return m_editingId; }
        const AnnotationItem& item() const noexcept { return m_item; }
        std::size_t caret() const noexcept { return m_buf.caret; }
        std::size_t anchor() const noexcept { return m_buf.anchor; }
        const std::wstring& composition() const noexcept { return m_composition; }
        bool HasSelection() const noexcept { return m_buf.HasSelection(); }
        std::pair<std::size_t, std::size_t> SelectionRange() const noexcept { return m_buf.SelectionRange(); }

        void DiscardDeviceResources() noexcept;
        void InvalidateLayout() noexcept;

        D2D1_RECT_F EditorBox(IDWriteFactory* dwrite, int dpi) const;
        void UpdateStyle(const AnnotationStyle& style) noexcept;
        void StartNew(HWND window, D2D1_POINT_2F origin, const AnnotationStyle& style,
                      IDWriteFactory* dwrite, int dpi);
        void StartEdit(HWND window, const AnnotationItem& existing,
                       std::vector<AnnotationItem> snapshotBeforeEdit,
                       IDWriteFactory* dwrite, int dpi);
        void Cancel(HWND window) noexcept;
        void Commit(HWND window, AnnotationDocument& doc);

        TextEditorZone HitTestZone(POINT point, IDWriteFactory* dwrite, int dpi) const;
        bool HitTestInside(POINT point, IDWriteFactory* dwrite, int dpi) const;
        bool OnLeftButtonDown(HWND window, POINT point, IDWriteFactory* dwrite, int dpi,
                              KeyModifiers mods = QueryCurrentKeyModifiers());
        bool OnMouseMove(HWND window, POINT point, IDWriteFactory* dwrite, int dpi);
        bool OnLeftButtonUp() noexcept;
        bool ShowContextMenu(HWND window, POINT point, IDWriteFactory* dwrite, int dpi);
        bool OnKeyDown(HWND window, WPARAM key, IDWriteFactory* dwrite, int dpi,
                       bool& committed, bool& needsRender,
                       KeyModifiers mods = QueryCurrentKeyModifiers());
        bool OnChar(HWND window, wchar_t ch, IDWriteFactory* dwrite, int dpi);
        bool OnImeStartComposition(HWND window, IDWriteFactory* dwrite, int dpi) const;
        bool OnImeComposition(HWND window, LPARAM lparam, IDWriteFactory* dwrite, int dpi);
        bool OnImeEndComposition(HWND window) noexcept;

        void Draw(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                  IDWriteFactory* dwrite, int dpi,
                  const HudBrushes* brushes = nullptr) const;

    private:
        std::wstring ComposedDisplayText() const;
        IDWriteTextLayout* EnsureLayout(IDWriteFactory* dwrite, int dpi) const;
        std::size_t HitTestTextPositionAt(POINT point, IDWriteFactory* dwrite, int dpi) const;
        void ComputeCaretGeometry(IDWriteFactory* dwrite, int dpi,
                                  float& outX, float& outY, float& outHeight) const;
        void UpdateImeCandidatePosition(HWND window, IDWriteFactory* dwrite, int dpi) const;

        bool m_active{};
        bool m_draggingBox{};
        bool m_selectingText{};
        POINT m_dragStartPt{};
        D2D1_POINT_2F m_dragStartOrigin{};
        std::uint64_t m_editingId{};
        AnnotationItem m_item{};
        TextEditBuffer m_buf{};
        std::wstring m_composition;
        std::size_t m_compCursor{};
        TextGranularSelection m_granularSelect{};
        MultiClickTracker m_clickTracker{};
        std::vector<AnnotationItem> m_snapshotBeforeEdit;

        mutable IDWriteFactory* m_layoutFactory{};
        mutable int m_layoutDpi{};
        mutable float m_layoutFontSizeDip{ -1.0f };
        mutable float m_layoutMaxWidth{ -1.0f };
        mutable std::size_t m_layoutCompCaret{};
        mutable std::size_t m_layoutCompLen{};
        mutable std::wstring m_layoutComposedText;
        mutable winrt::com_ptr<IDWriteTextLayout> m_cachedLayout;

        mutable ID2D1Factory1* m_strokeFactory{};
        mutable winrt::com_ptr<ID2D1StrokeStyle> m_dashStroke;
        mutable ID2D1DeviceContext* m_brushContext{};
        mutable winrt::com_ptr<ID2D1SolidColorBrush> m_scratchBrush;
        mutable winrt::com_ptr<ID2D1SolidColorBrush> m_whiteBrush;
        mutable std::vector<DWRITE_HIT_TEST_METRICS> m_hitScratch;
    };
}
