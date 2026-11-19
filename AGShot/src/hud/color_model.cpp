#include "pch.h"

#include "hud/color_model.h"

namespace agshot
{
    D2D1_COLOR_F HsvToRgb(float h, float s, float v, float a) noexcept
    {
        const float hh = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
        const int sector = static_cast<int>(std::floor(hh)) % 6;
        const float f = hh - std::floor(hh);
        const float p = v * (1.0f - s);
        const float q = v * (1.0f - f * s);
        const float t = v * (1.0f - (1.0f - f) * s);

        switch (sector)
        {
        case 0: return D2D1::ColorF(v, t, p, a);
        case 1: return D2D1::ColorF(q, v, p, a);
        case 2: return D2D1::ColorF(p, v, t, a);
        case 3: return D2D1::ColorF(p, q, v, a);
        case 4: return D2D1::ColorF(t, p, v, a);
        case 5:
        default: return D2D1::ColorF(v, p, q, a);
        }
    }

    void RgbToHsv(const D2D1_COLOR_F& c, float& h, float& s, float& v) noexcept
    {
        const float maxC = std::max({ c.r, c.g, c.b });
        const float minC = std::min({ c.r, c.g, c.b });
        const float delta = maxC - minC;

        v = maxC;
        s = (maxC > 1e-5f) ? (delta / maxC) : 0.0f;
        if (delta <= 1e-5f)
        {
            return;
        }
        if (maxC == c.r)
        {
            h = 60.0f * std::fmod((c.g - c.b) / delta, 6.0f);
        }
        else if (maxC == c.g)
        {
            h = 60.0f * ((c.b - c.r) / delta + 2.0f);
        }
        else
        {
            h = 60.0f * ((c.r - c.g) / delta + 4.0f);
        }
        if (h < 0.0f)
        {
            h += 360.0f;
        }
    }

    bool SameRgbaColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float eps) noexcept
    {
        return std::fabs(a.r - b.r) < eps
            && std::fabs(a.g - b.g) < eps
            && std::fabs(a.b - b.b) < eps
            && std::fabs(a.a - b.a) < eps;
    }

    int HexDigitValue(wchar_t ch) noexcept
    {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'a' && ch <= L'f') return 10 + (ch - L'a');
        if (ch >= L'A' && ch <= L'F') return 10 + (ch - L'A');
        return -1;
    }

    wchar_t ToUpperHexChar(wchar_t ch) noexcept
    {
        if (ch >= L'a' && ch <= L'f')
        {
            return static_cast<wchar_t>(ch - L'a' + L'A');
        }
        return ch;
    }

    bool ParseHexColorDigits(std::wstring_view s, bool allowShorthand,
                             float fallbackAlpha, D2D1_COLOR_F& outColor,
                             bool& outHadExplicitAlpha) noexcept
    {
        outHadExplicitAlpha = false;
        for (wchar_t ch : s)
        {
            if (HexDigitValue(ch) < 0)
            {
                return false;
            }
        }

        if (s.size() == 6 || s.size() == 8)
        {
            const auto byteAt = [&s](std::size_t idx) -> int
            {
                return HexDigitValue(s[idx]) * 16 + HexDigitValue(s[idx + 1]);
            };
            outColor.r = static_cast<float>(byteAt(0)) / 255.0f;
            outColor.g = static_cast<float>(byteAt(2)) / 255.0f;
            outColor.b = static_cast<float>(byteAt(4)) / 255.0f;
            outHadExplicitAlpha = (s.size() == 8);
            outColor.a = outHadExplicitAlpha ? (static_cast<float>(byteAt(6)) / 255.0f) : fallbackAlpha;
            return true;
        }
        if (allowShorthand && (s.size() == 3 || s.size() == 4))
        {
            const auto byteAt = [&s](std::size_t idx) -> int
            {
                return HexDigitValue(s[idx]) * 17;
            };
            outColor.r = static_cast<float>(byteAt(0)) / 255.0f;
            outColor.g = static_cast<float>(byteAt(1)) / 255.0f;
            outColor.b = static_cast<float>(byteAt(2)) / 255.0f;
            outHadExplicitAlpha = (s.size() == 4);
            outColor.a = outHadExplicitAlpha ? (static_cast<float>(byteAt(3)) / 255.0f) : fallbackAlpha;
            return true;
        }
        return false;
    }

    bool ParseUnsignedIntDigits(std::wstring_view s, int& outVal) noexcept
    {
        if (s.empty())
        {
            return false;
        }
        int val = 0;
        for (wchar_t ch : s)
        {
            if (ch < L'0' || ch > L'9')
            {
                return false;
            }
            val = std::min(10000, val * 10 + (ch - L'0'));
        }
        outVal = val;
        return true;
    }

    std::wstring FormatHexRgbString(const D2D1_COLOR_F& color)
    {
        const int rByte = std::clamp(static_cast<int>(std::lround(color.r * 255.0f)), 0, 255);
        const int gByte = std::clamp(static_cast<int>(std::lround(color.g * 255.0f)), 0, 255);
        const int bByte = std::clamp(static_cast<int>(std::lround(color.b * 255.0f)), 0, 255);
        wchar_t buf[16]{};
        swprintf_s(buf, L"%02X%02X%02X", rByte, gByte, bByte);
        return std::wstring(buf);
    }

    std::wstring FormatAlphaPercentNumber(float alpha)
    {
        const int aPct = std::clamp(static_cast<int>(std::lround(alpha * 100.0f)), 0, 100);
        wchar_t buf[16]{};
        swprintf_s(buf, L"%d", aPct);
        return std::wstring(buf);
    }

    std::wstring SanitizeHexClipboardText(std::wstring_view raw)
    {
        std::wstring trimmed;
        trimmed.reserve(raw.size());
        for (wchar_t ch : raw)
        {
            if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n')
            {
                trimmed.push_back(ch);
            }
        }

        std::size_t start = 0;
        if (!trimmed.empty() && trimmed[0] == L'#')
        {
            start = 1;
        }
        else if (trimmed.size() >= 2 && trimmed[0] == L'0'
                 && (trimmed[1] == L'x' || trimmed[1] == L'X'))
        {
            start = 2;
        }

        std::wstring filtered;
        filtered.reserve(trimmed.size());
        for (std::size_t i = start; i < trimmed.size(); ++i)
        {
            if (HexDigitValue(trimmed[i]) >= 0)
            {
                filtered.push_back(ToUpperHexChar(trimmed[i]));
            }
        }
        return filtered;
    }

    std::wstring SanitizeDigitsClipboardText(std::wstring_view raw)
    {
        std::wstring filtered;
        filtered.reserve(raw.size());
        for (wchar_t ch : raw)
        {
            if (ch >= L'0' && ch <= L'9')
            {
                filtered.push_back(ch);
            }
        }
        return filtered;
    }
}
