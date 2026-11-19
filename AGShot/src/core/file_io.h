#pragma once

#include <windows.h>
#include <wil/resource.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace agshot
{
    enum class ReadFileStatus
    {
        Ok,
        NotFound,
        TooLarge,
        OpenFailed,
        ReadFailed,
    };

    // Reads a file into `out` via Win32 ReadFile, enforcing `maxBytes` on both
    // the reported size and the actual bytes read.
    inline ReadFileStatus ReadFileBounded(const std::filesystem::path& path,
                                          std::uintmax_t maxBytes,
                                          std::string& out)
    {
        out.clear();
        const wil::unique_hfile file{ CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
        if (!file)
        {
            const DWORD err = GetLastError();
            return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                ? ReadFileStatus::NotFound
                : ReadFileStatus::OpenFailed;
        }

        LARGE_INTEGER fileSize{};
        if (GetFileSizeEx(file.get(), &fileSize) && fileSize.QuadPart > 0)
        {
            if (static_cast<std::uintmax_t>(fileSize.QuadPart) > maxBytes)
            {
                return ReadFileStatus::TooLarge;
            }
            out.reserve(static_cast<std::size_t>(fileSize.QuadPart));
        }

        char chunk[16384];
        for (;;)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(file.get(), chunk, sizeof(chunk), &bytesRead, nullptr))
            {
                out.clear();
                return ReadFileStatus::ReadFailed;
            }
            if (bytesRead == 0)
            {
                return ReadFileStatus::Ok;
            }
            if (static_cast<std::uintmax_t>(out.size()) + bytesRead > maxBytes)
            {
                out.clear();
                return ReadFileStatus::TooLarge;
            }
            out.append(chunk, bytesRead);
        }
    }
}
