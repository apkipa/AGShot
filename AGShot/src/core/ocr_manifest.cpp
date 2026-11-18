#include "pch.h"

#include "core/ocr_manifest.h"
#include "core/text.h"

#include <toml++/toml.hpp>

namespace agshot
{
    namespace
    {
        constexpr wchar_t kManifestName[] = L"manifest.toml";
        constexpr std::uintmax_t kMaxManifestBytes = 1024 * 1024;

        std::string NormalizeSha256(std::string_view value, const char* section)
        {
            if (value.size() != 64)
            {
                throw std::runtime_error(
                    std::string("manifest.toml [") + section + "].sha256 must be 64 hex characters");
            }
            std::string normalized;
            normalized.reserve(value.size());
            for (unsigned char ch : value)
            {
                if (!std::isxdigit(ch))
                {
                    throw std::runtime_error(
                        std::string("manifest.toml [") + section + "].sha256 contains non-hex character");
                }
                normalized.push_back(static_cast<char>(std::tolower(ch)));
            }
            return normalized;
        }

        std::filesystem::path ResolveSafeAssetPath(const std::filesystem::path& baseDir,
                                                   std::string_view fileName,
                                                   const char* section)
        {
            if (fileName.empty()
                || fileName.find('/') != std::string_view::npos
                || fileName.find('\\') != std::string_view::npos
                || fileName.find("..") != std::string_view::npos)
            {
                throw std::runtime_error(
                    std::string("manifest.toml [") + section + "].file must be a single filename");
            }
            return baseDir / std::filesystem::path(fileName);
        }

        OcrModelAsset ParseAssetSection(const toml::table& root, const std::filesystem::path& baseDir,
                                        const char* section)
        {
            const auto* table = root.get_as<toml::table>(section);
            if (table == nullptr)
            {
                throw std::runtime_error(
                    std::string("manifest.toml missing [") + section + "] section");
            }

            const auto fileValue = table->get("file") ? table->get("file")->value_exact<std::string>() : std::nullopt;
            if (!fileValue)
            {
                throw std::runtime_error(
                    std::string("manifest.toml [") + section + "].file must be a string");
            }

            OcrModelAsset asset;
            asset.path = ResolveSafeAssetPath(baseDir, *fileValue, section);

            const auto shaValue = table->get("sha256")
                ? table->get("sha256")->value_exact<std::string>()
                : std::nullopt;
            if (!shaValue)
            {
                throw std::runtime_error(
                    std::string("manifest.toml [") + section + "].sha256 must be a string");
            }
            asset.sha256 = NormalizeSha256(*shaValue, section);

            std::error_code ec;
            if (!std::filesystem::is_regular_file(asset.path, ec) || ec)
            {
                throw std::runtime_error(
                    "OCR model asset missing: " + ToUtf8(asset.path.wstring()));
            }

            return asset;
        }

        toml::table ParseManifestTable(const std::filesystem::path& ocrDirectory)
        {
            const std::filesystem::path manifestPath = ocrDirectory / kManifestName;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(manifestPath, ec) || ec)
            {
                throw std::runtime_error(
                    "OCR manifest not found: " + ToUtf8(manifestPath.wstring()));
            }
            if (const auto size = std::filesystem::file_size(manifestPath, ec); !ec && size > kMaxManifestBytes)
            {
                throw std::runtime_error("OCR manifest exceeds 1 MB limit");
            }

            std::ifstream file{ manifestPath, std::ios::binary };
            if (!file)
            {
                throw std::runtime_error(
                    "Could not open OCR manifest: " + ToUtf8(manifestPath.wstring()));
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            return toml::parse(buffer.str(), ToUtf8(manifestPath.wstring()));
        }

        std::optional<std::int64_t> ReadPackFormat(const toml::table& root)
        {
            const auto* node = root.get("pack_format");
            return node ? node->value_exact<std::int64_t>() : std::nullopt;
        }
    }

    bool HasOcrModelPack(const std::filesystem::path& ocrDirectory) noexcept
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(ocrDirectory / kManifestName, ec) && !ec;
    }

    bool HasSupportedOcrPackFormat(const std::filesystem::path& ocrDirectory) noexcept
    {
        try
        {
            const auto root = ParseManifestTable(ocrDirectory);
            const auto packFormat = ReadPackFormat(root);
            return packFormat && *packFormat == kSupportedOcrPackFormat;
        }
        catch (...)
        {
            return false;
        }
    }

    std::filesystem::path FindOcrModelDirectory(const std::filesystem::path& rootDirectory) noexcept
    {
        if (rootDirectory.empty())
        {
            return {};
        }

        const std::filesystem::path ocrDir = rootDirectory / L"models" / L"ocr";
        if (HasOcrModelPack(ocrDir))
        {
            return ocrDir;
        }

        // Recover gracefully if an external folder swap aborted between moving
        // models/ocr -> models/.old-ocr and moving staging into place.
        const std::filesystem::path fallbackDir = rootDirectory / L"models" / L".old-ocr";
        if (HasOcrModelPack(fallbackDir))
        {
            return fallbackDir;
        }
        return {};
    }

    std::filesystem::path FindLocalOcrModelDirectory(const std::filesystem::path& dataDirectory,
                                                     const std::filesystem::path& executableDirectory) noexcept
    {
        std::filesystem::path candidates[3];
        std::size_t count = 0;
        const auto addCandidate = [&](std::filesystem::path root) {
            if (root.empty())
            {
                return;
            }
            root = root.lexically_normal();
            for (std::size_t i = 0; i < count; ++i)
            {
                std::error_code eqEc;
                if (candidates[i] == root || (std::filesystem::equivalent(candidates[i], root, eqEc) && !eqEc))
                {
                    return;
                }
            }
            candidates[count++] = std::move(root);
        };

        addCandidate(dataDirectory);
        addCandidate(executableDirectory);
        std::error_code cwdEc;
        if (const auto cwd = std::filesystem::current_path(cwdEc); !cwdEc)
        {
            addCandidate(cwd);
        }

        std::filesystem::path fallback;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto found = FindOcrModelDirectory(candidates[i]);
            if (found.empty())
            {
                continue;
            }
            if (HasSupportedOcrPackFormat(found))
            {
                return found;
            }
            if (fallback.empty())
            {
                fallback = found;
            }
        }
        return fallback;
    }

    OcrModelPack LoadOcrModelManifest(const std::filesystem::path& ocrDirectory)
    {
        const auto root = ParseManifestTable(ocrDirectory);
        const auto packFormat = ReadPackFormat(root);
        if (!packFormat)
        {
            throw std::runtime_error("manifest.toml missing integer pack_format");
        }
        if (*packFormat != kSupportedOcrPackFormat)
        {
            throw std::runtime_error(
                "Unsupported OCR model pack format v" + std::to_string(*packFormat)
                + " (this build supports v" + std::to_string(kSupportedOcrPackFormat) + ")");
        }

        OcrModelPack pack;
        pack.packFormat = *packFormat;
        if (const auto* versionNode = root.get("pack_version"))
        {
            if (const auto version = versionNode->value_exact<std::string>())
            {
                pack.packVersion = *version;
            }
        }

        pack.det = ParseAssetSection(root, ocrDirectory, "det");
        pack.rec = ParseAssetSection(root, ocrDirectory, "rec");
        pack.dict = ParseAssetSection(root, ocrDirectory, "dict");

        return pack;
    }
}
