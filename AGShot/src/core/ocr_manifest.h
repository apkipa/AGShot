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

    struct OcrImportResult
    {
        bool ok{};
        std::wstring packVersion;
        std::filesystem::path installedDir;
        std::wstring error;
    };

    // Reads and validates `<ocrDirectory>/manifest.toml` (pack_format, asset
    // filenames, and SHA-256 format). Throws std::runtime_error with a clear
    // diagnostic message on mismatch.
    OcrModelPack LoadOcrModelManifest(const std::filesystem::path& ocrDirectory);

    // Computes the lowercase hex SHA-256 digest of a regular file via Windows CNG.
    bool ComputeFileSha256Hex(const std::filesystem::path& path,
                              std::string& outHex,
                              std::wstring& outError);

    // Validates manifest.toml AND checks actual SHA-256 digests of det, rec, and dict.
    // Throws std::runtime_error on any mismatch or missing/corrupted asset.
    OcrModelPack LoadAndVerifyOcrModelPack(const std::filesystem::path& ocrDirectory);

    // Imports an OCR model pack from either a .zip archive or a manifest.toml file
    // into `<targetDataDir>/models/ocr`, verifying manifest + SHA-256 digests before
    // atomically swapping the directory into place.
    OcrImportResult ImportOcrModelPack(const std::filesystem::path& sourcePath,
                                       const std::filesystem::path& targetDataDir) noexcept;
}
