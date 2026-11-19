#include "pch.h"

#include "core/file_io.h"
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
                || fileName == "."
                || fileName == ".."
                || fileName.find('/') != std::string_view::npos
                || fileName.find('\\') != std::string_view::npos
                || fileName.find(':') != std::string_view::npos)
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
            std::string content;
            switch (ReadFileBounded(manifestPath, kMaxManifestBytes, content))
            {
            case ReadFileStatus::Ok:
                return toml::parse(content, ToUtf8(manifestPath.wstring()));
            case ReadFileStatus::NotFound:
                throw std::runtime_error(
                    "OCR manifest not found: " + ToUtf8(manifestPath.wstring()));
            case ReadFileStatus::TooLarge:
                throw std::runtime_error("OCR manifest exceeds 1 MB limit");
            case ReadFileStatus::ReadFailed:
                throw std::runtime_error(
                    "Failed to read OCR manifest: " + ToUtf8(manifestPath.wstring()));
            case ReadFileStatus::OpenFailed:
            default:
                throw std::runtime_error(
                    "Could not open OCR manifest: " + ToUtf8(manifestPath.wstring()));
            }
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

    bool ComputeFileSha256Hex(const std::filesystem::path& path,
                              std::string& outHex,
                              std::wstring& outError)
    {
        outHex.clear();
        outError.clear();

        const wil::unique_hfile file{ CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
        if (!file)
        {
            outError = L"Could not open file for verification: " + path.filename().wstring();
            return false;
        }

        wil::unique_bcrypt_algorithm alg;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(alg.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        {
            outError = L"Failed to initialize Windows SHA-256 provider.";
            return false;
        }

        DWORD hashObjectSize = 0;
        DWORD cbData = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
                alg.get(), BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&hashObjectSize), sizeof(hashObjectSize),
                &cbData, 0))
            || hashObjectSize == 0)
        {
            outError = L"Failed to query SHA-256 object length.";
            return false;
        }

        std::vector<UCHAR> hashObject(hashObjectSize);

        wil::unique_bcrypt_hash hash;
        if (!BCRYPT_SUCCESS(BCryptCreateHash(
                alg.get(), hash.put(), hashObject.data(), hashObjectSize, nullptr, 0, 0)))
        {
            outError = L"Failed to create SHA-256 hash handle.";
            return false;
        }

        std::array<UCHAR, 65536> buffer{};
        for (;;)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr))
            {
                outError = L"I/O error while reading " + path.filename().wstring();
                return false;
            }
            if (bytesRead == 0)
            {
                break;
            }
            if (!BCRYPT_SUCCESS(BCryptHashData(hash.get(), buffer.data(), bytesRead, 0)))
            {
                outError = L"Failed while hashing " + path.filename().wstring();
                return false;
            }
        }

        std::array<UCHAR, 32> digest{};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(
                hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0)))
        {
            outError = L"Failed to finalize SHA-256 digest.";
            return false;
        }

        constexpr char kHex[] = "0123456789abcdef";
        outHex.reserve(64);
        for (UCHAR b : digest)
        {
            outHex.push_back(kHex[(b >> 4) & 0x0F]);
            outHex.push_back(kHex[b & 0x0F]);
        }
        return true;
    }

    OcrModelPack LoadAndVerifyOcrModelPack(const std::filesystem::path& ocrDirectory)
    {
        OcrModelPack pack = LoadOcrModelManifest(ocrDirectory);

        const auto verifyAsset = [](const OcrModelAsset& asset, const char* label)
        {
            std::string actual;
            std::wstring err;
            if (!ComputeFileSha256Hex(asset.path, actual, err))
            {
                throw std::runtime_error(ToUtf8(err));
            }
            if (actual != asset.sha256)
            {
                throw std::runtime_error(
                    std::string("SHA-256 mismatch for [") + label + "] ("
                    + ToUtf8(asset.path.filename().wstring()) + ")");
            }
        };

        verifyAsset(pack.det, "det");
        verifyAsset(pack.rec, "rec");
        verifyAsset(pack.dict, "dict");
        return pack;
    }

    namespace
    {
        bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept
        {
            return a.size() == b.size()
                && _wcsnicmp(a.data(), b.data(), a.size()) == 0;
        }

        bool ExtractZipWithSystemTar(const std::filesystem::path& zipPath,
                                     const std::filesystem::path& extractDir,
                                     std::wstring& outError)
        {
            std::wstring sysDir;
            if (FAILED(wil::GetSystemDirectoryW<std::wstring>(sysDir)) || sysDir.empty())
            {
                outError = L"Could not locate Windows System32 directory.";
                return false;
            }

            const std::filesystem::path tarExe = std::filesystem::path(sysDir) / L"tar.exe";
            std::error_code ec;
            if (!std::filesystem::is_regular_file(tarExe, ec) || ec)
            {
                outError = L"Windows built-in tar.exe was not found; extract the zip manually and select manifest.toml.";
                return false;
            }

            std::wstring cmdLine = L"\"" + tarExe.wstring() + L"\" -xf \""
                + zipPath.wstring() + L"\" -C \"" + extractDir.wstring() + L"\"";

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(
                    tarExe.c_str(),
                    cmdLine.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &si,
                    &pi))
            {
                outError = L"Failed to launch tar.exe to extract the model archive.";
                return false;
            }

            const wil::unique_handle threadHandle{ pi.hThread };
            const wil::unique_handle procHandle{ pi.hProcess };

            const DWORD waitRes = WaitForSingleObject(procHandle.get(), 60000);
            if (waitRes != WAIT_OBJECT_0)
            {
                TerminateProcess(procHandle.get(), 1);
                outError = L"Timed out while extracting the model archive.";
                return false;
            }

            DWORD exitCode = 1;
            if (!GetExitCodeProcess(procHandle.get(), &exitCode) || exitCode != 0)
            {
                outError = L"Could not extract the .zip archive (it may be corrupted or not a valid zip file).";
                return false;
            }
            return true;
        }

        std::filesystem::path LocateExtractedManifestDir(const std::filesystem::path& extractRoot) noexcept
        {
            if (HasOcrModelPack(extractRoot))
            {
                return extractRoot;
            }
            if (const auto direct = FindOcrModelDirectory(extractRoot); !direct.empty())
            {
                return direct;
            }

            std::error_code ec;
            std::filesystem::path fallback;
            for (const auto& entry : std::filesystem::directory_iterator(
                     extractRoot, std::filesystem::directory_options::skip_permission_denied, ec))
            {
                std::error_code dirEc;
                if (!entry.is_directory(dirEc) || dirEc)
                {
                    continue;
                }
                const auto& sub = entry.path();
                if (HasOcrModelPack(sub))
                {
                    if (HasSupportedOcrPackFormat(sub))
                    {
                        return sub;
                    }
                    if (fallback.empty())
                    {
                        fallback = sub;
                    }
                }
                if (const auto nested = FindOcrModelDirectory(sub); !nested.empty())
                {
                    if (HasSupportedOcrPackFormat(nested))
                    {
                        return nested;
                    }
                    if (fallback.empty())
                    {
                        fallback = nested;
                    }
                }
            }
            return fallback;
        }
    }

    OcrImportResult ImportOcrModelPack(const std::filesystem::path& sourcePath,
                                       const std::filesystem::path& targetDataDir) noexcept
    {
        OcrImportResult result;
        try
        {
            if (sourcePath.empty() || targetDataDir.empty())
            {
                result.error = L"Invalid source or destination path.";
                return result;
            }

            std::error_code ec;
            if (!std::filesystem::is_regular_file(sourcePath, ec) || ec)
            {
                result.error = L"Selected file does not exist.";
                return result;
            }

            const std::wstring ext = sourcePath.extension().wstring();
            const std::wstring fname = sourcePath.filename().wstring();
            const bool isZip = EqualsIgnoreCase(ext, L".zip");
            const bool isManifest = EqualsIgnoreCase(fname, kManifestName);
            if (!isZip && !isManifest)
            {
                result.error = L"Unsupported file. Select an OCR model pack (.zip) or manifest.toml.";
                return result;
            }

            const std::filesystem::path modelsRoot = targetDataDir / L"models";
            std::filesystem::create_directories(modelsRoot, ec);
            if (ec)
            {
                result.error = L"Could not create models directory: " + modelsRoot.wstring();
                return result;
            }

            std::filesystem::path extractDir;
            auto cleanupExtract = wil::scope_exit([&]
            {
                if (!extractDir.empty())
                {
                    std::error_code rmEc;
                    std::filesystem::remove_all(extractDir, rmEc);
                }
            });

            std::filesystem::path manifestDir;
            if (isZip)
            {
                const std::wstring suffix = std::to_wstring(GetCurrentProcessId())
                    + L"-" + std::to_wstring(GetTickCount64());
                extractDir = modelsRoot / (L".extract-ocr-" + suffix);
                std::filesystem::create_directories(extractDir, ec);
                if (ec)
                {
                    result.error = L"Could not create temporary extraction directory.";
                    return result;
                }

                std::wstring extractErr;
                if (!ExtractZipWithSystemTar(sourcePath, extractDir, extractErr))
                {
                    result.error = std::move(extractErr);
                    return result;
                }

                manifestDir = LocateExtractedManifestDir(extractDir);
                if (manifestDir.empty())
                {
                    result.error = L"No manifest.toml found inside the selected .zip archive.";
                    return result;
                }
            }
            else
            {
                manifestDir = sourcePath.parent_path();
            }

            const OcrModelPack sourcePack = LoadOcrModelManifest(manifestDir);

            const std::filesystem::path finalDir = modelsRoot / L"ocr";
            std::error_code eqEc;
            if (std::filesystem::exists(finalDir, eqEc) && !eqEc
                && std::filesystem::equivalent(manifestDir, finalDir, eqEc) && !eqEc)
            {
                const OcrModelPack verified = LoadAndVerifyOcrModelPack(finalDir);
                result.ok = true;
                result.packVersion = FromUtf8(verified.packVersion);
                result.installedDir = finalDir;
                return result;
            }

            const std::filesystem::path stagingDir = modelsRoot / L".staging-ocr";
            const std::filesystem::path backupDir = modelsRoot / L".old-ocr";

            std::filesystem::remove_all(stagingDir, ec);
            ec.clear();
            std::filesystem::create_directories(stagingDir, ec);
            if (ec)
            {
                result.error = L"Could not create staging directory for OCR models.";
                return result;
            }

            bool stagedMoved = false;
            auto cleanupStaging = wil::scope_exit([&]
            {
                if (!stagedMoved)
                {
                    std::error_code rmEc;
                    std::filesystem::remove_all(stagingDir, rmEc);
                }
            });

            const auto stageAsset = [&](const std::filesystem::path& src)
            {
                const std::filesystem::path dst = stagingDir / src.filename();
                std::error_code stageEc;
                if (isZip)
                {
                    if (std::filesystem::exists(dst, stageEc) && !stageEc)
                    {
                        return;
                    }
                    stageEc.clear();
                    std::filesystem::rename(src, dst, stageEc);
                    if (!stageEc)
                    {
                        return;
                    }
                    stageEc.clear();
                }
                std::filesystem::copy_file(
                    src,
                    dst,
                    std::filesystem::copy_options::overwrite_existing,
                    stageEc);
                if (stageEc)
                {
                    throw std::runtime_error(
                        "Failed to copy " + ToUtf8(src.filename().wstring()) + " into staging folder");
                }
            };

            stageAsset(manifestDir / kManifestName);
            stageAsset(sourcePack.det.path);
            stageAsset(sourcePack.rec.path);
            stageAsset(sourcePack.dict.path);

            // Verify manifest + SHA-256 on the staged copies before swapping into place.
            const OcrModelPack verified = LoadAndVerifyOcrModelPack(stagingDir);

            // Prepare backup slot (only remove stale .old-ocr now that stagingDir is verified).
            std::filesystem::remove_all(backupDir, ec);
            ec.clear();

            bool movedExistingToBackup = false;
            if (std::filesystem::exists(finalDir, ec) && !ec)
            {
                std::filesystem::rename(finalDir, backupDir, ec);
                if (ec)
                {
                    result.error = L"Could not replace the existing models folder (files may be in use).";
                    return result;
                }
                movedExistingToBackup = true;
            }

            ec.clear();
            std::filesystem::rename(stagingDir, finalDir, ec);
            if (ec)
            {
                if (movedExistingToBackup)
                {
                    std::error_code rbEc;
                    std::filesystem::rename(backupDir, finalDir, rbEc);
                }
                result.error = L"Could not finalize installation into models\\ocr.";
                return result;
            }
            stagedMoved = true;

            if (movedExistingToBackup)
            {
                std::error_code rmEc;
                std::filesystem::remove_all(backupDir, rmEc);
            }

            result.ok = true;
            result.packVersion = FromUtf8(verified.packVersion);
            result.installedDir = finalDir;
            return result;
        }
        catch (const std::exception& error)
        {
            result.error = FromUtf8(error.what());
            return result;
        }
        catch (...)
        {
            result.error = L"Unexpected error while importing OCR models.";
            return result;
        }
    }
}
