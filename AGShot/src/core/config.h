#pragma once

#include "core/hotkey.h"

#include <filesystem>
#include <string>

namespace agshot
{
    // Schema version stamped into the file. Bump this whenever the meaning or
    // the shape of a stored key changes, then extend the migration step in
    // LoadConfig() so older files are upgraded instead of silently misread.
    //
    // Still 1 because no version has ever been released: every key so far was
    // added before anyone could have a file, so there is nothing in the wild to
    // migrate from. The mechanism below stays in place for the first release
    // that changes an existing key's meaning.
    inline constexpr int kConfigVersion = 1;

    struct Config
    {
        int version = kConfigVersion;

        // The key that starts a capture. The file stores it as text - "F1",
        // "Ctrl+Shift+A" - and it is kept here already parsed, because that is
        // the form RegisterHotKey() will want.
        Hotkey screenshotHotkey = DefaultHotkey();

        // Draws the frame rate and a short history of it over the overlay. Off
        // by default: it is a diagnostic, not something to ship in front of a
        // screenshot.
        bool debugShowStats = false;
    };

    // Settings are applied by comparing: a reload that produces the same values
    // is not a change, and must not touch the window.
    inline bool operator==(const Config& left, const Config& right) noexcept
    {
        return left.version == right.version
            && left.screenshotHotkey == right.screenshotHotkey
            && left.debugShowStats == right.debugShowStats;
    }

    inline bool operator!=(const Config& left, const Config& right) noexcept
    {
        return !(left == right);
    }

    struct SettingsWindowState
    {
        bool hasPlacement = false;
        int x = 0;
        int y = 0;
        int width = 820;
        int height = 620;
        bool maximized = false;
    };

    inline bool operator==(const SettingsWindowState& left, const SettingsWindowState& right) noexcept
    {
        return left.hasPlacement == right.hasPlacement
            && left.x == right.x
            && left.y == right.y
            && left.width == right.width
            && left.height == right.height
            && left.maximized == right.maximized;
    }

    inline bool operator!=(const SettingsWindowState& left, const SettingsWindowState& right) noexcept
    {
        return !(left == right);
    }

    // Full path to the running executable, or empty if it cannot be determined.
    std::filesystem::path ExecutableFilePath();

    // The directory holding the running executable, or empty if it cannot be
    // determined. Anything that ships beside the binary is found through this.
    std::filesystem::path ExecutableDirectory();

    // True when AGShot.portable or an existing config.toml sits next to the
    // executable so settings and state stay beside the binary.
    bool IsPortableMode();

    // Roaming app data directory (%APPDATA%\AGShot normally, or ExecutableDirectory()
    // in portable mode). Holds user-visible configuration.
    std::filesystem::path DataDirectory();

    // Machine-local app data directory (%LOCALAPPDATA%\AGShot normally, or
    // ExecutableDirectory() in portable mode). Holds UI state and caches.
    std::filesystem::path LocalDataDirectory();

    // WebView2 UserDataFolder (%LOCALAPPDATA%\AGShot\WebView2 normally, or
    // <exe dir>\WebView2 in portable mode).
    std::filesystem::path WebView2UserDataDirectory();

    // OCR compiled NPU blob cache directory (%APPDATA%\AGShot\cache normally,
    // or <exe dir>\cache in portable mode).
    std::filesystem::path OcrCacheDirectory();

    // %APPDATA%\AGShot\config.toml normally, or <exe dir>\config.toml in
    // portable mode.
    std::filesystem::path ConfigPath();

    // Local UI state file (window placement, etc.), kept separate from config.toml
    // so dragging windows never triggers live config reloads.
    std::filesystem::path StatePath();

    SettingsWindowState LoadSettingsState(const std::filesystem::path& path);
    void SaveSettingsState(const SettingsWindowState& state, const std::filesystem::path& path);

    struct ConfigLoad
    {
        Config config;
        bool exists = false;    // the file was present on disk
        bool ok = true;         // ...and parsed successfully
        bool migrated = false;  // ...and was upgraded to kConfigVersion
        std::wstring error;     // why it did not, when ok == false
    };

    // Never throws, whatever goes wrong. A missing, unreadable, oversized or
    // malformed file all come back as defaults with ok == false, because a
    // broken configuration must not be able to stop the app from starting.
    //
    // Called again on every change ConfigWatcher reports, so it has to stay
    // cheap and completely side-effect free: it must never write the file back
    // or "fix" anything, or watching the file would feed itself.
    ConfigLoad LoadConfig(const std::filesystem::path& path);

    // Writes to a sibling .tmp and then atomically replaces the target, so an
    // interrupted write cannot leave a half-written file behind.
    // Throws std::system_error if the file cannot be written.
    void SaveConfig(const Config& config, const std::filesystem::path& path);
}
