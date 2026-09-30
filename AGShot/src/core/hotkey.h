#pragma once

#include <string>
#include <string_view>

namespace agshot
{
    // A key combination in the form RegisterHotKey() wants it: the MOD_* flags
    // in "modifiers" and one virtual-key code in "key". The parsed value is kept
    // apart from the text the file stores, so the spelling in the file can be
    // forgiving while the value itself stays exact.
    struct Hotkey
    {
        unsigned modifiers = 0;   // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
        unsigned key = 0;         // a virtual-key code; 0 means "no key at all"

        bool valid() const noexcept { return key != 0; }
    };

    inline bool operator==(const Hotkey& left, const Hotkey& right) noexcept
    {
        return left.modifiers == right.modifiers && left.key == right.key;
    }

    inline bool operator!=(const Hotkey& left, const Hotkey& right) noexcept
    {
        return !(left == right);
    }

    // The combination AGShot uses until the file says otherwise.
    Hotkey DefaultHotkey() noexcept;

    // Reads "F1", "Ctrl+Shift+A", "alt + win + PrintScreen". Modifiers come first
    // in any order, then exactly one key; spaces and case are ignored.
    //
    // On failure it returns false and fills "error" with a reason that names the
    // part it did not like, because "invalid hotkey" would tell the user nothing
    // about how to fix it.
    bool ParseHotkey(std::wstring_view text, Hotkey& hotkey, std::wstring& error);

    // The canonical spelling, which is what gets written back to the file.
    std::wstring FormatHotkey(const Hotkey& hotkey);
}
