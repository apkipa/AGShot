#pragma once

#include "core/hotkey.h"

#include <filesystem>
#include <string>

namespace agshot
{
    // Schema version stamped into the file. Bump this whenever the meaning or
    // the shape of a stored key changes, then extend the migration step in
    // LoadConfig() so older files are upgraded instead of silently misread.
    // 1: version on its own. 2: added [window]. 3: added [screenshot].
    inline constexpr int kConfigVersion = 3;

    // A window size is a client area in DIPs at 96 DPI, so the same file gives
    // the same apparent size on any monitor. The upper bound is the largest
    // dimension Win32 accepts for a window.
    inline constexpr int kMinWindowSize = 1;
    inline constexpr int kMaxWindowSize = 32767;

    struct Config
    {
        int version = kConfigVersion;
        int windowWidth = 800;
        int windowHeight = 450;

        // The key that starts a capture. The file stores it as text - "F1",
        // "Ctrl+Shift+A" - and it is kept here already parsed, because that is
        // the form RegisterHotKey() will want.
        Hotkey screenshotHotkey = DefaultHotkey();
    };

    // Settings are applied by comparing: a reload that produces the same values
    // is not a change, and must not touch the window.
    inline bool operator==(const Config& left, const Config& right) noexcept
    {
        return left.version == right.version
            && left.windowWidth == right.windowWidth
            && left.windowHeight == right.windowHeight
            && left.screenshotHotkey == right.screenshotHotkey;
    }

    inline bool operator!=(const Config& left, const Config& right) noexcept
    {
        return !(left == right);
    }

    // %APPDATA%\AGShot\config.toml normally. If a file named AGShot.portable
    // sits next to the executable, <exe dir>\config.toml is used instead, which
    // is what you want when running from a USB stick or a build output folder.
    std::filesystem::path ConfigPath();

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
