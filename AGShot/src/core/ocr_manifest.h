#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace agshot
{
    inline constexpr std::int64_t kSupportedOcrPackFormat = 1;

    struct OcrModelAsset
    {
        std::filesystem::path path;
        std::string sha256;
    };

    struct OcrModelPack
    {
        std::int64_t packFormat{};
        std::string packVersion;
        OcrModelAsset det;
        OcrModelAsset rec;
        OcrModelAsset dict;
    };

    // Fast probe without parsing weights or hashing payloads: true when the
    // domain directory holds a manifest.toml file.
    bool HasOcrModelPack(const std::filesystem::path& ocrDirectory) noexcept;

    // Fast compatibility check: true when `<ocrDirectory>/manifest.toml` parses
    // as a TOML table with `pack_format == kSupportedOcrPackFormat`. Does not
    // hash model payloads.
    bool HasSupportedOcrPackFormat(const std::filesystem::path& ocrDirectory) noexcept;

    // Resolves `<root>/models/ocr` (recovering `<root>/models/.old-ocr` if an
    // external folder swap left it behind) when a manifest is present.
    std::filesystem::path FindOcrModelDirectory(const std::filesystem::path& rootDirectory) noexcept;

    // Searches dataDirectory, executableDirectory, and current working directory
    // in order, preferring a pack whose pack_format matches kSupportedOcrPackFormat.
    std::filesystem::path FindLocalOcrModelDirectory(const std::filesystem::path& dataDirectory,
                                                     const std::filesystem::path& executableDirectory) noexcept;

    // Reads and validates `<ocrDirectory>/manifest.toml` (pack_format, asset
    // filenames, and SHA-256 format). Throws std::runtime_error with a clear
    // diagnostic message on mismatch.
    OcrModelPack LoadOcrModelManifest(const std::filesystem::path& ocrDirectory);
}
