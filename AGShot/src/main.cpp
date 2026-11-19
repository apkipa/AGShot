#include "pch.h"
#include "resource.h"
#include "core/app_ipc.h"
#include "core/config.h"
#include "core/ocr_manifest.h"
#include "core/ocr_runtime.h"
#include "core/config_watcher.h"
#include "core/error.h"
#include "core/tray_icon.h"
#include "hud/overlay.h"
#include "settings/settings_window.h"

using namespace winrt;

namespace
{
    constexpr UINT kSettingsExitedMessage = WM_APP + 9;

    // Names the capture combination in WM_HOTKEY. AGShot registers exactly one,
    // so the number only has to be distinct within this window.
    constexpr int kScreenshotHotkeyId = 1;

    // Owns the live application state inside RunApp() so that member teardown
    // runs deterministically in reverse order (watcher -> tray -> overlay ->
    // message window/hotkey -> ocrRuntime) before wWinMain returns and static
    // NPU objects begin tearing down.
    struct AppHost
    {
        agshot::OcrRuntime ocrRuntime;
        agshot::Config config;
        std::filesystem::path configPath;
        std::wstring reportedConfigError;
        bool hotkeyRegistered{};
        bool hotkeyPaused{};
        bool fatal{};
        bool* restartRequested{};
        wil::unique_hicon trayIcon;
        HWND hwnd{};
        agshot::Overlay overlay;
        agshot::ConfigWatcher watcher;
        wil::unique_handle settingsJob;
        wil::unique_handle settingsProcess;
        DWORD settingsProcessId{};
        HANDLE settingsWait{};

        ~AppHost();
    };

    void ReleaseSettingsWait(AppHost& app, bool waitForCallback) noexcept
    {
        if (app.settingsWait != nullptr)
        {
            UnregisterWaitEx(app.settingsWait, waitForCallback ? INVALID_HANDLE_VALUE : nullptr);
            app.settingsWait = nullptr;
        }
    }

    bool IsSettingsProcessRunning(const AppHost& app) noexcept
    {
        return app.settingsProcess
            && WaitForSingleObject(app.settingsProcess.get(), 0) == WAIT_TIMEOUT;
    }

    void ClearSettingsProcess(AppHost& app, bool waitForCallback) noexcept
    {
        ReleaseSettingsWait(app, waitForCallback);
        app.settingsProcess.reset();
        app.settingsProcessId = 0;
    }

    void ShutdownSettingsChild(AppHost& app) noexcept
    {
        ReleaseSettingsWait(app, true);

        if (IsSettingsProcessRunning(app))
        {
            if (const HWND settingsHwnd = FindWindowW(agshot::kSettingsWindowClass, nullptr);
                settingsHwnd != nullptr)
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(settingsHwnd, &pid);
                if (pid == app.settingsProcessId)
                {
                    PostMessageW(settingsHwnd, WM_CLOSE, 0, 0);
                    WaitForSingleObject(app.settingsProcess.get(), 500);
                }
            }
        }

        ClearSettingsProcess(app, false);
        app.settingsJob.reset();
    }

    AppHost::~AppHost()
    {
        ShutdownSettingsChild(*this);
        watcher.Stop();
        agshot::StopTrayIcon();
        overlay.Destroy();

        if (hotkeyRegistered && hwnd != nullptr)
        {
            UnregisterHotKey(hwnd, kScreenshotHotkeyId);
            hotkeyRegistered = false;
        }
        if (hwnd != nullptr && IsWindow(hwnd))
        {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }

    void ShowSettings(AppHost& app);
    void ReloadConfig(AppHost& app, bool force);

    // What the tray icon asks of the application. The tray itself, including its
    // hidden owner window and its menu, lives in core/tray_icon.cpp.
    void CaptureFromTray(void* context)
    {
        static_cast<AppHost*>(context)->overlay.Begin();
    }

    void ExitFromTray(void* context)
    {
        auto& app = *static_cast<AppHost*>(context);
        if (app.hwnd != nullptr)
        {
            DestroyWindow(app.hwnd);
        }
    }

    void RestartFromTray(void* context)
    {
        auto& app = *static_cast<AppHost*>(context);
        if (app.restartRequested != nullptr)
        {
            *app.restartRequested = true;
        }
        if (app.hwnd != nullptr)
        {
            DestroyWindow(app.hwnd);
        }
    }

    void FatalFromTray(void* context)
    {
        static_cast<AppHost*>(context)->fatal = true;
        PostQuitMessage(1);
    }

    void SettingsFromTray(void* context)
    {
        ShowSettings(*static_cast<AppHost*>(context));
    }

    void ReloadFromTray(void* context)
    {
        // Forced: the point of the menu item is to re-apply settings that are
        // already loaded, so that anything which failed the first time - a hotkey
        // another application was holding, say - gets another go.
        ReloadConfig(*static_cast<AppHost*>(context), true);
    }

    // Owns the capture combination for as long as the window lives. Called again
    // whenever the settings change, so a new combination replaces the old one.
    void ApplyHotkey(AppHost& app, const agshot::Hotkey& hotkey)
    {
        if (app.hotkeyRegistered)
        {
            UnregisterHotKey(app.hwnd, kScreenshotHotkeyId);
            app.hotkeyRegistered = false;
        }

        if (!hotkey.valid() || app.hwnd == nullptr)
        {
            return;
        }

        // MOD_NOREPEAT matters: without it, holding the key down fires the hotkey
        // again and again, which for a capture is not what anyone means by one
        // press. It is a registration detail, not part of what the file stores.
        if (RegisterHotKey(app.hwnd, kScreenshotHotkeyId, hotkey.modifiers | MOD_NOREPEAT, hotkey.key))
        {
            app.hotkeyRegistered = true;
            return;
        }

        // Another application holding the combination is the ordinary way for
        // this to fail, and it is not worth a dialog: the user picked the
        // combination, and can pick a different one in the file. Anything else
        // would be a real fault, so it is reported.
        const DWORD error = GetLastError();
        if (error != ERROR_HOTKEY_ALREADY_REGISTERED)
        {
            agshot::ShowWarning(
                agshot::DescribeWin32(L"RegisterHotKey", error, __FILEW__, __LINE__),
                L"AGShot could not register the capture hotkey.");
        }
    }

    // Everything a live setting has to touch goes here, so that there is exactly
    // one place where the running application is brought in line with the file.
    void ApplyConfig(AppHost& app, const agshot::Config& config)
    {
        if (!app.hotkeyPaused)
        {
            ApplyHotkey(app, config.screenshotHotkey);
        }
        app.overlay.SetDebugStats(config.debugShowStats);
    }

    void CALLBACK OnSettingsProcessExited(PVOID context, BOOLEAN) noexcept
    {
        auto* hwnd = static_cast<HWND>(context);
        if (hwnd != nullptr)
        {
            PostMessageW(hwnd, kSettingsExitedMessage, 0, 0);
        }
    }

    bool FocusExistingSettingsWindow() noexcept
    {
        const HWND existing = FindWindowW(agshot::kSettingsWindowClass, nullptr);
        if (existing == nullptr)
        {
            return false;
        }
        if (IsIconic(existing))
        {
            ShowWindow(existing, SW_RESTORE);
        }
        SetForegroundWindow(existing);
        return true;
    }

    // Launches `AGShot.exe --settings` bound to a kill-on-close Job Object so
    // WebView2 lives only while the settings window is open and cleans up if
    // the tray host exits.
    void ShowSettings(AppHost& app)
    {
        if (FocusExistingSettingsWindow())
        {
            return;
        }

        if (IsSettingsProcessRunning(app))
        {
            return; // Child process is still starting up.
        }
        if (app.settingsProcess)
        {
            ClearSettingsProcess(app, true);
        }

        if (!app.settingsJob)
        {
            wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
            if (job)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (SetInformationJobObject(
                        job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                {
                    app.settingsJob = std::move(job);
                }
            }
        }

        const auto exePath = agshot::ExecutableFilePath();
        if (exePath.empty())
        {
            agshot::ShowWarning(
                agshot::DescribeHresult(L"GetModuleFileNameW(settings)", agshot::LastErrorHresult(), __FILEW__, __LINE__),
                L"The settings window could not be launched.");
            return;
        }

        std::wstring commandLine = L"\"" + exePath.wstring() + L"\" ";
        commandLine += agshot::kSettingsArg;

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
        {
            agshot::ShowWarning(
                agshot::DescribeWin32(L"CreateProcessW(settings)", GetLastError(), __FILEW__, __LINE__),
                L"The settings window could not be launched.");
            return;
        }

        wil::unique_handle childThread{ process.hThread };
        wil::unique_handle childProcess{ process.hProcess };

        if (app.settingsJob)
        {
            AssignProcessToJobObject(app.settingsJob.get(), childProcess.get());
        }

        ResumeThread(childThread.get());

        app.settingsProcess = std::move(childProcess);
        app.settingsProcessId = process.dwProcessId;
        RegisterWaitForSingleObject(
            &app.settingsWait,
            app.settingsProcess.get(),
            OnSettingsProcessExited,
            app.hwnd,
            INFINITE,
            WT_EXECUTEONLYONCE);
    }

    // Reads the settings file again and applies whatever changed. Runs on the UI
    // thread, from the message the watcher posts.
    //
    // "force" applies the settings even when the values are unchanged, which is
    // what the tray's Reload item is for.
    void ReloadConfig(AppHost& app, bool force)
    {
        const auto load = agshot::LoadConfig(app.configPath);

        if (!load.ok)
        {
            // Once per distinct problem: an editor that keeps saving a file that
            // stays broken must not stack up one dialog per save.
            if (load.error != app.reportedConfigError)
            {
                app.reportedConfigError = load.error;
                agshot::ShowWarning(
                    agshot::Failure{ {}, app.configPath.wstring() + L"\n" + load.error },
                    L"The settings file changed, but AGShot could not read it, so the previous settings are still in use.");
            }
            return;
        }

        app.reportedConfigError.clear();

        // Rewriting the file without changing anything is not a change, and a
        // deleted file puts the defaults back in force.
        if (!force && load.config == app.config)
        {
            return;
        }

        app.config = load.config;
        ApplyConfig(app, app.config);
    }

    // Runs on the watcher thread. Posting is the whole job: applying a setting
    // touches windows, and the message queue is the only synchronisation this
    // application needs.
    void OnConfigWatchEvent(HWND hwnd, agshot::WatchEvent event, const std::wstring& detail)
    {
        if (hwnd == nullptr)
        {
            return;
        }

        if (event == agshot::WatchEvent::Changed)
        {
            PostMessageW(hwnd, agshot::kConfigChangedMessage, 0, 0);
            return;
        }

        // PostMessageW cannot carry a string, so the reason travels as a heap
        // pointer that the handler takes back. Saying nothing would leave the
        // user editing a file that nothing is listening to.
        auto* text = new (std::nothrow) std::wstring{ detail };
        if (text != nullptr
            && !PostMessageW(hwnd, agshot::kConfigWatchLostMessage, 0, reinterpret_cast<LPARAM>(text)))
        {
            delete text;
        }
    }

    LRESULT HandleMessage(AppHost& app, HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case agshot::kWakeExistingMessage:
            app.overlay.Begin();
            return 0;

        case agshot::kConfigChangedMessage:
            ReloadConfig(app, false);
            return 0;

        case agshot::kReloadConfigMessage:
            ReloadConfig(app, true);
            return 1;

        case agshot::kPauseHotkeyMessage:
            app.hotkeyPaused = true;
            if (app.hotkeyRegistered)
            {
                UnregisterHotKey(hwnd, kScreenshotHotkeyId);
                app.hotkeyRegistered = false;
            }
            return 1;

        case agshot::kResumeHotkeyMessage:
            app.hotkeyPaused = false;
            ReloadConfig(app, true);
            return app.hotkeyRegistered ? 1 : 0;

        case agshot::kQueryStatusMessage:
        {
            LRESULT bits = 0;
            if (app.ocrRuntime.available()) bits |= agshot::kStatusOcrAvailable;
            if (app.ocrRuntime.hasLoadedEngine()) bits |= agshot::kStatusOcrLoaded;
            if (app.ocrRuntime.hasInitializationError()) bits |= agshot::kStatusOcrError;
            if (app.hotkeyRegistered) bits |= agshot::kStatusHotkeyRegistered;
            if (app.hotkeyPaused) bits |= agshot::kStatusHotkeyPaused;
            return bits;
        }

        case agshot::kTrimOcrMemoryMessage:
            app.ocrRuntime.Trim();
            return 1;

        case agshot::kReloadOcrModelsMessage:
        {
            const auto dataDirectory = app.configPath.parent_path();
            const auto ocrModels = agshot::FindLocalOcrModelDirectory(
                dataDirectory, agshot::ExecutableDirectory());
            const auto ocrCache = agshot::OcrCacheDirectory();
            std::error_code ocrCacheError;
            std::filesystem::create_directories(ocrCache, ocrCacheError);
            app.ocrRuntime.Reload(ocrModels, ocrCacheError ? std::filesystem::path{} : ocrCache);
            return app.ocrRuntime.available() ? 1 : 0;
        }

        case kSettingsExitedMessage:
            // If ShowSettings already replaced a just-exited child before this
            // queued notification was pumped, the current handle is still alive.
            if (IsSettingsProcessRunning(app))
            {
                return 0;
            }
            ClearSettingsProcess(app, false);
            if (app.hotkeyPaused)
            {
                app.hotkeyPaused = false;
                ApplyHotkey(app, app.config.screenshotHotkey);
            }
            return 0;

        case agshot::kConfigWatchLostMessage:
        {
            // The watcher handed the reason over as a heap string; take it back.
            const std::unique_ptr<std::wstring> detail{ reinterpret_cast<std::wstring*>(lparam) };
            agshot::ShowWarning(
                agshot::Failure{ {}, app.configPath.wstring() + L"\n"
                                     + (detail ? *detail : std::wstring{}) },
                L"AGShot is no longer watching its settings file, so further changes will need a restart.");
            return 0;
        }

        case WM_HOTKEY:
            // RegisterHotKey posts to the window it was registered for, so the id
            // is the only thing worth checking.
            if (static_cast<int>(wparam) == kScreenshotHotkeyId)
            {
                app.overlay.Begin();
            }
            return 0;

        case WM_DESTROY:
            // The window is going away; hand the combination back rather than
            // leaving it to the process teardown.
            if (app.hotkeyRegistered)
            {
                UnregisterHotKey(hwnd, kScreenshotHotkeyId);
                app.hotkeyRegistered = false;
            }
            app.hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // DispatchMessageW has no handler above it, so an exception escaping a
    // message handler would not unwind: the CRT would call abort() and the user
    // would get a runtime dialog saying nothing useful. Catch it here instead.
    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        auto* app = reinterpret_cast<AppHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCDESTROY)
        {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        if (app == nullptr || app->fatal)
        {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        try
        {
            return HandleMessage(*app, hwnd, message, wparam, lparam);
        }
        catch (...)
        {
            if (!app->fatal)
            {
                app->fatal = true;
                agshot::ReportCurrentException(__FILEW__, __LINE__);
                PostQuitMessage(1);
            }
            return 0;
        }
    }

    bool WakeExistingInstance()
    {
        for (int attempt = 0; attempt < 150; ++attempt)
        {
            const HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr, agshot::kMessageClass, nullptr);
            if (hwnd != nullptr)
            {
                DWORD processId{};
                GetWindowThreadProcessId(hwnd, &processId);
                if (processId != 0)
                {
                    AllowSetForegroundWindow(processId);
                }
                if (PostMessageW(hwnd, agshot::kWakeExistingMessage, 0, 0))
                {
                    return true;
                }
            }
            Sleep(20);
        }
        return false;
    }

    HRESULT LaunchSelfRestart() noexcept
    {
        const auto path = agshot::ExecutableFilePath();
        if (path.empty())
        {
            return agshot::LastErrorHresult();
        }

        std::wstring commandLine = L"\"" + path.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &startup, &process))
        {
            return agshot::LastErrorHresult();
        }
        const wil::unique_handle childThread{ process.hThread };
        const wil::unique_handle childProcess{ process.hProcess };
        return S_OK;
    }
}

int RunApp(HINSTANCE instance, int showCommand, bool& restartRequested)
{
    (void)showCommand;

    EnableMouseInPointer(true);

    init_apartment(apartment_type::single_threaded);

    // Settings live outside the build output so they survive a rebuild. The
    // first run writes the file out; later runs read whatever is there.
    const auto configPath = agshot::ConfigPath();
    const auto config = agshot::LoadConfig(configPath);

    // What the running application is using, so that a reload can tell a real
    // change from a rewrite of the same values.
    AppHost app;
    app.restartRequested = &restartRequested;
    app.configPath = configPath;
    app.config = config.config;
    // The startup warning below covers this problem already, so do not repeat it
    // the first time the file is touched.
    app.reportedConfigError = config.ok ? std::wstring{} : config.error;

    // Shown at the bottom of every report, so it is obvious which file was in play.
    agshot::SetDiagnosticContext(L"Config: " + configPath.wstring());

    // Model distribution is intentionally external. Detect the local payload at
    // startup silently (without blocking capture/annotation if absent), and never
    // attempt a model load on the UI thread.
    const auto dataDirectory = configPath.parent_path();
    const auto ocrModels = agshot::FindLocalOcrModelDirectory(
        dataDirectory, agshot::ExecutableDirectory());
    const auto ocrCache = agshot::OcrCacheDirectory();
    std::error_code ocrCacheError;
    std::filesystem::create_directories(ocrCache, ocrCacheError);
    // Start() refuses an empty models path, so a missing payload leaves the
    // runtime unavailable rather than failing later on the pool thread.
    if (!ocrCacheError) app.ocrRuntime.Start(ocrModels, ocrCache);
    app.overlay.SetOcrRuntime(&app.ocrRuntime);

    if (!config.exists || config.migrated)
    {
        try
        {
            agshot::SaveConfig(config.config, configPath);
        }
        catch (const std::exception& e)
        {
            // Not fatal, but settings silently failing to save is exactly the
            // kind of thing that wastes an afternoon.
            auto warning = agshot::DescribeException(e, __FILEW__, __LINE__);
            warning.detail = configPath.wstring() + L"\n\n" + warning.detail;
            agshot::ShowWarning(std::move(warning), L"AGShot could not save its settings file.");
        }
    }
    else if (!config.ok)
    {
        agshot::ShowWarning(
            agshot::Failure{ {}, configPath.wstring() + L"\n" + config.error },
            L"AGShot could not read its settings file, so it is using the defaults.");
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = agshot::kMessageClass;
    AGSHOT_CHECK_WIN32(RegisterClassExW(&wc) != 0);

    // HWND_MESSAGE makes it a message-only window: no screen, no taskbar, no
    // Alt+Tab, and no pixels to keep in sync. It is here for the hotkey and the
    // watcher, nothing else.
    app.hwnd = CreateWindowExW(
        0, agshot::kMessageClass, L"AGShot", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, &app);
    AGSHOT_CHECK_WIN32(app.hwnd != nullptr);

    // The small icon is shared with the tray, so load it once and keep it.
    app.trayIcon.reset(static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_AGSHOT), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0)));
    AGSHOT_CHECK_WIN32(app.trayIcon != nullptr);

    // The overlay is created up front and stays hidden. Building it on the first
    // F1 would put device creation in the way of the capture.
    if (!app.overlay.Create(instance))
    {
        agshot::ShowWarning(
            agshot::DescribeWin32(L"Overlay::Create", GetLastError(), __FILEW__, __LINE__),
            L"AGShot could not create its capture overlay, so the hotkey will do nothing.");
    }

    ApplyConfig(app, app.config);

    const agshot::TrayCallbacks trayCallbacks{
        &app, CaptureFromTray, SettingsFromTray, ReloadFromTray, RestartFromTray, ExitFromTray, FatalFromTray };
    if (const HRESULT trayHr = agshot::StartTrayIcon(instance, app.trayIcon.get(), trayCallbacks);
        FAILED(trayHr))
    {
        // Not fatal: the hotkey still works, but say so rather than leaving the
        // user wondering why the icon never showed up.
        agshot::ShowWarning(
            agshot::DescribeHresult(L"StartTrayIcon", trayHr, __FILEW__, __LINE__),
            L"AGShot could not add its notification-area icon.");
    }

    // From here on the settings file is live. Started after the write above, so
    // that AGShot's own save does not come straight back as a change.
    if (const HRESULT watchHr = app.watcher.Start(configPath, [hwnd = app.hwnd](agshot::WatchEvent event, const std::wstring& detail) {
        OnConfigWatchEvent(hwnd, event, detail);
    }); FAILED(watchHr))
    {
        // Not fatal: the application still runs, but say so rather than leaving
        // the user to wonder why editing the file does nothing.
        agshot::ShowWarning(
            agshot::DescribeHresult(L"ConfigWatcher::Start", watchHr, __FILEW__, __LINE__),
            L"AGShot could not watch its settings file, so changes will only take effect after a restart.");
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

int __stdcall wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
#ifdef AGSHOT_TOOLBAR_PROOF
    {
        (void)instance;
        (void)showCommand;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        const wchar_t* path = argc > 2 ? argv[2] : L"toolbar.bmp";
        const int dpi = argc > 1 ? _wtoi(argv[1]) : 144;
        const bool text = argc > 3 && _wtoi(argv[3]) != 0;
        const float probe = argc > 4 ? static_cast<float>(_wtof(argv[4])) : 0.0f;
        const bool spinner = argc > 5 && _wtoi(argv[5]) != 0;
        try
        {
            agshot::Overlay overlay;
            const bool ok = overlay.ExportToolbarProof(path, dpi, text, probe, spinner);
            LocalFree(argv);
            return ok ? 0 : 1;
        }
        catch (const agshot::Error& error)
        {
            std::wprintf(L"FAIL %ls | %ls\n", error.failure().what.c_str(),
                         error.failure().detail.c_str());
            LocalFree(argv);
            return 1;
        }
    }
#else
    SetLastError(ERROR_SUCCESS);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool isSettingsMode = false;
    if (argv != nullptr)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], agshot::kSettingsArg) == 0)
            {
                isSettingsMode = true;
                break;
            }
        }
        LocalFree(argv);
    }

    if (isSettingsMode)
    {
        wil::unique_handle settingsMutex{
            CreateMutexW(nullptr, FALSE, agshot::kSettingsInstanceMutexName)
        };
        if (!settingsMutex)
        {
            MessageBoxW(nullptr, L"AGShot could not open the settings instance lock.",
                        L"AGShot Settings", MB_OK | MB_ICONERROR);
            return 1;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            FocusExistingSettingsWindow();
            return 0;
        }

        EnableMouseInPointer(true);
        init_apartment(apartment_type::single_threaded);

        try
        {
            return agshot::RunSettingsProcess(instance, showCommand);
        }
        catch (...)
        {
            agshot::ReportCurrentException(__FILEW__, __LINE__);
            return 1;
        }
    }

    wil::unique_handle singleInstance{ CreateMutexW(nullptr, FALSE, agshot::kSingleInstanceMutexName) };
    if (!singleInstance)
    {
        MessageBoxW(nullptr, L"AGShot could not check whether another instance is running.",
                    L"AGShot", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        WakeExistingInstance();
        return 0;
    }

    int result = 1;
    bool restartRequested = false;
    // The one place that catches everything escaping startup or the message
    // loop, so no failure can leave a silently dead process behind.
    try
    {
        result = RunApp(instance, showCommand, restartRequested);
    }
    catch (...)
    {
        agshot::ReportCurrentException(__FILEW__, __LINE__);
    }

    // Release the single-instance mutex before spawning the replacement process
    // so it never collides with this exiting instance.
    singleInstance.reset();
    if (restartRequested)
    {
        if (const HRESULT restartHr = LaunchSelfRestart(); FAILED(restartHr))
        {
            agshot::ShowWarning(
                agshot::DescribeHresult(L"CreateProcessW(restart)", restartHr, __FILEW__, __LINE__),
                L"AGShot could not restart itself.");
        }
    }
    return result;
#endif
}
