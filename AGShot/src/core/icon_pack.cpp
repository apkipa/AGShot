#include "pch.h"

#include "core/icon_pack.h"

#include "core/embedded_resource.h"
#include "resource.h"

#include <cstring>

namespace agshot
{
    namespace
    {
        std::uint32_t ReadU32Le(const std::uint8_t* ptr) noexcept
        {
            return static_cast<std::uint32_t>(ptr[0])
                | (static_cast<std::uint32_t>(ptr[1]) << 8u)
                | (static_cast<std::uint32_t>(ptr[2]) << 16u)
                | (static_cast<std::uint32_t>(ptr[3]) << 24u);
        }
    }

    bool IconPack::LoadFromResource(HMODULE module, UINT resourceId) noexcept
    {
        std::vector<std::uint8_t> raw;
        if (!LoadMszipResource(module, resourceId, raw))
        {
            return false;
        }
        return LoadFromRawPack(raw.data(), raw.size());
    }

    bool IconPack::LoadFromRawPack(const std::uint8_t* data, std::size_t size) noexcept
    {
        m_storage.clear();
        m_entries.clear();
        m_byName.clear();

        if (data == nullptr || size < 12)
        {
            return false;
        }
        if (std::memcmp(data, "AGIP", 4) != 0)
        {
            return false;
        }
        const std::uint32_t version = ReadU32Le(data + 4);
        const std::uint32_t count = ReadU32Le(data + 8);
        if (version != 1u || count == 0u || count > 4096u)
        {
            return false;
        }

        const auto fail = [&]() noexcept -> bool
        {
            m_storage.clear();
            m_entries.clear();
            m_byName.clear();
            return false;
        };

        try
        {
            m_storage.assign(data, data + size);
            m_entries.reserve(count);
            m_byName.reserve(count);
        }
        catch (...)
        {
            return fail();
        }

        std::size_t pos = 12;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (pos + 8 > m_storage.size())
            {
                return fail();
            }
            const std::uint32_t nameLen = ReadU32Le(m_storage.data() + pos);
            const std::uint32_t svgLen = ReadU32Le(m_storage.data() + pos + 4);
            pos += 8;

            if (nameLen > m_storage.size() - pos)
            {
                return fail();
            }
            const std::size_t nameOff = pos;
            pos += nameLen;

            if (svgLen > m_storage.size() - pos)
            {
                return fail();
            }
            const std::size_t svgOff = pos;
            pos += svgLen;

            m_entries.push_back(Entry{ nameOff, nameLen, svgOff, svgLen });
            const std::string_view nameView(
                reinterpret_cast<const char*>(m_storage.data() + nameOff), nameLen);
            try
            {
                m_byName.emplace(nameView, static_cast<std::size_t>(i));
            }
            catch (...)
            {
                return fail();
            }
        }
        return true;
    }

    std::string_view IconPack::SvgByIndex(std::size_t index) const noexcept
    {
        if (index >= m_entries.size())
        {
            return {};
        }
        const Entry& e = m_entries[index];
        return std::string_view(
            reinterpret_cast<const char*>(m_storage.data() + e.svgOffset), e.svgLen);
    }

    std::string_view IconPack::SvgByName(std::string_view name) const noexcept
    {
        const auto it = m_byName.find(name);
        if (it == m_byName.end())
        {
            return {};
        }
        return SvgByIndex(it->second);
    }

    const IconPack& SharedHudIconPack() noexcept
    {
        static IconPack pack;
        static const bool loaded = pack.LoadFromResource(GetModuleHandleW(nullptr), IDR_HUD_ICONS);
        (void)loaded;
        return pack;
    }
}
