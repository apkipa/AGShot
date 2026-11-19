#include "pch.h"

#include "hud/text_edit_utils.h"

#include <cwctype>

namespace agshot
{
    KeyModifiers QueryCurrentKeyModifiers() noexcept
    {
        KeyModifiers mods{};
        mods.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        mods.alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
        mods.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        return mods;
    }

    std::size_t PrevCodeUnitOffset(std::wstring_view text, std::size_t pos) noexcept
    {
        pos = std::min(pos, text.size());
        if (pos == 0)
        {
            return 0;
        }
        --pos;
        if (pos > 0 && IS_LOW_SURROGATE(text[pos]) && IS_HIGH_SURROGATE(text[pos - 1]))
        {
            --pos;
        }
        return pos;
    }

    std::size_t NextCodeUnitOffset(std::wstring_view text, std::size_t pos) noexcept
    {
        if (pos >= text.size())
        {
            return text.size();
        }
        if (pos + 1 < text.size()
            && IS_HIGH_SURROGATE(text[pos])
            && IS_LOW_SURROGATE(text[pos + 1]))
        {
            return pos + 2;
        }
        return pos + 1;
    }

    char32_t DecodeCodePointAt(std::wstring_view text, std::size_t pos,
                              std::size_t& outNext) noexcept
    {
        if (pos >= text.size())
        {
            outNext = text.size();
            return 0;
        }
        if (pos + 1 < text.size()
            && IS_HIGH_SURROGATE(text[pos])
            && IS_LOW_SURROGATE(text[pos + 1]))
        {
            outNext = pos + 2;
            const char32_t hi = static_cast<char32_t>(text[pos] - 0xD800);
            const char32_t lo = static_cast<char32_t>(text[pos + 1] - 0xDC00);
            return 0x10000u + ((hi << 10) | lo);
        }
        outNext = pos + 1;
        return static_cast<char32_t>(text[pos]);
    }

    TextCharClass ClassifyCodePoint(char32_t cp) noexcept
    {
        if (cp == L'\r' || cp == L'\n')
        {
            return TextCharClass::LineBreak;
        }
        if (cp == L' ' || cp == L'\t' || cp == L'\v' || cp == L'\f'
            || cp == 0x00A0u || cp == 0x3000u)
        {
            return TextCharClass::Whitespace;
        }
        if ((cp >= 0x3400u && cp <= 0x4DBFu)
            || (cp >= 0x4E00u && cp <= 0x9FFFu)
            || (cp >= 0x3040u && cp <= 0x30FFu)
            || (cp >= 0xAC00u && cp <= 0xD7AFu)
            || (cp >= 0xF900u && cp <= 0xFAFFu)
            || (cp >= 0x20000u && cp <= 0x2FA1Fu))
        {
            return TextCharClass::Cjk;
        }
        if (cp == L'_'
            || (cp >= L'0' && cp <= L'9')
            || (cp >= L'A' && cp <= L'Z')
            || (cp >= L'a' && cp <= L'z')
            || (cp < 0x3000u && std::iswalnum(static_cast<wint_t>(cp)) != 0))
        {
            return TextCharClass::Word;
        }
        return TextCharClass::Punctuation;
    }

    std::pair<std::size_t, std::size_t> WordRangeAt(
        std::wstring_view text, std::size_t index) noexcept
    {
        if (text.empty())
        {
            return { 0, 0 };
        }

        std::size_t probe = std::min(index, text.size());
        if (probe >= text.size())
        {
            probe = PrevCodeUnitOffset(text, text.size());
        }
        else if ((text[probe] == L'\n' || text[probe] == L'\r')
            && probe > 0 && text[probe - 1] != L'\n' && text[probe - 1] != L'\r')
        {
            probe = PrevCodeUnitOffset(text, probe);
        }

        std::size_t probeNext = 0;
        const TextCharClass cls = ClassifyCodePoint(DecodeCodePointAt(text, probe, probeNext));
        if (cls == TextCharClass::LineBreak)
        {
            return { probe, probeNext };
        }

        std::size_t start = probe;
        std::size_t dummy = 0;
        while (start > 0)
        {
            const std::size_t prev = PrevCodeUnitOffset(text, start);
            if (ClassifyCodePoint(DecodeCodePointAt(text, prev, dummy)) != cls)
            {
                break;
            }
            start = prev;
        }

        std::size_t end = probeNext;
        while (end < text.size())
        {
            std::size_t next = 0;
            if (ClassifyCodePoint(DecodeCodePointAt(text, end, next)) != cls)
            {
                break;
            }
            end = next;
        }
        return { start, end };
    }

    std::pair<std::size_t, std::size_t> LineRangeAt(
        std::wstring_view text, std::size_t index) noexcept
    {
        std::size_t pos = std::min(index, text.size());
        if (pos > 0 && pos < text.size() && text[pos] == L'\n' && text[pos - 1] == L'\r')
        {
            --pos;
        }
        std::size_t lineStart = pos;
        while (lineStart > 0 && text[lineStart - 1] != L'\n' && text[lineStart - 1] != L'\r')
        {
            --lineStart;
        }
        std::size_t lineEnd = pos;
        while (lineEnd < text.size() && text[lineEnd] != L'\n' && text[lineEnd] != L'\r')
        {
            ++lineEnd;
        }
        return { lineStart, lineEnd };
    }

    std::size_t PrevWordOffset(std::wstring_view text, std::size_t pos) noexcept
    {
        pos = std::min(pos, text.size());
        if (pos == 0)
        {
            return 0;
        }
        std::size_t dummy = 0;
        while (pos > 0)
        {
            const std::size_t prev = PrevCodeUnitOffset(text, pos);
            const TextCharClass cls = ClassifyCodePoint(DecodeCodePointAt(text, prev, dummy));
            if (cls != TextCharClass::Whitespace && cls != TextCharClass::LineBreak)
            {
                break;
            }
            pos = prev;
        }
        if (pos == 0)
        {
            return 0;
        }
        const std::size_t prevAnchor = PrevCodeUnitOffset(text, pos);
        const TextCharClass targetCls = ClassifyCodePoint(DecodeCodePointAt(text, prevAnchor, dummy));
        while (pos > 0)
        {
            const std::size_t prev = PrevCodeUnitOffset(text, pos);
            if (ClassifyCodePoint(DecodeCodePointAt(text, prev, dummy)) != targetCls)
            {
                break;
            }
            pos = prev;
        }
        return pos;
    }

    std::size_t NextWordOffset(std::wstring_view text, std::size_t pos) noexcept
    {
        pos = std::min(pos, text.size());
        if (pos >= text.size())
        {
            return text.size();
        }
        std::size_t next = pos;
        const TextCharClass firstCls = ClassifyCodePoint(DecodeCodePointAt(text, pos, next));
        if (firstCls == TextCharClass::LineBreak)
        {
            return next;
        }
        if (firstCls != TextCharClass::Whitespace)
        {
            while (pos < text.size())
            {
                if (ClassifyCodePoint(DecodeCodePointAt(text, pos, next)) != firstCls)
                {
                    break;
                }
                pos = next;
            }
        }
        while (pos < text.size())
        {
            if (ClassifyCodePoint(DecodeCodePointAt(text, pos, next)) != TextCharClass::Whitespace)
            {
                break;
            }
            pos = next;
        }
        return pos;
    }

    void MultiClickTracker::Reset() noexcept
    {
        lastMs = 0;
        lastPt = POINT{};
        count = 0;
    }

    int MultiClickTracker::RegisterClick(POINT pt, ULONGLONG nowMs) noexcept
    {
        const int tolX = GetSystemMetrics(SM_CXDOUBLECLK) / 2 + 2;
        const int tolY = GetSystemMetrics(SM_CYDOUBLECLK) / 2 + 2;
        const bool rapid = (lastMs != 0)
            && ((nowMs - lastMs) <= GetDoubleClickTime())
            && (std::abs(static_cast<int>(pt.x - lastPt.x)) <= tolX)
            && (std::abs(static_cast<int>(pt.y - lastPt.y)) <= tolY);
        count = rapid ? ((count % 3) + 1) : 1;
        lastMs = nowMs;
        lastPt = pt;
        return count;
    }

    void TextGranularSelection::Reset() noexcept
    {
        granularity = TextSelectGranularity::Char;
        anchorStart = 0;
        anchorEnd = 0;
    }

    void TextGranularSelection::BeginClick(
        std::wstring_view text, std::size_t pos, int clicks,
        bool extendSelection, std::size_t& ioAnchor, std::size_t& ioCaret) noexcept
    {
        pos = std::min(pos, text.size());
        if (clicks == 2)
        {
            const auto [ws, we] = WordRangeAt(text, pos);
            granularity = TextSelectGranularity::Word;
            anchorStart = ws;
            anchorEnd = we;
            ioAnchor = ws;
            ioCaret = we;
        }
        else if (clicks >= 3)
        {
            const auto [ls, le] = LineRangeAt(text, pos);
            granularity = TextSelectGranularity::Line;
            anchorStart = ls;
            anchorEnd = le;
            ioAnchor = ls;
            ioCaret = le;
        }
        else
        {
            granularity = TextSelectGranularity::Char;
            ioCaret = pos;
            if (!extendSelection)
            {
                ioAnchor = pos;
            }
            anchorStart = ioAnchor;
            anchorEnd = ioAnchor;
        }
    }

    void TextGranularSelection::ExtendTo(
        std::wstring_view text, std::size_t pos,
        std::size_t& ioAnchor, std::size_t& ioCaret) const noexcept
    {
        pos = std::min(pos, text.size());
        if (granularity == TextSelectGranularity::Word)
        {
            const auto [ws, we] = WordRangeAt(text, pos);
            if (ws < anchorStart)
            {
                ioAnchor = anchorEnd;
                ioCaret = ws;
            }
            else
            {
                ioAnchor = anchorStart;
                ioCaret = we;
            }
        }
        else if (granularity == TextSelectGranularity::Line)
        {
            const auto [ls, le] = LineRangeAt(text, pos);
            if (ls < anchorStart)
            {
                ioAnchor = anchorEnd;
                ioCaret = ls;
            }
            else
            {
                ioAnchor = anchorStart;
                ioCaret = le;
            }
        }
        else
        {
            ioCaret = pos;
        }
    }

    bool CopyTextToClipboard(HWND owner, std::wstring_view text) noexcept
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
            void* dst = GlobalLock(mem.get());
            if (dst == nullptr)
            {
                return false;
            }
            std::memcpy(dst, text.data(), text.size() * sizeof(wchar_t));
            static_cast<wchar_t*>(dst)[text.size()] = L'\0';
            GlobalUnlock(mem.get());
        }
        if (OpenClipboard(owner) == 0)
        {
            return false;
        }
        auto close = wil::scope_exit([] { CloseClipboard(); });
        if (EmptyClipboard() == 0)
        {
            return false;
        }
        if (SetClipboardData(CF_UNICODETEXT, mem.get()) == nullptr)
        {
            return false;
        }
        mem.release();
        return true;
    }

    std::wstring ReadTextFromClipboard(HWND owner, bool normalizeNewlines)
    {
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || OpenClipboard(owner) == 0)
        {
            return {};
        }
        auto close = wil::scope_exit([] { CloseClipboard(); });
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle == nullptr)
        {
            return {};
        }
        const auto* src = static_cast<const wchar_t*>(GlobalLock(handle));
        if (src == nullptr)
        {
            return {};
        }
        auto unlock = wil::scope_exit([handle] { GlobalUnlock(handle); });
        std::wstring result(src);
        if (!normalizeNewlines)
        {
            return result;
        }
        std::wstring normalized;
        normalized.reserve(result.size());
        for (std::size_t i = 0; i < result.size(); ++i)
        {
            if (result[i] == L'\r')
            {
                if (i + 1 < result.size() && result[i + 1] == L'\n')
                {
                    continue;
                }
                normalized.push_back(L'\n');
            }
            else
            {
                normalized.push_back(result[i]);
            }
        }
        return normalized;
    }

    bool CanPasteTextFromClipboard() noexcept
    {
        return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
    }

    TextContextCommand ShowTextContextMenu(HWND window, POINT clientPoint,
                                           bool editable, bool hasSelection,
                                           bool hasText, bool showUndoRedo,
                                           bool canUndo, bool canRedo) noexcept
    {
        if (window == nullptr)
        {
            return TextContextCommand::None;
        }
        wil::unique_hmenu menu{ CreatePopupMenu() };
        if (!menu)
        {
            return TextContextCommand::None;
        }

        const UINT selFlags = hasSelection ? MF_STRING : (MF_STRING | MF_GRAYED);
        const UINT copyFlags = (editable && showUndoRedo ? hasSelection : (hasSelection || hasText))
            ? MF_STRING
            : (MF_STRING | MF_GRAYED);
        const UINT selectAllFlags = hasText ? MF_STRING : (MF_STRING | MF_GRAYED);

        if (editable)
        {
            if (showUndoRedo)
            {
                AppendMenuW(menu.get(), canUndo ? MF_STRING : (MF_STRING | MF_GRAYED),
                            static_cast<UINT_PTR>(TextContextCommand::Undo), L"Undo");
                AppendMenuW(menu.get(), canRedo ? MF_STRING : (MF_STRING | MF_GRAYED),
                            static_cast<UINT_PTR>(TextContextCommand::Redo), L"Redo");
                AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
            }
            const UINT pasteFlags = CanPasteTextFromClipboard() ? MF_STRING : (MF_STRING | MF_GRAYED);
            AppendMenuW(menu.get(), selFlags, static_cast<UINT_PTR>(TextContextCommand::Cut), L"Cut");
            AppendMenuW(menu.get(), copyFlags, static_cast<UINT_PTR>(TextContextCommand::Copy), L"Copy");
            AppendMenuW(menu.get(), pasteFlags, static_cast<UINT_PTR>(TextContextCommand::Paste), L"Paste");
            AppendMenuW(menu.get(), selFlags, static_cast<UINT_PTR>(TextContextCommand::Delete), L"Delete");
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu.get(), selectAllFlags, static_cast<UINT_PTR>(TextContextCommand::SelectAll), L"Select all");
        }
        else
        {
            AppendMenuW(menu.get(), copyFlags, static_cast<UINT_PTR>(TextContextCommand::Copy), L"Copy");
            AppendMenuW(menu.get(), selectAllFlags, static_cast<UINT_PTR>(TextContextCommand::SelectAll), L"Select all");
        }

        SetForegroundWindow(window);
        POINT screen = clientPoint;
        ClientToScreen(window, &screen);
        const int cmd = TrackPopupMenu(
            menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            screen.x, screen.y, 0, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        SetFocus(window);

        switch (cmd)
        {
        case static_cast<int>(TextContextCommand::Undo):      return TextContextCommand::Undo;
        case static_cast<int>(TextContextCommand::Redo):      return TextContextCommand::Redo;
        case static_cast<int>(TextContextCommand::Cut):       return TextContextCommand::Cut;
        case static_cast<int>(TextContextCommand::Copy):      return TextContextCommand::Copy;
        case static_cast<int>(TextContextCommand::Paste):     return TextContextCommand::Paste;
        case static_cast<int>(TextContextCommand::Delete):    return TextContextCommand::Delete;
        case static_cast<int>(TextContextCommand::SelectAll): return TextContextCommand::SelectAll;
        default:                                              return TextContextCommand::None;
        }
    }

    void TextEditBuffer::Clear() noexcept
    {
        text.clear();
        caret = 0;
        anchor = 0;
        undoStack.clear();
        redoStack.clear();
        coalescingTyping = false;
    }

    void TextEditBuffer::SetText(std::wstring s, bool selectAll) noexcept
    {
        text = std::move(s);
        undoStack.clear();
        redoStack.clear();
        coalescingTyping = false;
        anchor = selectAll ? 0 : text.size();
        caret = text.size();
    }

    std::pair<std::size_t, std::size_t> TextEditBuffer::SelectionRange() const noexcept
    {
        return (caret <= anchor)
            ? std::pair<std::size_t, std::size_t>{ caret, anchor }
            : std::pair<std::size_t, std::size_t>{ anchor, caret };
    }

    std::wstring_view TextEditBuffer::SelectedText() const noexcept
    {
        if (!HasSelection())
        {
            return {};
        }
        const auto [s, e] = SelectionRange();
        return std::wstring_view(text).substr(s, e - s);
    }

    void TextEditBuffer::SelectAll() noexcept
    {
        coalescingTyping = false;
        anchor = 0;
        caret = text.size();
    }

    void TextEditBuffer::CollapseTo(std::size_t pos, bool extendSelection) noexcept
    {
        coalescingTyping = false;
        caret = std::min(pos, text.size());
        if (!extendSelection)
        {
            anchor = caret;
        }
    }

    void TextEditBuffer::PushUndo()
    {
        if (undoStack.size() >= kMaxUndoDepth)
        {
            undoStack.erase(undoStack.begin());
        }
        undoStack.push_back(Snapshot{ text, caret, anchor });
        redoStack.clear();
    }

    bool TextEditBuffer::Undo() noexcept
    {
        coalescingTyping = false;
        if (undoStack.empty())
        {
            return false;
        }
        if (redoStack.size() >= kMaxUndoDepth)
        {
            redoStack.erase(redoStack.begin());
        }
        redoStack.push_back(Snapshot{ std::move(text), caret, anchor });
        Snapshot prev = std::move(undoStack.back());
        undoStack.pop_back();
        text = std::move(prev.text);
        caret = std::min(prev.caret, text.size());
        anchor = std::min(prev.anchor, text.size());
        return true;
    }

    bool TextEditBuffer::Redo() noexcept
    {
        coalescingTyping = false;
        if (redoStack.empty())
        {
            return false;
        }
        if (undoStack.size() >= kMaxUndoDepth)
        {
            undoStack.erase(undoStack.begin());
        }
        undoStack.push_back(Snapshot{ std::move(text), caret, anchor });
        Snapshot next = std::move(redoStack.back());
        redoStack.pop_back();
        text = std::move(next.text);
        caret = std::min(next.caret, text.size());
        anchor = std::min(next.anchor, text.size());
        return true;
    }

    void TextEditBuffer::ReplaceSelection(
        std::wstring_view replacement, bool recordUndo, bool coalesceSingleChar)
    {
        const bool hadSelection = HasSelection();
        const bool isWordChar = coalesceSingleChar
            && !hadSelection
            && replacement.size() == 1
            && replacement[0] != L' '
            && replacement[0] != L'\t'
            && replacement[0] != L'\r'
            && replacement[0] != L'\n';

        if (recordUndo)
        {
            if (!isWordChar || !coalescingTyping)
            {
                PushUndo();
            }
            coalescingTyping = isWordChar;
        }
        else
        {
            coalescingTyping = false;
        }

        const auto [start, end] = SelectionRange();
        text.replace(start, end - start, replacement);
        caret = start + replacement.size();
        anchor = caret;
    }

    bool TextEditBuffer::DeleteBackward(bool wordMode)
    {
        coalescingTyping = false;
        if (HasSelection())
        {
            ReplaceSelection({}, true, false);
            return true;
        }
        if (caret == 0)
        {
            return false;
        }
        const std::size_t prev = wordMode
            ? PrevWordOffset(text, caret)
            : PrevCodeUnitOffset(text, caret);
        if (prev >= caret)
        {
            return false;
        }
        PushUndo();
        text.erase(prev, caret - prev);
        caret = prev;
        anchor = prev;
        return true;
    }

    bool TextEditBuffer::DeleteForward(bool wordMode)
    {
        coalescingTyping = false;
        if (HasSelection())
        {
            ReplaceSelection({}, true, false);
            return true;
        }
        if (caret >= text.size())
        {
            return false;
        }
        const std::size_t next = wordMode
            ? NextWordOffset(text, caret)
            : NextCodeUnitOffset(text, caret);
        if (next <= caret)
        {
            return false;
        }
        PushUndo();
        text.erase(caret, next - caret);
        anchor = caret;
        return true;
    }
}
