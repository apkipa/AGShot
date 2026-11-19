#pragma once

#include <d2d1_1.h>

#include <string>
#include <string_view>

namespace agshot
{
    D2D1_COLOR_F HsvToRgb(float h, float s, float v, float a = 1.0f) noexcept;
    void RgbToHsv(const D2D1_COLOR_F& c, float& h, float& s, float& v) noexcept;
    bool SameRgbaColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float eps = 0.002f) noexcept;

    int HexDigitValue(wchar_t ch) noexcept;
    wchar_t ToUpperHexChar(wchar_t ch) noexcept;

    bool ParseHexColorDigits(std::wstring_view s, bool allowShorthand,
                             float fallbackAlpha, D2D1_COLOR_F& outColor,
                             bool& outHadExplicitAlpha) noexcept;
    bool ParseUnsignedIntDigits(std::wstring_view s, int& outVal) noexcept;

    std::wstring FormatHexRgbString(const D2D1_COLOR_F& color);
    std::wstring FormatAlphaPercentNumber(float alpha);

    std::wstring SanitizeHexClipboardText(std::wstring_view raw);
    std::wstring SanitizeDigitsClipboardText(std::wstring_view raw);
}
