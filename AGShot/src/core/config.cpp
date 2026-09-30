#include "pch.h"

#include "core/config.h"
#include "core/text.h"

#include <toml++/toml.hpp>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <shlobj.h>

namespace agshot
{
    namespace
    {
        std::filesystem::path ExecutableDirectory()
        {
            std::wstring buffer(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD written = GetModuleFileNameW(
                    nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (written == 0)
                {
                    return {};
                }
                if (written < buffer.size())
                {
                    buffer.resize(written);
                    break;
                }
                buffer.resize(buffer.size() * 2);   // truncated: try again bigger
            }
            return std::filesystem::path{ buffer }.parent_path();
        }

        std::filesystem::path RoamingAppData()
        {
            PWSTR raw = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)))
            {
                return {};
            }
            std::filesystem::path result{ raw };
            CoTaskMemFree(raw);
            return result;
        }

        // Reads a window dimension. "absent" and "present but unusable" must not
        // be conflated: value_or() reports both as the fallback, which would make
        // a corrupted setting look exactly like a deliberate one.
        bool ReadWindowSize(const toml::table& window, std::string_view key,
                            int& target, std::wstring& error)
        {
            const auto* node = window.get(key);
            if (node == nullptr)
            {
                return true;    // absent: the default stands
            }

            const auto value = node->value_exact<int64_t>();
            if (!value)
            {
                error = L"window." + FromUtf8(key) + L" must be an integer";
                return false;
            }
            if (*value < kMinWindowSize || *value > kMaxWindowSize)
            {
                error = L"window." + FromUtf8(key) + L" must be between "
                    + std::to_wstring(kMinWindowSize) + L" and "
                    + std::to_wstring(kMaxWindowSize);
                return false;
            }

            target = static_cast<int>(*value);
            return true;
        }
    }

    std::filesystem::path ConfigPath()
    {
        const auto exeDir = ExecutableDirectory();
        if (!exeDir.empty() && std::filesystem::exists(exeDir / L"AGShot.portable"))
        {
            return exeDir / L"config.toml";
        }

        if (const auto roaming = RoamingAppData(); !roaming.empty())
        {
            return roaming / L"AGShot" / L"config.toml";
        }

        return exeDir / L"config.toml";   // last resort: keep it beside the exe
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
        if (const auto size = std::filesystem::file_size(path, ec); !ec && size > kMaxConfigBytes)
        {
            result.ok = false;
            result.error = L"the file is larger than 1 MB, so it is not an AGShot configuration";
            return result;
        }

        std::ifstream file{ path, std::ios::binary };
        if (!file)
        {
            result.ok = false;
            result.error = L"could not open " + path.wstring();
            return result;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();

        // Parsed into a local so that a file rejected half way through cannot
        // leave some of its values behind: ok == false has to mean the defaults
        // really are in force, or the warning the user reads would be a lie.
        Config parsed;

        try
        {
            const auto table = toml::parse(buffer.str(), ToUtf8(path.wstring()));

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

            if (const auto* node = table.get("window"))
            {
                const auto* window = node->as_table();
                if (window == nullptr)
                {
                    result.ok = false;
                    result.error = L"window must be a table";
                    return result;
                }

                if (!ReadWindowSize(*window, "width", parsed.windowWidth, result.error)
                    || !ReadWindowSize(*window, "height", parsed.windowHeight, result.error))
                {
                    result.ok = false;
                    return result;
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

        // Migration hook. 1 -> 2 added [window] and 2 -> 3 added [screenshot];
        // in both cases an absent key already means the default, so stamping the
        // new version is the whole upgrade - which still matters, because the
        // rewrite is how the new keys reach a file written before they existed.
        // Each future bump adds a real step here, keyed on the version that was
        // just read.
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
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        toml::table window;
        window.insert("width", config.windowWidth);
        window.insert("height", config.windowHeight);

        toml::table screenshot;
        screenshot.insert("hotkey", ToUtf8(FormatHotkey(config.screenshotHotkey)));

        toml::table table;
        table.insert("version", config.version);
        table.insert("window", std::move(window));
        table.insert("screenshot", std::move(screenshot));

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

        const std::filesystem::path temp{ path.wstring() + L".tmp" };
        {
            std::ofstream file{ temp, std::ios::binary | std::ios::trunc };
            if (!file)
            {
                throw std::system_error{ errno, std::system_category(), "cannot write " + ToUtf8(temp.wstring()) };
            }
            file << text;
            file.flush();
            if (!file)
            {
                throw std::system_error{ errno, std::system_category(), "cannot write " + ToUtf8(temp.wstring()) };
            }
        }

        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            throw std::system_error{ static_cast<int>(GetLastError()), std::system_category(),
                                     "cannot replace " + ToUtf8(path.wstring()) };
        }
    }
}
