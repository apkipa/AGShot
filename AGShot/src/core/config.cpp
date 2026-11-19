#include "pch.h"

#include "core/config.h"
#include "core/file_io.h"
#include "core/text.h"

// TODO: Replace toml++ with a minimal [section] + int64/bool/string subset parser/writer to drop ~185-190 KB of Release overhead (~72 KB toml++ code/data plus ~120 KB MSVC <charconv> double Ryu tables pulled in by float formatting/parsing that AGShot never uses).
#include <toml++/toml.hpp>

namespace agshot
{
    // Full path to the running executable.
    std::filesystem::path ExecutableFilePath()
    {
        std::wstring path;
        if (FAILED(wil::GetModuleFileNameW<std::wstring>(nullptr, path)))
        {
            return {};
        }
        return std::filesystem::path{ std::move(path) };
    }

    // Where the executable lives. Data that ships with the application goes
    // next to it - the portable config file and the OCR model payload both work
    // this way - so the search is always relative to the binary and never to the
    // build layout or to whatever directory the process was started from.
    std::filesystem::path ExecutableDirectory()
    {
        return ExecutableFilePath().parent_path();
    }

    namespace
    {
        std::filesystem::path KnownFolderPath(REFKNOWNFOLDERID folderId)
        {
            wil::unique_cotaskmem_string raw;
            if (FAILED(SHGetKnownFolderPath(folderId, 0, nullptr, raw.put())))
            {
                return {};
            }
            return std::filesystem::path{ raw.get() };
        }

        bool IsPortableForDir(const std::filesystem::path& exeDir)
        {
            if (exeDir.empty())
            {
                return false;
            }
            std::error_code ec;
            return (std::filesystem::exists(exeDir / L"AGShot.portable", ec) && !ec)
                || (std::filesystem::is_regular_file(exeDir / L"config.toml", ec) && !ec);
        }

        void AtomicWriteFile(const std::filesystem::path& path, std::string_view text)
        {
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path());
            }

            const std::filesystem::path temp{ path.wstring() + L".tmp" };
            {
                const wil::unique_hfile file{ CreateFileW(
                    temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr) };
                if (!file)
                {
                    throw std::system_error{ static_cast<int>(GetLastError()), std::system_category(),
                                             "cannot write " + ToUtf8(temp.wstring()) };
                }

                std::size_t offset = 0;
                while (offset < text.size())
                {
                    const DWORD chunk = static_cast<DWORD>(
                        std::min<std::size_t>(text.size() - offset, 64u * 1024u * 1024u));
                    DWORD written = 0;
                    if (!WriteFile(file.get(), text.data() + offset, chunk, &written, nullptr) || written == 0)
                    {
                        throw std::system_error{ static_cast<int>(GetLastError()), std::system_category(),
                                                 "cannot write " + ToUtf8(temp.wstring()) };
                    }
                    offset += written;
                }
            }

            if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                throw std::system_error{ static_cast<int>(GetLastError()), std::system_category(),
                                         "cannot replace " + ToUtf8(path.wstring()) };
            }
        }
    }

    bool IsPortableMode()
    {
        return IsPortableForDir(ExecutableDirectory());
    }

    std::filesystem::path DataDirectory()
    {
        const auto exeDir = ExecutableDirectory();
        if (IsPortableForDir(exeDir))
        {
            return exeDir;
        }

        if (const auto roaming = KnownFolderPath(FOLDERID_RoamingAppData); !roaming.empty())
        {
            return roaming / L"AGShot";
        }

        return exeDir;
    }

    std::filesystem::path LocalDataDirectory()
    {
        const auto exeDir = ExecutableDirectory();
        if (IsPortableForDir(exeDir))
        {
            return exeDir;
        }

        if (const auto local = KnownFolderPath(FOLDERID_LocalAppData); !local.empty())
        {
            return local / L"AGShot";
        }

        return DataDirectory();
    }

    std::filesystem::path WebView2UserDataDirectory()
    {
        return LocalDataDirectory() / L"WebView2";
    }

    std::filesystem::path OcrCacheDirectory()
    {
        return DataDirectory() / L"cache";
    }

    std::filesystem::path ConfigPath()
    {
        return DataDirectory() / L"config.toml";
    }

    std::filesystem::path StatePath()
    {
        return LocalDataDirectory() / L"state.toml";
    }

    ConfigLoad LoadConfig(const std::filesystem::path& path)
    {
        ConfigLoad result;

        std::error_code ec;
        result.exists = std::filesystem::exists(path, ec);
        if (!result.exists)
        {
            return result;
        }

        // A configuration file is a few hundred bytes. Anything much larger is
        // not our file, and slurping it into memory would be the bug.
        constexpr std::uintmax_t kMaxConfigBytes = 1024 * 1024;
        std::string content;
        switch (ReadFileBounded(path, kMaxConfigBytes, content))
        {
        case ReadFileStatus::TooLarge:
            result.ok = false;
            result.error = L"the file is larger than 1 MB, so it is not an AGShot configuration";
            return result;
        case ReadFileStatus::NotFound:
        case ReadFileStatus::OpenFailed:
        case ReadFileStatus::ReadFailed:
            result.ok = false;
            result.error = L"could not open " + path.wstring();
            return result;
        case ReadFileStatus::Ok:
            break;
        }

        // Parsed into a local so that a file rejected half way through cannot
        // leave some of its values behind: ok == false has to mean the defaults
        // really are in force, or the warning the user reads would be a lie.
        Config parsed;

        try
        {
            const auto table = toml::parse(content, ToUtf8(path.wstring()));

            // "absent" and "present but not an integer" must not be conflated:
            // value_or() reports both as the fallback, which would make a
            // corrupted version look like a perfectly good current one.
            if (const auto* node = table.get("version"))
            {
                const auto value = node->value_exact<int64_t>();
                if (!value)
                {
                    result.ok = false;
                    result.error = L"version must be an integer";
                    return result;
                }
                parsed.version = static_cast<int>(*value);
            }

            if (const auto* node = table.get("debug"))
            {
                const auto* debug = node->as_table();
                if (debug == nullptr)
                {
                    result.ok = false;
                    result.error = L"debug must be a table";
                    return result;
                }

                if (const auto* showStats = debug->get("showStats"))
                {
                    const auto value = showStats->value_exact<bool>();
                    if (!value)
                    {
                        result.ok = false;
                        result.error = L"debug.showStats must be true or false";
                        return result;
                    }
                    parsed.debugShowStats = *value;
                }
            }

            if (const auto* node = table.get("screenshot"))
            {
                const auto* screenshot = node->as_table();
                if (screenshot == nullptr)
                {
                    result.ok = false;
                    result.error = L"screenshot must be a table";
                    return result;
                }

                if (const auto* hotkey = screenshot->get("hotkey"))
                {
                    // Stored as text rather than as a key code, because this file
                    // is meant to be written and read by hand.
                    const auto text = hotkey->value_exact<std::string>();
                    if (!text)
                    {
                        result.ok = false;
                        result.error =
                            L"screenshot.hotkey must be a string, such as \"F1\" or \"Ctrl+Shift+A\"";
                        return result;
                    }

                    // Checked here, at load, rather than left for whatever
                    // registers it later: a hotkey that cannot work is a
                    // configuration mistake, and the file is where it is fixed.
                    std::wstring reason;
                    if (!ParseHotkey(FromUtf8(*text), parsed.screenshotHotkey, reason))
                    {
                        result.ok = false;
                        result.error = L"screenshot.hotkey: " + reason;
                        return result;
                    }
                }
            }
        }
        catch (const toml::parse_error& e)
        {
            std::ostringstream what;
            what << e.description() << " (line " << e.source().begin.line << ")";
            result.ok = false;
            result.error = FromUtf8(what.str());
            return result;
        }
        catch (const std::exception& e)
        {
            // toml++ only documents parse_error, but nothing stops std::bad_alloc
            // and friends. Swallowing them here is the whole point of this
            // function: a bad configuration must never stop the app from starting.
            result.ok = false;
            result.error = FromUtf8(e.what() == nullptr ? "unknown parsing failure" : e.what());
            return result;
        }
        catch (...)
        {
            result.ok = false;
            result.error = L"an unrecognised failure occurred while parsing";
            return result;
        }

        result.config = parsed;

        // Migration hook. There is no step yet because no version has ever been
        // released, so there is nothing in the wild to upgrade from. When a
        // release changes the meaning of a key that already exists, bump
        // kConfigVersion and add the step here, keyed on the version that was
        // just read. For a newly added key no step is needed at all: an absent
        // key already means its default.
        if (result.config.version < kConfigVersion)
        {
            result.config.version = kConfigVersion;
            result.migrated = true;
        }
        // A version newer than ours means the file was written by a later build.
        // Nothing to do yet: leave it alone rather than rewriting keys we may
        // not understand. Revisit once the schema has keys with real meaning.

        return result;
    }

    void SaveConfig(const Config& config, const std::filesystem::path& path)
    {
        toml::table screenshot;
        screenshot.insert("hotkey", ToUtf8(FormatHotkey(config.screenshotHotkey)));

        toml::table debug;
        debug.insert("showStats", config.debugShowStats);

        toml::table table;
        table.insert("version", config.version);
        table.insert("screenshot", std::move(screenshot));
        table.insert("debug", std::move(debug));

        // toml++ would rather write single-quoted literal strings. The file is
        // meant to be read and edited by hand, and the comment above shows
        // double quotes, so ask for those and keep the two consistent.
        constexpr toml::format_flags kWriteFlags =
            toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings;

        toml::toml_formatter formatter{ table, kWriteFlags };

        std::ostringstream body;
        body << formatter;

        std::string text =
            "# AGShot configuration file.\n"
            "# Delete it to restore the defaults.\n"
            "#\n"
            "# AGShot watches this file, so changes apply while it is running.\n"
            "#\n"
            "# A hotkey is one key with optional modifiers: \"F1\", \"Ctrl+Shift+A\".\n"
            "\n";
        text += body.str();

        AtomicWriteFile(path, text);
    }

    SettingsWindowState LoadSettingsState(const std::filesystem::path& path)
    {
        SettingsWindowState state;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec)
        {
            return state;
        }
        std::string content;
        if (ReadFileBounded(path, 64 * 1024, content) != ReadFileStatus::Ok)
        {
            return state;
        }

        try
        {
            const auto table = toml::parse(content, ToUtf8(path.wstring()));
            const auto* win = table["settings_window"].as_table();
            if (win == nullptr)
            {
                return state;
            }
            const auto x = win->get("x") ? win->get("x")->value_exact<int64_t>() : std::nullopt;
            const auto y = win->get("y") ? win->get("y")->value_exact<int64_t>() : std::nullopt;
            const auto w = win->get("width") ? win->get("width")->value_exact<int64_t>() : std::nullopt;
            const auto h = win->get("height") ? win->get("height")->value_exact<int64_t>() : std::nullopt;
            if (x && y && w && h && *w >= 400 && *w <= 16384 && *h >= 300 && *h <= 16384)
            {
                state.hasPlacement = true;
                state.x = static_cast<int>(*x);
                state.y = static_cast<int>(*y);
                state.width = static_cast<int>(*w);
                state.height = static_cast<int>(*h);
            }
            if (const auto maximized = win->get("maximized")
                    ? win->get("maximized")->value_exact<bool>()
                    : std::nullopt)
            {
                state.maximized = *maximized;
            }
        }
        catch (...)
        {
            return SettingsWindowState{};
        }
        return state;
    }

    void SaveSettingsState(const SettingsWindowState& state, const std::filesystem::path& path)
    {
        toml::table win;
        if (state.hasPlacement)
        {
            win.insert("x", state.x);
            win.insert("y", state.y);
            win.insert("width", state.width);
            win.insert("height", state.height);
        }
        win.insert("maximized", state.maximized);

        toml::table table;
        table.insert("settings_window", std::move(win));

        toml::toml_formatter formatter{ table };
        std::ostringstream body;
        body << formatter;

        std::string text = "# AGShot local UI state.\n\n";
        text += body.str();
        AtomicWriteFile(path, text);
    }
}
