#pragma once

#include <winrt/base.h>

#include <string>
#include <string_view>

// Thin UTF-8 <-> UTF-16 wrappers backed directly by C++/WinRT (winrt::to_string / winrt::to_hstring).
namespace agshot
{
    inline std::string ToUtf8(std::wstring_view text)
    {
        return winrt::to_string(text);
    }

    inline std::wstring FromUtf8(std::string_view text)
    {
        return std::wstring{ winrt::to_hstring(text) };
    }
}
