#include "pch.h"

#include "core/embedded_resource.h"

#include <compressapi.h>

#include <cstring>

namespace agshot
{
    bool LoadRcData(HMODULE module, UINT resourceId,
                    const void*& outData, DWORD& outSize) noexcept
    {
        outData = nullptr;
        outSize = 0;

        HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (res == nullptr)
        {
            return false;
        }
        HGLOBAL loaded = LoadResource(module, res);
        if (loaded == nullptr)
        {
            return false;
        }
        const DWORD bytes = SizeofResource(module, res);
        const void* ptr = LockResource(loaded);
        if (ptr == nullptr || bytes == 0)
        {
            return false;
        }
        outData = ptr;
        outSize = bytes;
        return true;
    }

    bool DecompressMszip(const void* compressedData, DWORD compressedSize,
                         std::vector<std::uint8_t>& out,
                         std::size_t maxOutputBytes) noexcept
    {
        out.clear();
        if (compressedData == nullptr || compressedSize == 0)
        {
            return false;
        }

        wil::unique_any<DECOMPRESSOR_HANDLE, decltype(&::CloseDecompressor), ::CloseDecompressor> decompressor;
        if (!CreateDecompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, decompressor.put()))
        {
            return false;
        }

        SIZE_T required = 0;
        if (!Decompress(decompressor.get(), compressedData, compressedSize, nullptr, 0, &required)
            && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return false;
        }
        if (required == 0 || (maxOutputBytes > 0 && required > maxOutputBytes))
        {
            return false;
        }

        try
        {
            out.resize(required);
        }
        catch (...)
        {
            out.clear();
            return false;
        }

        SIZE_T written = 0;
        if (!Decompress(decompressor.get(), compressedData, compressedSize,
                        out.data(), out.size(), &written))
        {
            out.clear();
            return false;
        }
        out.resize(written);
        return !out.empty();
    }

    bool LoadMszipResource(HMODULE module, UINT resourceId,
                           std::vector<std::uint8_t>& out,
                           std::size_t maxOutputBytes) noexcept
    {
        const void* data = nullptr;
        DWORD size = 0;
        if (!LoadRcData(module, resourceId, data, size))
        {
            out.clear();
            return false;
        }
        return DecompressMszip(data, size, out, maxOutputBytes);
    }

    HRESULT CreateMemoryStream(const void* data, DWORD size,
                               winrt::com_ptr<IStream>& outStream) noexcept
    {
        outStream = nullptr;
        if (size > 0 && data == nullptr)
        {
            return E_INVALIDARG;
        }

        wil::unique_hglobal memory{ GlobalAlloc(GMEM_MOVEABLE, size > 0 ? size : 1u) };
        if (!memory)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (size > 0)
        {
            wil::unique_hglobal_locked locked{ memory.get() };
            if (!locked)
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            std::memcpy(locked.get(), data, size);
        }

        winrt::com_ptr<IStream> stream;
        const HRESULT hr = CreateStreamOnHGlobal(memory.get(), TRUE, stream.put());
        if (FAILED(hr))
        {
            return hr;
        }
        memory.release();

        if (size == 0)
        {
            const ULARGE_INTEGER zeroSize{};
            stream->SetSize(zeroSize);
        }
        outStream = std::move(stream);
        return S_OK;
    }
}
