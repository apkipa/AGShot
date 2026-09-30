#pragma once

#include <filesystem>
#include <string>

namespace agshot
{
    // Schema version stamped into the file. Bump this whenever the meaning or
    // the shape of a stored key changes, then extend the migration step in
    // LoadConfig() so older files are upgraded instead of silently misread.
    inline constexpr int kConfigVersion = 1;

    struct Config
    {
        int version = kConfigVersion;
    };

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
    ConfigLoad LoadConfig(const std::filesystem::path& path);

    // Writes to a sibling .tmp and then atomically replaces the target, so an
    // interrupted write cannot leave a half-written file behind.
    // Throws std::system_error if the file cannot be written.
    void SaveConfig(const Config& config, const std::filesystem::path& path);
}
