#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agshot
{
    // Decodes a binary "AGIP" icon pack (optionally MSZIP-compressed in RCDATA)
    // produced by scripts/bake_icons.mjs so both D2D HUD and WebView2 share one
    // baked single-path SVG resource.
    class IconPack
    {
    public:
        IconPack() = default;
        IconPack(const IconPack&) = delete;
        IconPack& operator=(const IconPack&) = delete;
        IconPack(IconPack&&) = delete;
        IconPack& operator=(IconPack&&) = delete;

        bool LoadFromResource(HMODULE module, UINT resourceId) noexcept;
        bool LoadFromRawPack(const std::uint8_t* data, std::size_t size) noexcept;

        bool empty() const noexcept { return m_entries.empty(); }
        std::size_t count() const noexcept { return m_entries.size(); }

        std::string_view SvgByIndex(std::size_t index) const noexcept;
        std::string_view SvgByName(std::string_view name) const noexcept;

    private:
        struct Entry
        {
            std::size_t nameOffset{};
            std::size_t nameLen{};
            std::size_t svgOffset{};
            std::size_t svgLen{};
        };

        std::vector<std::uint8_t> m_storage;
        std::vector<Entry> m_entries;
        std::unordered_map<std::string_view, std::size_t> m_byName;
    };

    // Process-wide HUD icon pack lazily loaded once from IDR_HUD_ICONS.
    const IconPack& SharedHudIconPack() noexcept;
}
