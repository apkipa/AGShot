#pragma once

#include <windows.h>
#include <objidl.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <winrt/base.h>

namespace agshot
{
    inline constexpr std::size_t kDefaultMaxResourceBytes = 16u * 1024u * 1024u;

    // Loads raw bytes of an RT_RCDATA resource from the given module.
    bool LoadRcData(HMODULE module, UINT resourceId,
                    const void*& outData, DWORD& outSize) noexcept;

    // Decompresses an MSZIP buffer produced by scripts/compress_mszip.ps1.
    bool DecompressMszip(const void* compressedData, DWORD compressedSize,
                         std::vector<std::uint8_t>& out,
                         std::size_t maxOutputBytes = kDefaultMaxResourceBytes) noexcept;

    // Loads and MSZIP-decompresses an RT_RCDATA resource in one step.
    bool LoadMszipResource(HMODULE module, UINT resourceId,
                           std::vector<std::uint8_t>& out,
                           std::size_t maxOutputBytes = kDefaultMaxResourceBytes) noexcept;

    // Creates a read-ready COM IStream backed by an HGLOBAL copy of [data, data + size).
    HRESULT CreateMemoryStream(const void* data, DWORD size,
                               winrt::com_ptr<IStream>& outStream) noexcept;
}
