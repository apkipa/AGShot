#include "pch.h"

#include "core/text.h"

namespace agshot
{
    std::string ToUtf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }

        const int size = WideCharToMultiByte(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            return {};
        }

        std::string out(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    std::wstring FromUtf8(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }

        const int size = MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0)
        {
            return {};
        }

        std::wstring out(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
        return out;
    }
}
