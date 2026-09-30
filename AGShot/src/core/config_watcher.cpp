#include "pch.h"

#include "core/config_watcher.h"
#include "core/error.h"

#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace agshot
{
    namespace
    {
        // A save is a burst - a temporary file, a rename, a flush - so wait for
        // the writer to go quiet before saying anything. Reloading on the first
        // notification would race with the write that is still in progress.
        constexpr DWORD kSettleMs = 250;

        // ...but never settle forever. A directory that somehow never stops
        // changing must not be able to hold the reload off for good.
        constexpr ULONGLONG kMaxSettleMs = 2000;

        constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME
                                      | FILE_NOTIFY_CHANGE_SIZE
                                      | FILE_NOTIFY_CHANGE_LAST_WRITE;

        // Enough for a burst of changes in a directory holding one file.
        constexpr DWORD kBufferBytes = 16 * 1024;

        enum class Wait
        {
            Changed,   // a notification arrived and is in the buffer
            Quiet,     // nothing more arrived within the timeout
            Stop,      // the owner asked the watcher to shut down
            Failed,    // the directory is no longer watchable
        };

        // Cancels a pending read and waits for its completion, which is what the
        // wait event signals. Leaving it outstanding would let the next read
        // share an event with a request nobody is ever going to look at.
        void Abandon(HANDLE directory, HANDLE ioEvent, OVERLAPPED& overlapped) noexcept
        {
            CancelIoEx(directory, &overlapped);
            WaitForSingleObject(ioEvent, INFINITE);
        }

        Wait WaitOnce(HANDLE directory, HANDLE stopEvent, HANDLE ioEvent, DWORD timeout,
                      void* buffer, DWORD size, DWORD& bytes, DWORD& error) noexcept
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = ioEvent;

            bytes = 0;
            error = 0;

            if (!ReadDirectoryChangesW(directory, buffer, size, FALSE, kNotifyFilter,
                                       &bytes, &overlapped, nullptr)
                && GetLastError() != ERROR_IO_PENDING)
            {
                error = GetLastError();
                return Wait::Failed;
            }

            const HANDLE handles[2] = { stopEvent, ioEvent };

            switch (WaitForMultipleObjects(2, handles, FALSE, timeout))
            {
            case WAIT_OBJECT_0:
                Abandon(directory, ioEvent, overlapped);
                return Wait::Stop;

            case WAIT_OBJECT_0 + 1:
                if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE))
                {
                    error = GetLastError();
                    return Wait::Failed;
                }
                return Wait::Changed;

            case WAIT_TIMEOUT:
                Abandon(directory, ioEvent, overlapped);
                return Wait::Quiet;

            default:
                error = GetLastError();
                Abandon(directory, ioEvent, overlapped);
                return Wait::Failed;
            }
        }

        // The buffer is a chain of variable-length records, each naming one
        // changed entry. Only the file we were asked about counts: the same
        // directory holds the temporary that SaveConfig writes.
        bool NamesTarget(const void* buffer, DWORD bytes, std::wstring_view fileName)
        {
            constexpr std::size_t kHeader = offsetof(FILE_NOTIFY_INFORMATION, FileName);
            const auto* base = static_cast<const BYTE*>(buffer);

            for (DWORD offset = 0;;)
            {
                if (offset > bytes || bytes - offset < kHeader)
                {
                    return false;
                }

                FILE_NOTIFY_INFORMATION info{};
                std::memcpy(&info, base + offset, kHeader);

                const DWORD nameBytes = info.FileNameLength;
                if (nameBytes != 0 && nameBytes <= bytes - offset - kHeader)
                {
                    const std::wstring_view name{
                        reinterpret_cast<const wchar_t*>(base + offset + kHeader),
                        nameBytes / sizeof(wchar_t) };

                    if (CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                             fileName.data(), static_cast<int>(fileName.size()),
                                             TRUE) == CSTR_EQUAL)
                    {
                        return true;
                    }
                }

                if (info.NextEntryOffset == 0 || info.NextEntryOffset > bytes - offset)
                {
                    return false;
                }
                offset += info.NextEntryOffset;
            }
        }
    }

    ConfigWatcher::~ConfigWatcher()
    {
        Stop();
    }

    bool ConfigWatcher::Start(const std::filesystem::path& path, WatchCallback on_change)
    {
        Stop();

        if (path.filename().empty() || !on_change)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }

        // On a first run the directory may not exist yet, and a watch cannot be
        // placed on a directory that is not there.
        std::error_code ec;
        auto directory = path.parent_path();
        if (directory.empty())
        {
            directory = std::filesystem::path{ L"." };
        }
        std::filesystem::create_directories(directory, ec);

        const HANDLE directoryHandle = CreateFileW(
            directory.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (directoryHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stopEvent == nullptr)
        {
            CloseHandle(directoryHandle);
            return false;
        }

        const HANDLE ioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ioEvent == nullptr)
        {
            const DWORD error = GetLastError();
            CloseHandle(stopEvent);
            CloseHandle(directoryHandle);
            SetLastError(error);
            return false;
        }

        m_directory = directoryHandle;
        m_stopEvent = stopEvent;
        m_ioEvent = ioEvent;
        m_fileName = path.filename().wstring();
        m_callback = std::move(on_change);

        const HANDLE thread = CreateThread(nullptr, 0, &ConfigWatcher::ThreadMain, this, 0, nullptr);
        if (thread == nullptr)
        {
            const DWORD error = GetLastError();
            m_directory = nullptr;
            m_stopEvent = nullptr;
            m_ioEvent = nullptr;
            m_fileName.clear();
            m_callback = nullptr;
            CloseHandle(ioEvent);
            CloseHandle(stopEvent);
            CloseHandle(directoryHandle);
            SetLastError(error);
            return false;
        }

        m_thread = thread;
        return true;
    }

    void ConfigWatcher::Stop() noexcept
    {
        if (m_thread != nullptr)
        {
            SetEvent(m_stopEvent);
            WaitForSingleObject(m_thread, INFINITE);
            CloseHandle(m_thread);
            m_thread = nullptr;
        }

        // Only once the thread has finished, so it cannot be using a handle that
        // has already been closed.
        if (m_directory != nullptr)
        {
            CloseHandle(m_directory);
            m_directory = nullptr;
        }
        if (m_ioEvent != nullptr)
        {
            CloseHandle(m_ioEvent);
            m_ioEvent = nullptr;
        }
        if (m_stopEvent != nullptr)
        {
            CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
        }

        m_fileName.clear();
        m_callback = nullptr;
    }

    DWORD WINAPI ConfigWatcher::ThreadMain(LPVOID parameter)
    {
        static_cast<ConfigWatcher*>(parameter)->Run();
        return 0;
    }

    void ConfigWatcher::Run() noexcept
    {
        std::vector<BYTE> buffer(kBufferBytes);
        const DWORD size = static_cast<DWORD>(buffer.size());

        for (;;)
        {
            DWORD bytes = 0;
            DWORD error = 0;

            // Sit on the directory until something names the file we were asked
            // about. Anything else in there is not our business.
            Wait state = WaitOnce(m_directory, m_stopEvent, m_ioEvent, INFINITE,
                                  buffer.data(), size, bytes, error);
            while (state == Wait::Changed && !NamesTarget(buffer.data(), bytes, m_fileName))
            {
                state = WaitOnce(m_directory, m_stopEvent, m_ioEvent, INFINITE,
                                 buffer.data(), size, bytes, error);
            }

            if (state == Wait::Failed)
            {
                m_callback(WatchEvent::Failed,
                           DescribeWin32(L"ReadDirectoryChangesW", error, __FILEW__, __LINE__).detail);
                return;
            }
            if (state != Wait::Changed)
            {
                return;   // Stop
            }

            const ULONGLONG deadline = GetTickCount64() + kMaxSettleMs;
            for (;;)
            {
                const Wait again = WaitOnce(m_directory, m_stopEvent, m_ioEvent, kSettleMs,
                                            buffer.data(), size, bytes, error);
                if (again == Wait::Quiet)
                {
                    break;
                }
                if (again == Wait::Changed)
                {
                    if (GetTickCount64() < deadline)
                    {
                        continue;   // still being written to
                    }
                    break;
                }
                if (again == Wait::Failed)
                {
                    m_callback(WatchEvent::Failed,
                               DescribeWin32(L"ReadDirectoryChangesW", error, __FILEW__, __LINE__).detail);
                }
                return;
            }

            m_callback(WatchEvent::Changed, std::wstring{});
        }
    }
}
