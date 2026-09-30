#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>

namespace agshot
{
    // What the watcher tells the application.
    enum class WatchEvent
    {
        // The file was written to and the writer has gone quiet. "detail" is empty.
        Changed,
        // Watching is over for good; "detail" says why, for the user to read.
        Failed,
    };

    // Runs on the watcher thread, so it must not block and must not throw: it is
    // expected to hand the work to the UI thread and return.
    using WatchCallback = std::function<void(WatchEvent event, const std::wstring& detail)>;

    // Reports when a settings file changes, so the app can re-read it while it
    // is running.
    //
    // The directory is watched rather than the file, because saving usually
    // replaces the file: SaveConfig writes a temporary and renames it over the
    // target, which gives the result a new identity and would silently end a
    // watch held on the old one.
    //
    // The watcher parses nothing. Reading and validating the file stays in
    // LoadConfig(), and applying the result stays in the caller.
    class ConfigWatcher
    {
    public:
        ConfigWatcher() = default;
        ~ConfigWatcher();

        ConfigWatcher(const ConfigWatcher&) = delete;
        ConfigWatcher& operator=(const ConfigWatcher&) = delete;

        // Returns false when the directory cannot be watched, leaving the last
        // Win32 error set for the caller to report, exactly like StartTrayIcon.
        // A configuration that cannot be watched is an inconvenience, not a
        // reason to fail to start.
        bool Start(const std::filesystem::path& path, WatchCallback on_change);

        // Idempotent. Signals the thread and waits for it, so no callback can
        // run once this returns.
        void Stop() noexcept;

    private:
        void Run() noexcept;

        // Matches LPTHREAD_START_ROUTINE exactly, __stdcall included, which a
        // captureless lambda would not on a 32-bit build.
        static DWORD WINAPI ThreadMain(LPVOID parameter);

        HANDLE m_thread{};
        HANDLE m_stopEvent{};
        HANDLE m_ioEvent{};
        HANDLE m_directory{};
        std::wstring m_fileName;
        WatchCallback m_callback;
    };
}
