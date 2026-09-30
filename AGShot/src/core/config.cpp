#include "pch.h"

#include "core/config.h"
#include "core/text.h"

#include <toml++/toml.hpp>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

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
                result.config.version = static_cast<int>(*value);
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

        // Migration hook. With a single schema revision there is nothing to
        // upgrade yet; each future bump adds a step here keyed on the version
        // that was just read.
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

        toml::table table;
        table.insert("version", config.version);

        std::ostringstream body;
        body << table;

        std::string text =
            "# AGShot configuration file.\n"
            "# Delete it to restore the defaults.\n"
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
