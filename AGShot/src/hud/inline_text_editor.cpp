#include "pch.h"

#include "core/error.h"
#include "hud/inline_text_editor.h"

#include <imm.h>

namespace agshot
{
    namespace
    {
        void CancelActiveImeComposition(HWND window) noexcept
        {
            if (window == nullptr)
            {
                return;
            }
            HIMC imc = ImmGetContext(window);
            if (imc != nullptr)
            {
                ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(window, imc);
            }
        }
    }

    void InlineTextEditor::DiscardDeviceResources() noexcept
    {
        m_strokeFactory = nullptr;
        m_dashStroke = nullptr;
    }

    void InlineTextEditor::InvalidateLayout() noexcept
    {
        m_cachedLayout = nullptr;
        m_layoutFactory = nullptr;
        m_layoutDpi = 0;
        m_layoutFontSizeDip = -1.0f;
        m_layoutMaxWidth = -1.0f;
        m_layoutCompCaret = 0;
        m_layoutCompLen = 0;
        m_layoutComposedText.clear();
    }

    void InlineTextEditor::UpdateStyle(const AnnotationStyle& style) noexcept
    {
        if (!m_active)
        {
            return;
        }
        if (m_item.style.fontSizeDip != style.fontSizeDip)
        {
            InvalidateLayout();
        }
        m_item.style.color = style.color;
        m_item.style.fontSizeDip = style.fontSizeDip;
    }

    void InlineTextEditor::StartNew(HWND window, D2D1_POINT_2F origin, const AnnotationStyle& style,
                                   IDWriteFactory* dwrite, int dpi)
    {
        Cancel(window);
        m_active = true;
        m_draggingBox = false;
        m_selectingText = false;
        m_editingId = 0;
        m_item = AnnotationItem{};
        m_item.kind = AnnotationKind::Text;
        m_item.origin = origin;
        m_item.style = style;
        m_buf.Clear();
        m_granularSelect.Reset();
        m_clickTracker.Reset();
        m_composition.clear();
        m_compCursor = 0;
        m_snapshotBeforeEdit.clear();
        InvalidateLayout();
        if (window != nullptr)
        {
            ImmAssociateContextEx(window, nullptr, IACE_DEFAULT);
        }
        UpdateImeCandidatePosition(window, dwrite, dpi);
    }

    void InlineTextEditor::StartEdit(HWND window, const AnnotationItem& existing,
                                     std::vector<AnnotationItem> snapshotBeforeEdit,
                                     IDWriteFactory* dwrite, int dpi)
    {
        Cancel(window);
        m_active = true;
        m_draggingBox = false;
        m_selectingText = false;
        m_editingId = existing.id;
        m_item = existing;
        m_buf.SetText(existing.text, false);
        m_granularSelect.Reset();
        m_clickTracker.Reset();
        m_composition.clear();
        m_compCursor = 0;
        m_snapshotBeforeEdit = std::move(snapshotBeforeEdit);
        InvalidateLayout();
        if (window != nullptr)
        {
            ImmAssociateContextEx(window, nullptr, IACE_DEFAULT);
        }
        UpdateImeCandidatePosition(window, dwrite, dpi);
    }

    void InlineTextEditor::Cancel(HWND window) noexcept
    {
        if (!m_active)
        {
            return;
        }
        CancelActiveImeComposition(window);
        if (window != nullptr)
        {
            ImmAssociateContextEx(window, nullptr, 0);
        }
        m_active = false;
        m_draggingBox = false;
        m_selectingText = false;
        m_editingId = 0;
        m_composition.clear();
        m_compCursor = 0;
        m_item = AnnotationItem{};
        m_buf.Clear();
        m_granularSelect.Reset();
        m_clickTracker.Reset();
        m_snapshotBeforeEdit.clear();
        InvalidateLayout();
    }

    void InlineTextEditor::Commit(HWND window, AnnotationDocument& doc)
    {
        if (!m_active)
        {
            return;
        }
        CancelActiveImeComposition(window);
        if (window != nullptr)
        {
            ImmAssociateContextEx(window, nullptr, 0);
        }

        const std::uint64_t editId = m_editingId;
        AnnotationItem committed = std::move(m_item);
        committed.text = m_buf.text;
        std::vector<AnnotationItem> snapshot = std::move(m_snapshotBeforeEdit);

        m_active = false;
        m_draggingBox = false;
        m_selectingText = false;
        m_editingId = 0;
        m_composition.clear();
        m_compCursor = 0;
        m_item = AnnotationItem{};
        m_buf.Clear();
        m_granularSelect.Reset();
        m_clickTracker.Reset();
        m_snapshotBeforeEdit.clear();
        InvalidateLayout();

        bool hasVisible = false;
        for (wchar_t ch : committed.text)
        {
            if (ch != L' ' && ch != L'\t' && ch != L'\n' && ch != L'\r')
            {
                hasVisible = true;
                break;
            }
        }

        if (editId == 0)
        {
            if (hasVisible)
            {
                const std::uint64_t newId = doc.AddItem(std::move(committed));
                doc.SetSelectedId(newId);
            }
            return;
        }

        if (!hasVisible)
        {
            doc.DeleteItem(editId);
            return;
        }

        if (AnnotationItem* existing = doc.FindItem(editId))
        {
            const bool changed = (existing->text != committed.text)
                || (existing->origin.x != committed.origin.x)
                || (existing->origin.y != committed.origin.y)
                || (existing->style.fontSizeDip != committed.style.fontSizeDip)
                || (std::fabs(existing->style.color.r - committed.style.color.r) > 1e-3f)
                || (std::fabs(existing->style.color.g - committed.style.color.g) > 1e-3f)
                || (std::fabs(existing->style.color.b - committed.style.color.b) > 1e-3f)
                || (std::fabs(existing->style.color.a - committed.style.color.a) > 1e-3f);
            if (changed)
            {
                doc.CommitPreviousState(std::move(snapshot));
                *existing = std::move(committed);
                existing->id = editId;
            }
            doc.SetSelectedId(editId);
        }
    }

    std::wstring InlineTextEditor::ComposedDisplayText() const
    {
        if (m_composition.empty())
        {
            return m_buf.text;
        }
        std::wstring result;
        result.reserve(m_buf.text.size() + m_composition.size());
        const std::size_t pos = std::min(m_buf.caret, m_buf.text.size());
        result.append(m_buf.text, 0, pos);
        result.append(m_composition);
        result.append(m_buf.text, pos, m_buf.text.size() - pos);
        return result;
    }

    IDWriteTextLayout* InlineTextEditor::EnsureLayout(IDWriteFactory* dwrite, int dpi) const
    {
        if (dwrite == nullptr)
        {
            return nullptr;
        }
        std::wstring composed = ComposedDisplayText();
        const std::size_t compCaret = m_composition.empty() ? 0 : m_buf.caret;
        const std::size_t compLen = m_composition.size();

        if (!m_cachedLayout
            || m_layoutFactory != dwrite
            || m_layoutDpi != dpi
            || m_layoutFontSizeDip != m_item.style.fontSizeDip
            || m_layoutMaxWidth != m_item.textMaxWidth
            || m_layoutCompCaret != compCaret
            || m_layoutCompLen != compLen
            || m_layoutComposedText != composed)
        {
            AnnotationItem preview = m_item;
            preview.text = composed;
            m_cachedLayout = CreateAnnotationTextLayout(dwrite, preview, dpi);
            if (m_cachedLayout && compLen > 0)
            {
                const DWRITE_TEXT_RANGE compRange{
                    static_cast<UINT32>(compCaret),
                    static_cast<UINT32>(compLen),
                };
                m_cachedLayout->SetUnderline(TRUE, compRange);
            }
            m_layoutFactory = dwrite;
            m_layoutDpi = dpi;
            m_layoutFontSizeDip = m_item.style.fontSizeDip;
            m_layoutMaxWidth = m_item.textMaxWidth;
            m_layoutCompCaret = compCaret;
            m_layoutCompLen = compLen;
            m_layoutComposedText = std::move(composed);
        }
        return m_cachedLayout.get();
    }

    D2D1_RECT_F InlineTextEditor::EditorBox(IDWriteFactory* dwrite, int dpi) const
    {
        float textW = 0.0f;
        float textH = ScaleByDpi(m_item.style.fontSizeDip * 1.35f, dpi);
        if (IDWriteTextLayout* layout = EnsureLayout(dwrite, dpi))
        {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics)))
            {
                textW = metrics.widthIncludingTrailingWhitespace;
                textH = std::max(textH, metrics.height);
            }
        }
        const float padL = ScaleByDpi(8.0f, dpi);
        const float padY = ScaleByDpi(6.0f, dpi);
        const float rightExt = std::max(ScaleByDpi(72.0f, dpi), textW + ScaleByDpi(18.0f, dpi));
        return D2D1::RectF(
            m_item.origin.x - padL,
            m_item.origin.y - padY,
            m_item.origin.x + rightExt,
            m_item.origin.y + textH + padY);
    }

    void InlineTextEditor::ComputeCaretGeometry(
        IDWriteFactory* dwrite, int dpi, float& outX, float& outY, float& outHeight) const
    {
        outHeight = ScaleByDpi(m_item.style.fontSizeDip * 1.25f, dpi);
        outX = m_item.origin.x;
        outY = m_item.origin.y;

        IDWriteTextLayout* layout = EnsureLayout(dwrite, dpi);
        if (layout == nullptr)
        {
            return;
        }

        const std::size_t compOffset = m_composition.empty()
            ? 0
            : std::min(m_compCursor, m_composition.size());
        const UINT32 visualPos = static_cast<UINT32>(m_buf.caret + compOffset);
        float x = 0.0f;
        float y = 0.0f;
        DWRITE_HIT_TEST_METRICS metrics{};
        if (SUCCEEDED(layout->HitTestTextPosition(visualPos, FALSE, &x, &y, &metrics)))
        {
            outX = m_item.origin.x + x;
            outY = m_item.origin.y + y;
            if (metrics.height > 1.0f)
            {
                outHeight = metrics.height;
            }
        }
    }

    void InlineTextEditor::UpdateImeCandidatePosition(
        HWND window, IDWriteFactory* dwrite, int dpi) const
    {
        if (!m_active || window == nullptr)
        {
            return;
        }
        HIMC imc = ImmGetContext(window);
        if (imc == nullptr)
        {
            return;
        }
        auto release = wil::scope_exit([window, imc] { ImmReleaseContext(window, imc); });

        float caretX = 0.0f;
        float caretY = 0.0f;
        float caretH = 0.0f;
        ComputeCaretGeometry(dwrite, dpi, caretX, caretY, caretH);

        const LONG fontPx = std::max<LONG>(1, static_cast<LONG>(std::lround(
            ScaleByDpi(m_item.style.fontSizeDip, dpi))));
        LOGFONTW lf{};
        lf.lfHeight = -fontPx;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        ImmSetCompositionFontW(imc, &lf);

        const LONG left = static_cast<LONG>(std::floor(caretX));
        const LONG top = static_cast<LONG>(std::floor(caretY));
        const LONG right = static_cast<LONG>(std::ceil(caretX + std::max(2.0f, ScaleByDpi(2.0f, dpi))));
        const LONG bottom = static_cast<LONG>(std::ceil(caretY + caretH + ScaleByDpi(4.0f, dpi)));

        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = POINT{ left, top };
        ImmSetCompositionWindow(imc, &cf);

        CANDIDATEFORM cand{};
        cand.dwIndex = 0;
        cand.dwStyle = CFS_EXCLUDE;
        cand.ptCurrentPos = POINT{ left, bottom };
        cand.rcArea = RECT{ left, top, right, bottom };
        ImmSetCandidateWindow(imc, &cand);
    }

    TextEditorZone InlineTextEditor::HitTestZone(POINT point, IDWriteFactory* dwrite, int dpi) const
    {
        if (!m_active)
        {
            return TextEditorZone::Outside;
        }
        const D2D1_RECT_F box = EditorBox(dwrite, dpi);
        const D2D1_POINT_2F pt{ static_cast<float>(point.x), static_cast<float>(point.y) };
        const float outerPad = ScaleByDpi(4.0f, dpi);
        if (pt.x < (box.left - outerPad) || pt.x > (box.right + outerPad)
            || pt.y < (box.top - outerPad) || pt.y > (box.bottom + outerPad))
        {
            return TextEditorZone::Outside;
        }

        const float innerInset = ScaleByDpi(2.5f, dpi);
        if (pt.x >= (box.left + innerInset) && pt.x <= (box.right - innerInset)
            && pt.y >= (box.top + innerInset) && pt.y <= (box.bottom - innerInset))
        {
            return TextEditorZone::InsideText;
        }
        return TextEditorZone::Border;
    }

    bool InlineTextEditor::HitTestInside(POINT point, IDWriteFactory* dwrite, int dpi) const
    {
        return HitTestZone(point, dwrite, dpi) != TextEditorZone::Outside;
    }

    std::size_t InlineTextEditor::HitTestTextPositionAt(POINT point, IDWriteFactory* dwrite, int dpi) const
    {
        if (IDWriteTextLayout* layout = EnsureLayout(dwrite, dpi))
        {
            BOOL isTrailing = FALSE;
            BOOL isInside = FALSE;
            DWRITE_HIT_TEST_METRICS metrics{};
            const float localX = static_cast<float>(point.x) - m_item.origin.x;
            const float localY = static_cast<float>(point.y) - m_item.origin.y;
            if (SUCCEEDED(layout->HitTestPoint(localX, localY, &isTrailing, &isInside, &metrics)))
            {
                std::size_t pos = metrics.textPosition + (isTrailing ? metrics.length : 0);
                if (pos > m_buf.caret)
                {
                    pos = (pos >= m_buf.caret + m_composition.size())
                        ? (pos - m_composition.size())
                        : m_buf.caret;
                }
                return std::min(pos, m_buf.text.size());
            }
        }
        return m_buf.caret;
    }

    bool InlineTextEditor::OnLeftButtonDown(
        HWND window, POINT point, IDWriteFactory* dwrite, int dpi, KeyModifiers mods)
    {
        const TextEditorZone zone = HitTestZone(point, dwrite, dpi);
        if (zone == TextEditorZone::Outside)
        {
            return false;
        }
        if (zone == TextEditorZone::Border)
        {
            m_clickTracker.Reset();
            m_draggingBox = true;
            m_selectingText = false;
            m_dragStartPt = point;
            m_dragStartOrigin = m_item.origin;
            return true;
        }

        if (!m_composition.empty())
        {
            CancelActiveImeComposition(window);
            m_composition.clear();
            m_compCursor = 0;
            InvalidateLayout();
        }

        const int clicks = m_clickTracker.RegisterClick(point);
        const std::size_t pos = HitTestTextPositionAt(point, dwrite, dpi);
        m_draggingBox = false;
        m_selectingText = true;
        m_buf.BreakUndoCoalescing();
        m_granularSelect.BeginClick(m_buf.text, pos, clicks, mods.shift, m_buf.anchor, m_buf.caret);

        UpdateImeCandidatePosition(window, dwrite, dpi);
        return true;
    }

    bool InlineTextEditor::OnMouseMove(
        HWND window, POINT point, IDWriteFactory* dwrite, int dpi)
    {
        if (!m_active)
        {
            return false;
        }
        if (m_draggingBox)
        {
            m_item.origin.x = m_dragStartOrigin.x + static_cast<float>(point.x - m_dragStartPt.x);
            m_item.origin.y = m_dragStartOrigin.y + static_cast<float>(point.y - m_dragStartPt.y);
            UpdateImeCandidatePosition(window, dwrite, dpi);
            return true;
        }
        if (m_selectingText)
        {
            const std::size_t pos = HitTestTextPositionAt(point, dwrite, dpi);
            m_buf.BreakUndoCoalescing();
            m_granularSelect.ExtendTo(m_buf.text, pos, m_buf.anchor, m_buf.caret);
            UpdateImeCandidatePosition(window, dwrite, dpi);
            return true;
        }
        return false;
    }

    bool InlineTextEditor::OnLeftButtonUp() noexcept
    {
        const bool wasDragging = m_draggingBox || m_selectingText;
        m_draggingBox = false;
        m_selectingText = false;
        return wasDragging;
    }

    bool InlineTextEditor::ShowContextMenu(HWND window, POINT point, IDWriteFactory* dwrite, int dpi)
    {
        if (!HitTestInside(point, dwrite, dpi))
        {
            return false;
        }
        OnLeftButtonUp();

        const std::size_t pos = HitTestTextPositionAt(point, dwrite, dpi);
        const auto [selS, selE] = SelectionRange();
        if (!HasSelection() || pos < selS || pos > selE)
        {
            m_buf.CollapseTo(pos, false);
            UpdateImeCandidatePosition(window, dwrite, dpi);
        }

        const TextContextCommand cmd = ShowTextContextMenu(
            window, point, /*editable=*/true, HasSelection(), !m_buf.text.empty(),
            /*showUndoRedo=*/true, !m_buf.undoStack.empty(), !m_buf.redoStack.empty());

        switch (cmd)
        {
        case TextContextCommand::Undo:
            if (m_buf.Undo())
            {
                m_item.text = m_buf.text;
            }
            break;
        case TextContextCommand::Redo:
            if (m_buf.Redo())
            {
                m_item.text = m_buf.text;
            }
            break;
        case TextContextCommand::Cut:
            if (HasSelection())
            {
                CopyTextToClipboard(window, m_buf.SelectedText());
                m_buf.ReplaceSelection({}, true, false);
                m_item.text = m_buf.text;
            }
            break;
        case TextContextCommand::Copy:
            if (HasSelection())
            {
                CopyTextToClipboard(window, m_buf.SelectedText());
            }
            break;
        case TextContextCommand::Paste:
        {
            const std::wstring clip = ReadTextFromClipboard(window, true);
            if (!clip.empty())
            {
                m_buf.ReplaceSelection(clip, true, false);
                m_item.text = m_buf.text;
            }
            break;
        }
        case TextContextCommand::Delete:
            if (HasSelection())
            {
                m_buf.ReplaceSelection({}, true, false);
                m_item.text = m_buf.text;
            }
            break;
        case TextContextCommand::SelectAll:
            m_buf.SelectAll();
            break;
        case TextContextCommand::None:
        default:
            break;
        }

        UpdateImeCandidatePosition(window, dwrite, dpi);
        return true;
    }

    bool InlineTextEditor::OnKeyDown(
        HWND window, WPARAM key, IDWriteFactory* dwrite, int dpi,
        bool& committed, bool& needsRender, KeyModifiers mods)
    {
        committed = false;
        needsRender = false;
        if (!m_active)
        {
            return false;
        }

        const bool shift = mods.shift;
        const bool ctrl = mods.ctrl;

        if (key == VK_ESCAPE)
        {
            Cancel(window);
            needsRender = true;
            return true;
        }

        if (key == VK_RETURN)
        {
            if (shift)
            {
                m_buf.ReplaceSelection(L"\n", true, false);
                m_item.text = m_buf.text;
                UpdateImeCandidatePosition(window, dwrite, dpi);
                needsRender = true;
            }
            else
            {
                committed = true;
            }
            return true;
        }

        if (ctrl && key == 'Z')
        {
            const bool ok = shift ? m_buf.Redo() : m_buf.Undo();
            if (ok)
            {
                m_item.text = m_buf.text;
                UpdateImeCandidatePosition(window, dwrite, dpi);
                needsRender = true;
            }
            return true;
        }
        if (ctrl && key == 'Y')
        {
            if (m_buf.Redo())
            {
                m_item.text = m_buf.text;
                UpdateImeCandidatePosition(window, dwrite, dpi);
                needsRender = true;
            }
            return true;
        }
        if (ctrl && key == 'A')
        {
            m_buf.SelectAll();
            needsRender = true;
            return true;
        }
        if (ctrl && (key == 'C' || key == 'X'))
        {
            if (HasSelection())
            {
                CopyTextToClipboard(window, m_buf.SelectedText());
                if (key == 'X')
                {
                    m_buf.ReplaceSelection({}, true, false);
                    m_item.text = m_buf.text;
                    UpdateImeCandidatePosition(window, dwrite, dpi);
                    needsRender = true;
                }
            }
            return true;
        }
        if (ctrl && key == 'V')
        {
            const std::wstring clip = ReadTextFromClipboard(window, true);
            if (!clip.empty())
            {
                m_buf.ReplaceSelection(clip, true, false);
                m_item.text = m_buf.text;
                UpdateImeCandidatePosition(window, dwrite, dpi);
                needsRender = true;
            }
            return true;
        }

        switch (key)
        {
        case VK_BACK:
            if (m_buf.DeleteBackward(ctrl))
            {
                m_item.text = m_buf.text;
                needsRender = true;
            }
            UpdateImeCandidatePosition(window, dwrite, dpi);
            return true;

        case VK_DELETE:
            if (m_buf.DeleteForward(ctrl))
            {
                m_item.text = m_buf.text;
                needsRender = true;
            }
            UpdateImeCandidatePosition(window, dwrite, dpi);
            return true;

        case VK_LEFT:
            if (ctrl)
            {
                m_buf.CollapseTo(PrevWordOffset(m_buf.text, m_buf.caret), shift);
            }
            else if (!shift && HasSelection())
            {
                m_buf.CollapseTo(SelectionRange().first, false);
            }
            else
            {
                m_buf.CollapseTo(PrevCodeUnitOffset(m_buf.text, m_buf.caret), shift);
            }
            UpdateImeCandidatePosition(window, dwrite, dpi);
            needsRender = true;
            return true;

        case VK_RIGHT:
            if (ctrl)
            {
                m_buf.CollapseTo(NextWordOffset(m_buf.text, m_buf.caret), shift);
            }
            else if (!shift && HasSelection())
            {
                m_buf.CollapseTo(SelectionRange().second, false);
            }
            else
            {
                m_buf.CollapseTo(NextCodeUnitOffset(m_buf.text, m_buf.caret), shift);
            }
            UpdateImeCandidatePosition(window, dwrite, dpi);
            needsRender = true;
            return true;

        case VK_UP:
        case VK_DOWN:
        {
            std::size_t next = m_buf.caret;
            if (IDWriteTextLayout* layout = EnsureLayout(dwrite, dpi))
            {
                FLOAT x = 0.0f;
                FLOAT y = 0.0f;
                DWRITE_HIT_TEST_METRICS cm{};
                const std::size_t compOffset = m_composition.empty()
                    ? 0
                    : std::min(m_compCursor, m_composition.size());
                const UINT32 visualPos = static_cast<UINT32>(m_buf.caret + compOffset);
                if (SUCCEEDED(layout->HitTestTextPosition(visualPos, FALSE, &x, &y, &cm)))
                {
                    const float lineH = (cm.height > 1.0f)
                        ? cm.height
                        : ScaleByDpi(m_item.style.fontSizeDip * 1.25f, dpi);
                    DWRITE_TEXT_METRICS tm{};
                    layout->GetMetrics(&tm);
                    if (key == VK_UP)
                    {
                        if (y <= 1.0f)
                        {
                            next = 0;
                        }
                        else
                        {
                            BOOL trailing = FALSE;
                            BOOL inside = FALSE;
                            DWRITE_HIT_TEST_METRICS hit{};
                            if (SUCCEEDED(layout->HitTestPoint(x, y - lineH * 0.5f, &trailing, &inside, &hit)))
                            {
                                std::size_t pos = hit.textPosition + (trailing ? hit.length : 0);
                                if (pos > m_buf.caret)
                                {
                                    pos = (pos >= m_buf.caret + m_composition.size())
                                        ? (pos - m_composition.size())
                                        : m_buf.caret;
                                }
                                next = std::min(pos, m_buf.text.size());
                            }
                        }
                    }
                    else
                    {
                        if (y + lineH >= tm.height - 1.0f)
                        {
                            next = m_buf.text.size();
                        }
                        else
                        {
                            BOOL trailing = FALSE;
                            BOOL inside = FALSE;
                            DWRITE_HIT_TEST_METRICS hit{};
                            if (SUCCEEDED(layout->HitTestPoint(x, y + lineH * 1.5f, &trailing, &inside, &hit)))
                            {
                                std::size_t pos = hit.textPosition + (trailing ? hit.length : 0);
                                if (pos > m_buf.caret)
                                {
                                    pos = (pos >= m_buf.caret + m_composition.size())
                                        ? (pos - m_composition.size())
                                        : m_buf.caret;
                                }
                                next = std::min(pos, m_buf.text.size());
                            }
                        }
                    }
                }
            }
            m_buf.CollapseTo(next, shift);
            UpdateImeCandidatePosition(window, dwrite, dpi);
            needsRender = true;
            return true;
        }

        case VK_HOME:
            m_buf.CollapseTo(ctrl ? 0 : LineRangeAt(m_buf.text, m_buf.caret).first, shift);
            UpdateImeCandidatePosition(window, dwrite, dpi);
            needsRender = true;
            return true;

        case VK_END:
            m_buf.CollapseTo(ctrl ? m_buf.text.size() : LineRangeAt(m_buf.text, m_buf.caret).second, shift);
            UpdateImeCandidatePosition(window, dwrite, dpi);
            needsRender = true;
            return true;

        default:
            return false;
        }
    }

    bool InlineTextEditor::OnChar(HWND window, wchar_t ch, IDWriteFactory* dwrite, int dpi)
    {
        if (!m_active)
        {
            return false;
        }
        if (ch < 32 || ch == 127)
        {
            return true;
        }
        m_buf.ReplaceSelection(std::wstring_view(&ch, 1), true, true);
        m_item.text = m_buf.text;
        UpdateImeCandidatePosition(window, dwrite, dpi);
        return true;
    }

    bool InlineTextEditor::OnImeStartComposition(
        HWND window, IDWriteFactory* dwrite, int dpi) const
    {
        if (!m_active)
        {
            return false;
        }
        UpdateImeCandidatePosition(window, dwrite, dpi);
        return true;
    }

    bool InlineTextEditor::OnImeComposition(
        HWND window, LPARAM lparam, IDWriteFactory* dwrite, int dpi)
    {
        if (!m_active)
        {
            return false;
        }

        if (lparam == 0)
        {
            m_composition.clear();
            m_compCursor = 0;
            UpdateImeCandidatePosition(window, dwrite, dpi);
            return true;
        }

        HIMC imc = ImmGetContext(window);
        if (imc == nullptr)
        {
            return false;
        }
        auto release = wil::scope_exit([window, imc] { ImmReleaseContext(window, imc); });

        const auto readImeString = [imc](DWORD flag) -> std::wstring
        {
            const LONG bytes = ImmGetCompositionStringW(imc, flag, nullptr, 0);
            if (bytes <= 0)
            {
                return {};
            }
            std::wstring out(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(imc, flag, out.data(), static_cast<DWORD>(bytes));
            return out;
        };

        if (lparam & GCS_RESULTSTR)
        {
            const std::wstring raw = readImeString(GCS_RESULTSTR);
            std::wstring clean;
            clean.reserve(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                const wchar_t ch = raw[i];
                if (ch == L'\r')
                {
                    clean.push_back(L'\n');
                    if (i + 1 < raw.size() && raw[i + 1] == L'\n')
                    {
                        ++i;
                    }
                }
                else if (ch == L'\n' || ch == L'\t' || (ch >= 32 && ch != 127))
                {
                    clean.push_back(ch);
                }
            }
            m_composition.clear();
            m_compCursor = 0;
            if (!clean.empty())
            {
                m_buf.ReplaceSelection(clean, true, false);
                m_item.text = m_buf.text;
            }
        }

        if (lparam & GCS_COMPSTR)
        {
            if (HasSelection())
            {
                m_buf.ReplaceSelection({}, true, false);
                m_item.text = m_buf.text;
            }
            m_composition = readImeString(GCS_COMPSTR);
            m_compCursor = m_composition.size();
            if ((lparam & GCS_CURSORPOS) && !m_composition.empty())
            {
                const LONG cursor = ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0);
                if (cursor >= 0)
                {
                    m_compCursor = std::min<std::size_t>(
                        static_cast<std::size_t>(cursor), m_composition.size());
                }
            }
        }

        UpdateImeCandidatePosition(window, dwrite, dpi);
        return true;
    }

    bool InlineTextEditor::OnImeEndComposition(HWND window) noexcept
    {
        (void)window;
        if (!m_active)
        {
            return false;
        }
        m_composition.clear();
        m_compCursor = 0;
        return true;
    }

    void InlineTextEditor::Draw(
        ID2D1Factory1* factory, ID2D1DeviceContext* context,
        IDWriteFactory* dwrite, int dpi,
        const HudBrushes& brushes) const
    {
        if (!m_active || factory == nullptr || context == nullptr || dwrite == nullptr)
        {
            return;
        }

        if (m_strokeFactory != factory || !m_dashStroke)
        {
            const D2D1_STROKE_STYLE_PROPERTIES dashProps = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
            m_dashStroke = nullptr;
            AGSHOT_CHECK_HR(factory->CreateStrokeStyle(dashProps, nullptr, 0, m_dashStroke.put()));
            m_strokeFactory = factory;
        }

        const auto colorBrush = [&](const D2D1_COLOR_F& c) noexcept -> ID2D1SolidColorBrush*
        {
            return brushes.WithColor(c);
        };
        ID2D1SolidColorBrush* whiteBrush = brushes.handleFill.get();

        // Single EnsureLayout() reused across EditorBox, DrawTextLayout, and Caret!
        IDWriteTextLayout* layout = EnsureLayout(dwrite, dpi);
        const D2D1_RECT_F box = EditorBox(dwrite, dpi);

        const float boxRadius = ScaleByDpi(3.0f, dpi);
        const D2D1_ROUNDED_RECT boxRR = D2D1::RoundedRect(box, boxRadius, boxRadius);
        context->FillRoundedRectangle(boxRR, colorBrush(D2D1::ColorF(0.06f, 0.06f, 0.08f, 0.28f)));
        context->DrawRoundedRectangle(
            boxRR,
            colorBrush(D2D1::ColorF(0.18f, 0.49f, 0.94f, 0.95f)),
            ScaleByDpi(1.5f, dpi),
            m_dashStroke.get());

        if (layout != nullptr)
        {
            bool appliedSelectionEffect = false;
            DWRITE_TEXT_RANGE selRange{};

            if (HasSelection() && m_composition.empty())
            {
                const auto [selStart, selEnd] = SelectionRange();
                selRange = DWRITE_TEXT_RANGE{
                    static_cast<UINT32>(selStart),
                    static_cast<UINT32>(selEnd - selStart)
                };
                UINT32 hitCount = 0;
                layout->HitTestTextRange(
                    selRange.startPosition, selRange.length,
                    m_item.origin.x, m_item.origin.y, nullptr, 0, &hitCount);
                if (hitCount > 0)
                {
                    m_hitScratch.resize(hitCount);
                    if (SUCCEEDED(layout->HitTestTextRange(
                        selRange.startPosition, selRange.length,
                        m_item.origin.x, m_item.origin.y, m_hitScratch.data(), hitCount, &hitCount)))
                    {
                        ID2D1SolidColorBrush* selBg = colorBrush(D2D1::ColorF(0.18f, 0.49f, 0.94f, 0.85f));
                        for (UINT32 i = 0; i < hitCount; ++i)
                        {
                            const D2D1_RECT_F r = D2D1::RectF(
                                m_hitScratch[i].left, m_hitScratch[i].top,
                                m_hitScratch[i].left + m_hitScratch[i].width,
                                m_hitScratch[i].top + m_hitScratch[i].height);
                            context->FillRectangle(r, selBg);
                        }
                    }
                }
                layout->SetDrawingEffect(whiteBrush, selRange);
                appliedSelectionEffect = true;
            }

            if (!m_layoutComposedText.empty())
            {
                context->DrawTextLayout(
                    m_item.origin, layout, colorBrush(m_item.style.color),
                    D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            }

            if (appliedSelectionEffect)
            {
                layout->SetDrawingEffect(nullptr, selRange);
            }
        }

        float caretX = 0.0f;
        float caretY = 0.0f;
        float caretH = 0.0f;
        ComputeCaretGeometry(dwrite, dpi, caretX, caretY, caretH);
        const float caretW = std::max(1.5f, ScaleByDpi(1.75f, dpi));
        const float lum = RelativeLuminance(m_item.style.color);
        context->FillRectangle(
            D2D1::RectF(caretX - 0.75f, caretY - 0.5f, caretX + caretW + 0.75f, caretY + caretH + 0.5f),
            colorBrush(lum < 0.25f
                ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.75f)
                : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f)));
        context->FillRectangle(
            D2D1::RectF(caretX, caretY, caretX + caretW, caretY + caretH),
            colorBrush(m_item.style.color));
    }
}
