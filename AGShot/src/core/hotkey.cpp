#include "pch.h"

#include "core/hotkey.h"

#include <cstddef>

namespace agshot
{
    namespace
    {
        struct NamedKey
        {
            const wchar_t* name;
            unsigned code;
        };

        struct NamedModifier
        {
            const wchar_t* name;
            unsigned flag;
        };

        // The spellings a person would actually type. The first entry for a code
        // is the one FormatHotkey() writes back, the rest are aliases.
        constexpr NamedKey kNamedKeys[] =
        {
            { L"Backspace",   VK_BACK },
            { L"Tab",         VK_TAB },
            { L"Enter",       VK_RETURN },
            { L"Return",      VK_RETURN },
            { L"Esc",         VK_ESCAPE },
            { L"Escape",      VK_ESCAPE },
            { L"Space",       VK_SPACE },
            { L"PageUp",      VK_PRIOR },
            { L"PgUp",        VK_PRIOR },
            { L"PageDown",    VK_NEXT },
            { L"PgDn",        VK_NEXT },
            { L"End",         VK_END },
            { L"Home",        VK_HOME },
            { L"Left",        VK_LEFT },
            { L"Up",          VK_UP },
            { L"Right",       VK_RIGHT },
            { L"Down",        VK_DOWN },
            { L"Insert",      VK_INSERT },
            { L"Ins",         VK_INSERT },
            { L"Delete",      VK_DELETE },
            { L"Del",         VK_DELETE },
            { L"PrintScreen", VK_SNAPSHOT },
            { L"PrtSc",       VK_SNAPSHOT },
            { L"CapsLock",    VK_CAPITAL },
            { L"NumLock",     VK_NUMLOCK },
            { L"ScrollLock",  VK_SCROLL },
            { L"Pause",       VK_PAUSE },
        };

        constexpr NamedModifier kNamedModifiers[] =
        {
            { L"Ctrl",    MOD_CONTROL },
            { L"Control", MOD_CONTROL },
            { L"Shift",   MOD_SHIFT },
            { L"Alt",     MOD_ALT },
            { L"Win",     MOD_WIN },
            { L"Windows", MOD_WIN },
        };

        std::wstring_view Trim(std::wstring_view text) noexcept
        {
            while (!text.empty() && (text.front() == L' ' || text.front() == L'\t'))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        // Ordinal and case-insensitive on purpose: a hotkey name is not text, and
        // the Turkish dotless i has no business deciding whether "Ins" means the
        // Insert key.
        bool SameName(std::wstring_view text, const wchar_t* name) noexcept
        {
            return !text.empty()
                && CompareStringOrdinal(text.data(), static_cast<int>(text.size()),
                                        name, -1, TRUE) == CSTR_EQUAL;
        }

        bool LookupKey(std::wstring_view text, unsigned& code) noexcept
        {
            for (const NamedKey& entry : kNamedKeys)
            {
                if (SameName(text, entry.name))
                {
                    code = entry.code;
                    return true;
                }
            }

            // F1 to F24, written as they are spoken.
            if (text.size() >= 2 && (text.front() == L'F' || text.front() == L'f'))
            {
                unsigned number = 0;
                bool digits = true;
                for (std::size_t i = 1; i < text.size(); ++i)
                {
                    if (text[i] < L'0' || text[i] > L'9')
                    {
                        digits = false;
                        break;
                    }
                    number = number * 10 + static_cast<unsigned>(text[i] - L'0');
                }
                if (digits && number >= 1 && number <= 24)
                {
                    code = VK_F1 + (number - 1);
                    return true;
                }
            }

            // A letter or a digit is its own virtual-key code.
            if (text.size() == 1)
            {
                const wchar_t c = text.front();
                if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))
                {
                    code = static_cast<unsigned>(c >= L'a' ? c - L'a' + L'A' : c);
                    return true;
                }
                if (c >= L'0' && c <= L'9')
                {
                    code = static_cast<unsigned>(c);
                    return true;
                }
            }

            return false;
        }

        // The canonical spelling of one key. A code outside these tables cannot
        // come out of ParseHotkey, but Hotkey has public members, so an unnamed
        // code still has to print as something rather than vanish from the file.
        std::wstring KeyText(unsigned code)
        {
            for (const NamedKey& entry : kNamedKeys)
            {
                if (entry.code == code)
                {
                    return entry.name;
                }
            }
            if (code >= VK_F1 && code <= VK_F24)
            {
                return L"F" + std::to_wstring(code - VK_F1 + 1);
            }
            if ((code >= L'A' && code <= L'Z') || (code >= L'0' && code <= L'9'))
            {
                return std::wstring(1, static_cast<wchar_t>(code));
            }

            wchar_t buffer[16]{};
            swprintf_s(buffer, L"0x%02X", code);
            return buffer;
        }
    }

    Hotkey DefaultHotkey() noexcept
    {
        return Hotkey{ 0, VK_F1 };
    }

    bool ParseHotkey(std::wstring_view text, Hotkey& hotkey, std::wstring& error)
    {
        hotkey = Hotkey{};
        error.clear();

        std::wstring_view rest = Trim(text);
        if (rest.empty())
        {
            error = L"a hotkey cannot be empty; write one key, with modifiers if you want them, "
                    L"such as \"F1\" or \"Ctrl+Shift+A\"";
            return false;
        }

        bool haveKey = false;

        while (!rest.empty())
        {
            const std::size_t plus = rest.find(L'+');
            const std::wstring_view part =
                Trim(plus == std::wstring_view::npos ? rest : rest.substr(0, plus));
            rest = plus == std::wstring_view::npos
                 ? std::wstring_view{}
                 : Trim(rest.substr(plus + 1));

            if (part.empty())
            {
                error = L"there is an empty part in the middle; write it as \"Ctrl+Shift+A\"";
                return false;
            }

            bool isModifier = false;
            for (const NamedModifier& modifier : kNamedModifiers)
            {
                if (SameName(part, modifier.name))
                {
                    if ((hotkey.modifiers & modifier.flag) != 0)
                    {
                        error = L"the modifier \"" + std::wstring{ part } + L"\" is named twice";
                        return false;
                    }
                    hotkey.modifiers |= modifier.flag;
                    isModifier = true;
                    break;
                }
            }
            if (isModifier)
            {
                continue;
            }

            if (haveKey)
            {
                error = L"\"" + std::wstring{ part }
                      + L"\" is a second key; a hotkey is any number of modifiers and exactly one key";
                return false;
            }

            unsigned code = 0;
            if (!LookupKey(part, code))
            {
                error = L"\"" + std::wstring{ part }
                      + L"\" is not a key AGShot knows; try a function key, a letter, a digit, "
                        L"or a name such as \"PrintScreen\"";
                return false;
            }

            hotkey.key = code;
            haveKey = true;
        }

        if (!haveKey)
        {
            error = L"a hotkey needs a key as well as its modifiers";
            return false;
        }

        return true;
    }

    std::wstring FormatHotkey(const Hotkey& hotkey)
    {
        std::wstring text;

        const auto append = [&text](const std::wstring& part)
        {
            if (!text.empty())
            {
                text += L'+';
            }
            text += part;
        };

        if ((hotkey.modifiers & MOD_CONTROL) != 0) { append(L"Ctrl"); }
        if ((hotkey.modifiers & MOD_SHIFT) != 0) { append(L"Shift"); }
        if ((hotkey.modifiers & MOD_ALT) != 0) { append(L"Alt"); }
        if ((hotkey.modifiers & MOD_WIN) != 0) { append(L"Win"); }
        if (hotkey.key != 0) { append(KeyText(hotkey.key)); }

        return text;
    }
}
