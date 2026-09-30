#pragma once

#include <string>
#include <string_view>

// Windows speaks UTF-16, TOML and most on-disk formats speak UTF-8. These two
// conversions are needed by both the config and the error reporter, so they live
// in one place instead of once per module.
namespace agshot
{
    std::string ToUtf8(std::wstring_view text);
    std::wstring FromUtf8(std::string_view text);
}
