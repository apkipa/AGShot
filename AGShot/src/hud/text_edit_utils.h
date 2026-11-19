#pragma once

#include "core/clipboard.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agshot
{
    struct KeyModifiers
    {
        bool shift{};
        bool alt{};
        bool ctrl{};
    };

    KeyModifiers QueryCurrentKeyModifiers() noexcept;

    enum class TextCharClass
    {
        LineBreak,
        Whitespace,
        Word,
        Cjk,
        Punctuation,
    };

    enum class TextSelectGranularity
    {
        Char = 1,
        Word = 2,
        Line = 3,
    };

    enum class TextContextCommand
    {
        None = 0,
        Undo = 1,
        Redo = 2,
        Cut = 3,
        Copy = 4,
        Paste = 5,
        Delete = 6,
        SelectAll = 7,
    };

    std::size_t PrevCodeUnitOffset(std::wstring_view text, std::size_t pos) noexcept;
    std::size_t NextCodeUnitOffset(std::wstring_view text, std::size_t pos) noexcept;
    char32_t DecodeCodePointAt(std::wstring_view text, std::size_t pos, std::size_t& outNext) noexcept;
    TextCharClass ClassifyCodePoint(char32_t cp) noexcept;

    std::pair<std::size_t, std::size_t> WordRangeAt(std::wstring_view text, std::size_t index) noexcept;
    std::pair<std::size_t, std::size_t> LineRangeAt(std::wstring_view text, std::size_t index) noexcept;
    std::size_t PrevWordOffset(std::wstring_view text, std::size_t pos) noexcept;
    std::size_t NextWordOffset(std::wstring_view text, std::size_t pos) noexcept;

    struct MultiClickTracker
    {
        ULONGLONG lastMs{};
        POINT lastPt{};
        int count{};

        void Reset() noexcept;
        int RegisterClick(POINT pt, ULONGLONG nowMs = GetTickCount64()) noexcept;
    };

    // Shared 1/2/3-click character/word/line selection anchor & drag tracker used by
    // both InlineTextEditor and OcrTextPanel.
    struct TextGranularSelection
    {
        TextSelectGranularity granularity{ TextSelectGranularity::Char };
        std::size_t anchorStart{};
        std::size_t anchorEnd{};

        void Reset() noexcept;
        void BeginClick(std::wstring_view text, std::size_t pos, int clicks,
                        bool extendSelection, std::size_t& ioAnchor, std::size_t& ioCaret) noexcept;
        void ExtendTo(std::wstring_view text, std::size_t pos,
                      std::size_t& ioAnchor, std::size_t& ioCaret) const noexcept;
    };

    std::wstring ReadTextFromClipboard(HWND owner, bool normalizeNewlines);
    bool CanPasteTextFromClipboard() noexcept;

    TextContextCommand ShowTextContextMenu(HWND window, POINT clientPoint,
                                           bool editable, bool hasSelection,
                                           bool hasText, bool showUndoRedo = false,
                                           bool canUndo = false, bool canRedo = false) noexcept;

    struct TextEditBuffer
    {
        static constexpr std::size_t kMaxUndoDepth = 128;

        struct Snapshot
        {
            std::wstring text;
            std::size_t caret{};
            std::size_t anchor{};
        };

        std::wstring text;
        std::size_t caret{};
        std::size_t anchor{};
        std::vector<Snapshot> undoStack;
        std::vector<Snapshot> redoStack;
        bool coalescingTyping{};

        void Clear() noexcept;
        void SetText(std::wstring s, bool selectAll) noexcept;

        bool HasSelection() const noexcept { return caret != anchor; }
        std::pair<std::size_t, std::size_t> SelectionRange() const noexcept;
        std::wstring_view SelectedText() const noexcept;

        void BreakUndoCoalescing() noexcept { coalescingTyping = false; }
        void SelectAll() noexcept;
        void CollapseTo(std::size_t pos, bool extendSelection) noexcept;

        void PushUndo();
        bool Undo() noexcept;
        bool Redo() noexcept;

        void ReplaceSelection(std::wstring_view replacement, bool recordUndo,
                              bool coalesceSingleChar = false);
        bool DeleteBackward(bool wordMode);
        bool DeleteForward(bool wordMode);
    };
}
