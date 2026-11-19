#include "pch.h"

#include "settings/settings_window.h"
#include "resource.h"

#include "core/app_ipc.h"
#include "core/config.h"
#include "core/embedded_resource.h"
#include "core/error.h"
#include "core/hotkey.h"
#include "core/icon_pack.h"
#include "core/ocr_manifest.h"
#include "core/text.h"

#if defined(__has_include)
#if __has_include("build_info.h")
#include "build_info.h"
#endif
#endif
#ifndef AGSHOT_BUILD_VERSION
#define AGSHOT_BUILD_VERSION L"dev"
#endif

namespace agshot
{
    namespace
    {
        namespace Json = winrt::Windows::Data::Json;

        constexpr wchar_t kSettingsUrl[] = L"https://app.local/settings.html";
        constexpr int kDefaultWidthDip = 740;
        constexpr int kDefaultHeightDip = 520;
        constexpr int kMinWidthDip = 480;
        constexpr int kMinHeightDip = 360;
        constexpr int kProbeHotkeyId = 9001;
        constexpr UINT kFallbackOpenConfigMsg = WM_APP + 20;

        bool IsSystemDarkMode() noexcept
        {
            DWORD value = 1;
            DWORD size = sizeof(value);
            const LSTATUS status = RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme",
                RRF_RT_REG_DWORD,
                nullptr,
                &value,
                &size);
            if (status == ERROR_SUCCESS)
            {
                return value == 0;
            }
            return false;
        }

        void ApplyDarkTitleBar(HWND hwnd, bool dark) noexcept
        {
            const BOOL enabled = dark ? TRUE : FALSE;
            constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
            DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &enabled, sizeof(enabled));
        }

        HWND FindMainMessageWindow() noexcept
        {
            return FindWindowExW(HWND_MESSAGE, nullptr, kMessageClass, nullptr);
        }

        bool SendMainCommand(UINT message, LRESULT* outResult = nullptr) noexcept
        {
            const HWND target = FindMainMessageWindow();
            if (target == nullptr)
            {
                return false;
            }
            DWORD_PTR result = 0;
            if (!SendMessageTimeoutW(target, message, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 500, &result))
            {
                return false;
            }
            if (outResult != nullptr)
            {
                *outResult = static_cast<LRESULT>(result);
            }
            return true;
        }

        bool OpenShellTarget(const wchar_t* target, const wchar_t* label)
        {
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
                nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32)
            {
                ShowWarning(
                    DescribeWin32(label, static_cast<unsigned long>(result), __FILEW__, __LINE__),
                    std::wstring(L"Could not open: ") + target);
                return false;
            }
            return true;
        }

        struct CacheStats
        {
            std::uint64_t bytes = 0;
            std::uint64_t files = 0;
        };

        CacheStats MeasureDirectory(const std::filesystem::path& dir) noexcept
        {
            CacheStats stats;
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec) || ec)
            {
                return stats;
            }

            std::filesystem::recursive_directory_iterator it(
                dir, std::filesystem::directory_options::skip_permission_denied, ec);
            for (const std::filesystem::recursive_directory_iterator end; it != end && !ec; it.increment(ec))
            {
                std::error_code fileEc;
                if (it->is_regular_file(fileEc) && !fileEc)
                {
                    const auto size = it->file_size(fileEc);
                    if (!fileEc)
                    {
                        stats.bytes += size;
                    }
                    ++stats.files;
                }
            }
            return stats;
        }

        struct ClearResult
        {
            std::uint64_t deletedFiles = 0;
            std::uint64_t failedEntries = 0;
        };

        ClearResult ClearDirectoryContents(const std::filesystem::path& dir) noexcept
        {
            ClearResult out;
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec) || ec)
            {
                return out;
            }

            for (const auto& entry : std::filesystem::directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied, ec))
            {
                std::error_code countEc;
                std::uint64_t entryFiles = 0;
                if (entry.is_regular_file(countEc) && !countEc)
                {
                    entryFiles = 1;
                }
                else if (entry.is_directory(countEc) && !countEc)
                {
                    entryFiles = MeasureDirectory(entry.path()).files;
                }

                std::error_code rmEc;
                std::filesystem::remove_all(entry.path(), rmEc);
                if (rmEc)
                {
                    ++out.failedEntries;
                }
                else
                {
                    out.deletedFiles += entryFiles;
                }
            }
            return out;
        }

        RECT ClampBoundsToMonitor(const RECT& candidate, int minWidthPx, int minHeightPx) noexcept
        {
            HMONITOR monitor = MonitorFromRect(&candidate, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            RECT work{ 0, 0, 1280, 720 };
            if (monitor != nullptr && GetMonitorInfoW(monitor, &mi))
            {
                work = mi.rcWork;
            }

            const int workW = std::max<int>(1, work.right - work.left);
            const int workH = std::max<int>(1, work.bottom - work.top);
            const int width = std::clamp<int>(candidate.right - candidate.left,
                                             std::min(minWidthPx, workW), workW);
            const int height = std::clamp<int>(candidate.bottom - candidate.top,
                                              std::min(minHeightPx, workH), workH);
            const int maxX = std::max<int>(work.left, work.right - width);
            const int maxY = std::max<int>(work.top, work.bottom - height);
            const int x = std::clamp<int>(candidate.left, work.left, maxX);
            const int y = std::clamp<int>(candidate.top, work.top, maxY);
            return RECT{ x, y, x + width, y + height };
        }

        RECT ComputeInitialBounds(const SettingsWindowState& saved) noexcept
        {
            UINT dpi = 96;
            POINT cursor{};
            HMONITOR cursorMonitor = nullptr;
            if (GetCursorPos(&cursor))
            {
                cursorMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            }
            const HDC screenDc = GetDC(nullptr);
            if (screenDc != nullptr)
            {
                dpi = static_cast<UINT>(GetDeviceCaps(screenDc, LOGPIXELSX));
                ReleaseDC(nullptr, screenDc);
            }
            const int minW = MulDiv(kMinWidthDip, static_cast<int>(dpi), 96);
            const int minH = MulDiv(kMinHeightDip, static_cast<int>(dpi), 96);

            if (saved.hasPlacement)
            {
                const RECT candidate{
                    saved.x,
                    saved.y,
                    saved.x + saved.width,
                    saved.y + saved.height
                };
                return ClampBoundsToMonitor(candidate, minW, minH);
            }

            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            RECT work{ 0, 0, 1280, 800 };
            if (cursorMonitor != nullptr && GetMonitorInfoW(cursorMonitor, &mi))
            {
                work = mi.rcWork;
            }

            const int workW = work.right - work.left;
            const int workH = work.bottom - work.top;
            const int w = std::clamp<int>(MulDiv(kDefaultWidthDip, static_cast<int>(dpi), 96), minW, workW);
            const int h = std::clamp<int>(MulDiv(kDefaultHeightDip, static_cast<int>(dpi), 96), minH, workH);
            const int x = work.left + (workW - w) / 2;
            const int y = work.top + (workH - h) / 2;
            return RECT{ x, y, x + w, y + h };
        }

        class SettingsHost
        {
        public:
            explicit SettingsHost(HINSTANCE instance) noexcept
                : m_instance(instance)
            {
            }

            ~SettingsHost()
            {
                ResumeMainHotkeyIfPaused();
            }

            SettingsHost(const SettingsHost&) = delete;
            SettingsHost& operator=(const SettingsHost&) = delete;

            int Run(int showCommand);

            void OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* environment);
            void OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller);
            void OnWebResourceRequested(ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args);
            void OnWebMessageReceived(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args);

        private:
            static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
            LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

            void UpdateWebViewBackground() noexcept;
            void StartWebView();

            Json::JsonObject BuildStateJson();
            void SendState();
            void SendJson(const Json::JsonObject& obj);
            void SendResult(const wchar_t* type, bool ok, const std::wstring& message);

            void HandleSetHotkey(const std::wstring& value);
            void HandleSetDebugStats(bool enabled);
            bool RequestAndWaitOcrTrim(int maxPolls);
            bool OpenConfigInShell();
            void HandleTrimOcr();
            void HandleClearOcrCache();

            void PauseMainHotkey() noexcept;
            void ResumeMainHotkeyIfPaused() noexcept;
            void PersistWindowState() noexcept;
            void TriggerFallbackToConfig(const std::wstring& reason) noexcept;

            HINSTANCE m_instance{};
            HWND m_hwnd{};
            std::filesystem::path m_configPath;
            std::filesystem::path m_statePath;
            SettingsWindowState m_initialState;

            winrt::com_ptr<ICoreWebView2Environment> m_env;
            winrt::com_ptr<ICoreWebView2Controller> m_controller;
            winrt::com_ptr<ICoreWebView2> m_webview;
            EventRegistrationToken m_webResourceToken{};
            EventRegistrationToken m_webMessageToken{};
            EventRegistrationToken m_navigationToken{};
            EventRegistrationToken m_newWindowToken{};

            bool m_hotkeyPausedByUs = false;
            bool m_fallbackHandled = false;
            bool m_closing = false;
        };

        void SettingsHost::PauseMainHotkey() noexcept
        {
            if (m_hotkeyPausedByUs)
            {
                return;
            }
            if (SendMainCommand(kPauseHotkeyMessage))
            {
                m_hotkeyPausedByUs = true;
            }
        }

        void SettingsHost::ResumeMainHotkeyIfPaused() noexcept
        {
            if (!m_hotkeyPausedByUs)
            {
                return;
            }
            m_hotkeyPausedByUs = false;
            SendMainCommand(kResumeHotkeyMessage);
        }

        void SettingsHost::PersistWindowState() noexcept
        {
            if (m_hwnd == nullptr)
            {
                return;
            }
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (!GetWindowPlacement(m_hwnd, &wp))
            {
                return;
            }

            // GetWindowPlacement returns rcNormalPosition in workspace coordinates
            // (shifted by taskbar offset when docked top/left), whereas
            // CreateWindowExW takes screen coordinates.
            RECT normal = wp.rcNormalPosition;
            if (!IsIconic(m_hwnd) && !IsZoomed(m_hwnd))
            {
                if (!GetWindowRect(m_hwnd, &normal))
                {
                    return;
                }
            }
            else if (const HMONITOR monitor = MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONEAREST))
            {
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(monitor, &mi))
                {
                    OffsetRect(&normal,
                               mi.rcWork.left - mi.rcMonitor.left,
                               mi.rcWork.top - mi.rcMonitor.top);
                }
            }

            SettingsWindowState next;
            next.hasPlacement = true;
            next.x = normal.left;
            next.y = normal.top;
            next.width = normal.right - normal.left;
            next.height = normal.bottom - normal.top;
            next.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);

            if (next != m_initialState)
            {
                try
                {
                    SaveSettingsState(next, m_statePath);
                    m_initialState = next;
                }
                catch (...)
                {
                }
            }
        }

        void SettingsHost::TriggerFallbackToConfig(const std::wstring& reason) noexcept
        {
            if (m_fallbackHandled || m_closing)
            {
                return;
            }
            m_fallbackHandled = true;

            if (!OpenConfigInShell() && !reason.empty())
            {
                ShowWarning(Failure{ {}, reason }, L"Could not start the AGShot Settings window.");
            }
            if (m_hwnd != nullptr)
            {
                PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            }
        }

        void SettingsHost::UpdateWebViewBackground() noexcept
        {
            if (!m_controller)
            {
                return;
            }
            winrt::com_ptr<ICoreWebView2Controller2> controller2;
            if (SUCCEEDED(m_controller->QueryInterface(IID_PPV_ARGS(controller2.put()))) && controller2)
            {
                const bool dark = IsSystemDarkMode();
                const COREWEBVIEW2_COLOR color = dark
                    ? COREWEBVIEW2_COLOR{ 255, 0x20, 0x20, 0x20 }
                    : COREWEBVIEW2_COLOR{ 255, 0xf3, 0xf3, 0xf3 };
                controller2->put_DefaultBackgroundColor(color);
            }
        }

        void SettingsHost::SendJson(const Json::JsonObject& obj)
        {
            if (!m_webview)
            {
                return;
            }
            const winrt::hstring json = obj.Stringify();
            m_webview->PostWebMessageAsJson(json.c_str());
        }

        void SettingsHost::SendResult(const wchar_t* type, bool ok, const std::wstring& message)
        {
            Json::JsonObject obj;
            obj.Insert(L"type", Json::JsonValue::CreateStringValue(type));
            obj.Insert(L"ok", Json::JsonValue::CreateBooleanValue(ok));
            obj.Insert(L"message", Json::JsonValue::CreateStringValue(message));
            obj.Insert(L"state", BuildStateJson());
            SendJson(obj);
        }

        Json::JsonObject SettingsHost::BuildStateJson()
        {
            const ConfigLoad cfgLoad = LoadConfig(m_configPath);
            const std::filesystem::path dataDir = DataDirectory();
            const std::filesystem::path ocrCacheDir = OcrCacheDirectory();
            const std::filesystem::path modelsDir =
                FindLocalOcrModelDirectory(dataDir, ExecutableDirectory());

            LRESULT rtBits = 0;
            const bool mainRunning = SendMainCommand(kQueryStatusMessage, &rtBits);

            Json::JsonObject configObj;
            configObj.Insert(L"hotkey", Json::JsonValue::CreateStringValue(FormatHotkey(cfgLoad.config.screenshotHotkey)));
            configObj.Insert(L"debugShowStats", Json::JsonValue::CreateBooleanValue(cfgLoad.config.debugShowStats));
            configObj.Insert(L"configOk", Json::JsonValue::CreateBooleanValue(cfgLoad.ok));
            configObj.Insert(L"configError", Json::JsonValue::CreateStringValue(cfgLoad.error));

            Json::JsonObject runtimeObj;
            runtimeObj.Insert(L"mainRunning", Json::JsonValue::CreateBooleanValue(mainRunning));
            runtimeObj.Insert(L"ocrAvailable", Json::JsonValue::CreateBooleanValue((rtBits & kStatusOcrAvailable) != 0));
            runtimeObj.Insert(L"ocrLoaded", Json::JsonValue::CreateBooleanValue((rtBits & kStatusOcrLoaded) != 0));
            runtimeObj.Insert(L"ocrError", Json::JsonValue::CreateBooleanValue((rtBits & kStatusOcrError) != 0));
            runtimeObj.Insert(L"hotkeyRegistered", Json::JsonValue::CreateBooleanValue((rtBits & kStatusHotkeyRegistered) != 0));
            runtimeObj.Insert(L"hotkeyPaused", Json::JsonValue::CreateBooleanValue((rtBits & kStatusHotkeyPaused) != 0));

            Json::JsonObject storageObj;
            storageObj.Insert(L"portable", Json::JsonValue::CreateBooleanValue(IsPortableMode()));
            storageObj.Insert(L"dataDir", Json::JsonValue::CreateStringValue(dataDir.wstring()));
            storageObj.Insert(L"configPath", Json::JsonValue::CreateStringValue(m_configPath.wstring()));

            Json::JsonObject ocrObj;
            ocrObj.Insert(L"modelsFound", Json::JsonValue::CreateBooleanValue(!modelsDir.empty()));
            ocrObj.Insert(L"modelsDir", Json::JsonValue::CreateStringValue(modelsDir.wstring()));

            bool manifestOk = false;
            std::wstring packVersion;
            std::wstring manifestError;

            if (!modelsDir.empty())
            {
                try
                {
                    const OcrModelPack pack = LoadOcrModelManifest(modelsDir);
                    manifestOk = true;
                    packVersion = FromUtf8(pack.packVersion);
                }
                catch (const std::exception& error)
                {
                    manifestError = FromUtf8(error.what());
                }
            }

            const CacheStats cache = MeasureDirectory(ocrCacheDir);
            ocrObj.Insert(L"manifestOk", Json::JsonValue::CreateBooleanValue(manifestOk));
            ocrObj.Insert(L"packVersion", Json::JsonValue::CreateStringValue(packVersion));
            ocrObj.Insert(L"manifestError", Json::JsonValue::CreateStringValue(manifestError));
            ocrObj.Insert(L"cacheBytes", Json::JsonValue::CreateNumberValue(static_cast<double>(cache.bytes)));
            ocrObj.Insert(L"cacheFiles", Json::JsonValue::CreateNumberValue(static_cast<double>(cache.files)));

            Json::JsonObject root;
            root.Insert(L"type", Json::JsonValue::CreateStringValue(L"state"));
            root.Insert(L"version", Json::JsonValue::CreateStringValue(AGSHOT_BUILD_VERSION));
            root.Insert(L"config", configObj);
            root.Insert(L"runtime", runtimeObj);
            root.Insert(L"storage", storageObj);
            root.Insert(L"ocr", ocrObj);
            return root;
        }

        void SettingsHost::SendState()
        {
            SendJson(BuildStateJson());
        }

        void SettingsHost::HandleSetHotkey(const std::wstring& value)
        {
            Hotkey parsed;
            std::wstring parseError;
            auto reply = [this](bool ok, const std::wstring& message) {
                SendResult(L"hotkeyResult", ok, message);
            };

            if (!ParseHotkey(value, parsed, parseError))
            {
                ResumeMainHotkeyIfPaused();
                reply(false, L"Invalid hotkey: " + parseError);
                return;
            }

            // Ensure main's registration is paused while probing availability so we
            // don't collide with AGShot itself.
            PauseMainHotkey();

            if (!RegisterHotKey(m_hwnd, kProbeHotkeyId, parsed.modifiers | MOD_NOREPEAT, parsed.key))
            {
                const DWORD err = GetLastError();
                ResumeMainHotkeyIfPaused();
                if (err == ERROR_HOTKEY_ALREADY_REGISTERED)
                {
                    reply(false, L"\"" + FormatHotkey(parsed)
                                     + L"\" is already registered by another application.");
                }
                else
                {
                    reply(false, L"Could not test hotkey registration (error " + std::to_wstring(err) + L").");
                }
                return;
            }
            UnregisterHotKey(m_hwnd, kProbeHotkeyId);

            ConfigLoad cfgLoad = LoadConfig(m_configPath);
            cfgLoad.config.screenshotHotkey = parsed;
            try
            {
                SaveConfig(cfgLoad.config, m_configPath);
            }
            catch (const std::exception& error)
            {
                ResumeMainHotkeyIfPaused();
                reply(false, L"Failed to save config.toml: " + FromUtf8(error.what()));
                return;
            }

            // Tell main to resume & reload immediately so runtime state updates without waiting
            // for ConfigWatcher debounce.
            m_hotkeyPausedByUs = false;
            SendMainCommand(kResumeHotkeyMessage);

            reply(true, L"Capture hotkey updated to " + FormatHotkey(parsed) + L".");
        }

        void SettingsHost::HandleSetDebugStats(bool enabled)
        {
            ConfigLoad cfgLoad = LoadConfig(m_configPath);
            cfgLoad.config.debugShowStats = enabled;
            try
            {
                SaveConfig(cfgLoad.config, m_configPath);
                SendMainCommand(kReloadConfigMessage);
                SendState();
            }
            catch (const std::exception& error)
            {
                SendResult(L"actionResult", false, L"Failed to save config.toml: " + FromUtf8(error.what()));
            }
        }

        bool SettingsHost::OpenConfigInShell()
        {
            std::error_code ec;
            if (!std::filesystem::exists(m_configPath, ec))
            {
                try
                {
                    SaveConfig(Config{}, m_configPath);
                }
                catch (...)
                {
                }
            }
            return OpenShellTarget(m_configPath.c_str(), L"ShellExecuteW(open config)");
        }

        bool SettingsHost::RequestAndWaitOcrTrim(int maxPolls)
        {
            LRESULT ignored = 0;
            if (!SendMainCommand(kTrimOcrMemoryMessage, &ignored))
            {
                return false;
            }
            for (int i = 0; i < maxPolls; ++i)
            {
                Sleep(35);
                LRESULT status = 0;
                if (SendMainCommand(kQueryStatusMessage, &status) && (status & kStatusOcrLoaded) == 0)
                {
                    break;
                }
            }
            return true;
        }

        void SettingsHost::HandleTrimOcr()
        {
            const bool sent = RequestAndWaitOcrTrim(6);
            SendResult(L"actionResult", sent,
                       sent ? L"Requested NPU OCR engine unload." : L"AGShot tray host is not running.");
        }

        void SettingsHost::HandleClearOcrCache()
        {
            LRESULT rtBits = 0;
            if (SendMainCommand(kQueryStatusMessage, &rtBits) && (rtBits & kStatusOcrLoaded) != 0)
            {
                RequestAndWaitOcrTrim(8);
            }

            const std::filesystem::path cacheDir = OcrCacheDirectory();
            const ClearResult res = ClearDirectoryContents(cacheDir);

            std::wstring msg;
            if (res.failedEntries == 0)
            {
                msg = L"Cleared " + std::to_wstring(res.deletedFiles) + L" cached file(s).";
            }
            else
            {
                msg = L"Removed " + std::to_wstring(res.deletedFiles) + L" file(s); "
                    + std::to_wstring(res.failedEntries) + L" item(s) could not be removed (in use).";
            }

            SendResult(L"actionResult", res.failedEntries == 0, msg);
        }

        void SettingsHost::OnWebResourceRequested(
            ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args)
        {
            if (args == nullptr || !m_env)
            {
                return;
            }

            winrt::com_ptr<ICoreWebView2WebResourceRequest> request;
            if (FAILED(args->get_Request(request.put())) || !request)
            {
                return;
            }
            wil::unique_cotaskmem_string uri;
            if (FAILED(request->get_Uri(uri.put())) || !uri)
            {
                return;
            }

            const std::wstring_view target{ uri.get() };
            constexpr std::wstring_view kIconsPrefix = L"https://app.local/icons/";
            constexpr std::wstring_view kSvgSuffix = L".svg";
            if (target.size() > kIconsPrefix.size() + kSvgSuffix.size()
                && target.compare(0, kIconsPrefix.size(), kIconsPrefix) == 0
                && target.compare(target.size() - kSvgSuffix.size(), kSvgSuffix.size(), kSvgSuffix) == 0)
            {
                const std::wstring_view wname = target.substr(
                    kIconsPrefix.size(),
                    target.size() - kIconsPrefix.size() - kSvgSuffix.size());
                std::string name;
                name.reserve(wname.size());
                bool safeName = !wname.empty();
                for (wchar_t ch : wname)
                {
                    if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')
                        || ch == L'_' || ch == L'-')
                    {
                        name.push_back(static_cast<char>(ch));
                    }
                    else
                    {
                        safeName = false;
                        break;
                    }
                }
                const std::string_view svg = safeName
                    ? SharedHudIconPack().SvgByName(name)
                    : std::string_view{};
                if (!svg.empty())
                {
                    winrt::com_ptr<IStream> iconStream;
                    if (SUCCEEDED(CreateMemoryStream(svg.data(), static_cast<DWORD>(svg.size()), iconStream)))
                    {
                        winrt::com_ptr<ICoreWebView2WebResourceResponse> iconResponse;
                        if (SUCCEEDED(m_env->CreateWebResourceResponse(
                                iconStream.get(), 200, L"OK",
                                L"Content-Type: image/svg+xml; charset=utf-8\r\nCache-Control: immutable, max-age=3600",
                                iconResponse.put()))
                            && iconResponse)
                        {
                            args->put_Response(iconResponse.get());
                            return;
                        }
                    }
                }
                winrt::com_ptr<ICoreWebView2WebResourceResponse> notFound;
                if (SUCCEEDED(m_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", notFound.put())))
                {
                    args->put_Response(notFound.get());
                }
                return;
            }

            if (target != kSettingsUrl)
            {
                winrt::com_ptr<ICoreWebView2WebResourceResponse> emptyResponse;
                if (SUCCEEDED(m_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", emptyResponse.put())))
                {
                    args->put_Response(emptyResponse.get());
                }
                return;
            }

            std::vector<std::uint8_t> htmlBytes;
            if (!LoadMszipResource(GetModuleHandleW(nullptr), IDR_SETTINGS_HTML, htmlBytes))
            {
                return;
            }

            winrt::com_ptr<IStream> stream;
            if (FAILED(CreateMemoryStream(htmlBytes.data(), static_cast<DWORD>(htmlBytes.size()), stream)))
            {
                return;
            }

            winrt::com_ptr<ICoreWebView2WebResourceResponse> response;
            if (SUCCEEDED(m_env->CreateWebResourceResponse(
                    stream.get(), 200, L"OK",
                    L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store",
                    response.put()))
                && response)
            {
                args->put_Response(response.get());
            }
        }

        void SettingsHost::OnWebMessageReceived(
            ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args)
        {
            if (args == nullptr)
            {
                return;
            }
            wil::unique_cotaskmem_string raw;
            if (FAILED(args->get_WebMessageAsJson(raw.put())) || !raw)
            {
                return;
            }

            Json::JsonObject msg;
            if (!Json::JsonObject::TryParse(raw.get(), msg))
            {
                return;
            }

            const std::wstring type{ msg.GetNamedString(L"type", L"") };
            if (type == L"init" || type == L"refresh")
            {
                SendState();
            }
            else if (type == L"hotkeyPause")
            {
                if (msg.GetNamedBoolean(L"paused", false))
                {
                    PauseMainHotkey();
                }
                else
                {
                    ResumeMainHotkeyIfPaused();
                }
                SendState();
            }
            else if (type == L"setHotkey")
            {
                HandleSetHotkey(std::wstring{ msg.GetNamedString(L"value", L"") });
            }
            else if (type == L"setDebugStats")
            {
                HandleSetDebugStats(msg.GetNamedBoolean(L"value", false));
            }
            else if (type == L"trimOcrMemory")
            {
                HandleTrimOcr();
            }
            else if (type == L"clearOcrCache")
            {
                HandleClearOcrCache();
            }
            else if (type == L"openConfig")
            {
                OpenConfigInShell();
            }
            else if (type == L"openDataFolder")
            {
                const auto dir = DataDirectory();
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                OpenShellTarget(dir.c_str(), L"ShellExecuteW(open data folder)");
            }
            else if (type == L"openModelsFolder")
            {
                std::filesystem::path dir = FindLocalOcrModelDirectory(DataDirectory(), ExecutableDirectory());
                if (dir.empty())
                {
                    dir = DataDirectory() / L"models" / L"ocr";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                }
                OpenShellTarget(dir.c_str(), L"ShellExecuteW(open models folder)");
            }
            else if (type == L"openRepo")
            {
                OpenShellTarget(L"https://github.com/apkipa/AGShot", L"ShellExecuteW(open github)");
            }
        }

        class ControllerCallback
            : public winrt::implements<ControllerCallback, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
        {
        public:
            explicit ControllerCallback(SettingsHost& host) noexcept : m_host(host) {}
            HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override
            {
                m_host.OnControllerCreated(result, controller);
                return S_OK;
            }
        private:
            SettingsHost& m_host;
        };

        class EnvironmentCallback
            : public winrt::implements<EnvironmentCallback, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
        {
        public:
            explicit EnvironmentCallback(SettingsHost& host) noexcept : m_host(host) {}
            HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override
            {
                m_host.OnEnvironmentCreated(result, env);
                return S_OK;
            }
        private:
            SettingsHost& m_host;
        };

        class WebResourceCallback
            : public winrt::implements<WebResourceCallback, ICoreWebView2WebResourceRequestedEventHandler>
        {
        public:
            explicit WebResourceCallback(SettingsHost& host) noexcept : m_host(host) {}
            HRESULT STDMETHODCALLTYPE Invoke(
                ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) override
            {
                m_host.OnWebResourceRequested(sender, args);
                return S_OK;
            }
        private:
            SettingsHost& m_host;
        };

        class WebMessageCallback
            : public winrt::implements<WebMessageCallback, ICoreWebView2WebMessageReceivedEventHandler>
        {
        public:
            explicit WebMessageCallback(SettingsHost& host) noexcept : m_host(host) {}
            HRESULT STDMETHODCALLTYPE Invoke(
                ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override
            {
                m_host.OnWebMessageReceived(sender, args);
                return S_OK;
            }
        private:
            SettingsHost& m_host;
        };

        class NavigationStartingCallback
            : public winrt::implements<NavigationStartingCallback, ICoreWebView2NavigationStartingEventHandler>
        {
        public:
            HRESULT STDMETHODCALLTYPE Invoke(
                ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) override
            {
                if (args == nullptr) return S_OK;
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(args->get_Uri(uri.put())) && uri)
                {
                    if (std::wstring_view{ uri.get() } != kSettingsUrl)
                    {
                        args->put_Cancel(TRUE);
                    }
                }
                return S_OK;
            }
        };

        class NewWindowCallback
            : public winrt::implements<NewWindowCallback, ICoreWebView2NewWindowRequestedEventHandler>
        {
        public:
            HRESULT STDMETHODCALLTYPE Invoke(
                ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) override
            {
                if (args != nullptr)
                {
                    args->put_Handled(TRUE);
                }
                return S_OK;
            }
        };

        void SettingsHost::OnEnvironmentCreated(HRESULT result, ICoreWebView2Environment* environment)
        {
            if (m_closing)
            {
                return;
            }
            if (FAILED(result) || environment == nullptr)
            {
                TriggerFallbackToConfig(L"WebView2 environment creation failed.");
                return;
            }

            m_env.copy_from(environment);
            const auto handler = winrt::make<ControllerCallback>(*this);
            const HRESULT hr = m_env->CreateCoreWebView2Controller(m_hwnd, handler.get());
            if (FAILED(hr))
            {
                TriggerFallbackToConfig(L"WebView2 controller creation failed.");
            }
        }

        void SettingsHost::OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller)
        {
            if (m_closing)
            {
                if (controller != nullptr)
                {
                    controller->Close();
                }
                return;
            }
            if (FAILED(result) || controller == nullptr)
            {
                TriggerFallbackToConfig(L"WebView2 controller initialization failed.");
                return;
            }

            m_controller.copy_from(controller);
            UpdateWebViewBackground();

            RECT client{};
            GetClientRect(m_hwnd, &client);
            m_controller->put_Bounds(client);
            m_controller->put_IsVisible(TRUE);

            if (FAILED(m_controller->get_CoreWebView2(m_webview.put())) || !m_webview)
            {
                TriggerFallbackToConfig(L"CoreWebView2 interface unavailable.");
                return;
            }

            winrt::com_ptr<ICoreWebView2Settings> settings;
            if (SUCCEEDED(m_webview->get_Settings(settings.put())) && settings)
            {
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_AreDevToolsEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE);
                settings->put_IsZoomControlEnabled(FALSE);
                winrt::com_ptr<ICoreWebView2Settings3> settings3;
                if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(settings3.put()))) && settings3)
                {
                    settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                }
            }

            // Match https://app.local/* explicitly.
            m_webview->AddWebResourceRequestedFilter(
                L"https://app.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

            m_webview->add_WebResourceRequested(
                winrt::make<WebResourceCallback>(*this).get(), &m_webResourceToken);
            m_webview->add_WebMessageReceived(
                winrt::make<WebMessageCallback>(*this).get(), &m_webMessageToken);
            m_webview->add_NavigationStarting(
                winrt::make<NavigationStartingCallback>().get(), &m_navigationToken);
            m_webview->add_NewWindowRequested(
                winrt::make<NewWindowCallback>().get(), &m_newWindowToken);

            const HRESULT navHr = m_webview->Navigate(kSettingsUrl);
            if (FAILED(navHr))
            {
                TriggerFallbackToConfig(L"WebView2 could not load the settings page.");
            }
        }

        void SettingsHost::StartWebView()
        {
            const std::filesystem::path userDataDir = WebView2UserDataDirectory();
            std::error_code ec;
            std::filesystem::create_directories(userDataDir, ec);

            const auto handler = winrt::make<EnvironmentCallback>(*this);
            const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
                nullptr,
                userDataDir.empty() ? nullptr : userDataDir.c_str(),
                nullptr,
                handler.get());
            if (FAILED(hr))
            {
                PostMessageW(m_hwnd, kFallbackOpenConfigMsg, 0, 0);
            }
        }

        LRESULT SettingsHost::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            switch (message)
            {
            case kFallbackOpenConfigMsg:
                TriggerFallbackToConfig(L"WebView2 Runtime was not found on this system.");
                return 0;

            case WM_SIZE:
                if (m_controller)
                {
                    RECT client{};
                    GetClientRect(hwnd, &client);
                    m_controller->put_Bounds(client);
                }
                return 0;

            case WM_SETFOCUS:
                if (m_controller)
                {
                    m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                }
                return 0;

            case WM_ACTIVATE:
                if (LOWORD(wparam) != WA_INACTIVE && m_webview)
                {
                    SendState();
                }
                return 0;

            case WM_ERASEBKGND:
            {
                auto* hdc = reinterpret_cast<HDC>(wparam);
                RECT client{};
                GetClientRect(hwnd, &client);
                const COLORREF bgColor = IsSystemDarkMode() ? RGB(0x20, 0x20, 0x20) : RGB(0xf3, 0xf3, 0xf3);
                const wil::unique_hbrush brush{ CreateSolidBrush(bgColor) };
                if (brush)
                {
                    FillRect(hdc, &client, brush.get());
                }
                return 1;
            }

            case WM_SETTINGCHANGE:
                if (lparam != 0 && _wcsicmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0)
                {
                    ApplyDarkTitleBar(hwnd, IsSystemDarkMode());
                    UpdateWebViewBackground();
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                return 0;

            case WM_DPICHANGED:
            {
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                if (suggested != nullptr)
                {
                    SetWindowPos(
                        hwnd, nullptr,
                        suggested->left, suggested->top,
                        suggested->right - suggested->left,
                        suggested->bottom - suggested->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                return 0;
            }

            case WM_GETMINMAXINFO:
            {
                auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
                const UINT dpi = hwnd != nullptr ? GetDpiForWindow(hwnd) : 96;
                info->ptMinTrackSize.x = MulDiv(kMinWidthDip, static_cast<int>(dpi ? dpi : 96), 96);
                info->ptMinTrackSize.y = MulDiv(kMinHeightDip, static_cast<int>(dpi ? dpi : 96), 96);
                return 0;
            }

            case WM_CLOSE:
                m_closing = true;
                PersistWindowState();
                // Hide the top-level window while its rendered WebView content is still
                // intact so closing never flashes an empty host frame during teardown.
                ShowWindow(hwnd, SW_HIDE);
                ResumeMainHotkeyIfPaused();
                if (m_controller)
                {
                    m_controller->Close();
                    m_controller = nullptr;
                }
                m_webview = nullptr;
                m_env = nullptr;
                DestroyWindow(hwnd);
                return 0;

            case WM_DESTROY:
                m_hwnd = nullptr;
                PostQuitMessage(0);
                return 0;
            }

            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        LRESULT CALLBACK SettingsHost::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            if (message == WM_NCCREATE)
            {
                const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
                return DefWindowProcW(hwnd, message, wparam, lparam);
            }

            auto* host = reinterpret_cast<SettingsHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (message == WM_NCDESTROY)
            {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            if (host == nullptr)
            {
                return DefWindowProcW(hwnd, message, wparam, lparam);
            }

            try
            {
                return host->HandleMessage(hwnd, message, wparam, lparam);
            }
            catch (...)
            {
                ReportCurrentException(__FILEW__, __LINE__);
                PostQuitMessage(1);
                return 0;
            }
        }

        int SettingsHost::Run(int showCommand)
        {
            m_configPath = ConfigPath();
            m_statePath = StatePath();
            m_initialState = LoadSettingsState(m_statePath);
            SetDiagnosticContext(L"Config: " + m_configPath.wstring());

            // Shared icon resource: LR_SHARED is owned by the system and must not be destroyed.
            HICON icon = static_cast<HICON>(LoadImageW(
                m_instance, MAKEINTRESOURCEW(IDI_AGSHOT), IMAGE_ICON,
                0, 0, LR_DEFAULTSIZE | LR_SHARED));
            HICON smallIcon = static_cast<HICON>(LoadImageW(
                m_instance, MAKEINTRESOURCEW(IDI_AGSHOT), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = m_instance;
            wc.hIcon = icon;
            wc.hIconSm = smallIcon != nullptr ? smallIcon : icon;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = kSettingsWindowClass;
            if (!RegisterClassExW(&wc))
            {
                const DWORD err = GetLastError();
                if (err != ERROR_CLASS_ALREADY_EXISTS)
                {
                    AGSHOT_CHECK_WIN32(false);
                }
            }

            const RECT bounds = ComputeInitialBounds(m_initialState);
            m_hwnd = CreateWindowExW(
                0,
                kSettingsWindowClass,
                L"AGShot Settings",
                WS_OVERLAPPEDWINDOW,
                bounds.left, bounds.top,
                bounds.right - bounds.left,
                bounds.bottom - bounds.top,
                nullptr, nullptr, m_instance, this);
            AGSHOT_CHECK_WIN32(m_hwnd != nullptr);

            ApplyDarkTitleBar(m_hwnd, IsSystemDarkMode());

            const int cmd = m_initialState.maximized
                ? SW_SHOWMAXIMIZED
                : (showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
            ShowWindow(m_hwnd, cmd);
            UpdateWindow(m_hwnd);

            StartWebView();

            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            return static_cast<int>(msg.wParam);
        }
    }

    int RunSettingsProcess(HINSTANCE instance, int showCommand)
    {
        SettingsHost host{ instance };
        return host.Run(showCommand);
    }
}
