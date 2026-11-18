#include "pch.h"
#include "resource.h"
#include "core/config.h"
#include "core/ocr_manifest.h"
#include "core/ocr_runtime.h"
#include "core/config_watcher.h"
#include "core/error.h"
#include "core/tray_icon.h"
#include "hud/overlay.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <new>

#include <shellapi.h>
#include <wil/resource.h>

using namespace winrt;

namespace
{
    // The application's own window, and the only one it creates that is never
    // seen. It exists to receive the hotkey and the watcher's notifications:
    // both need a window to be posted to, and neither needs a screen.
    constexpr wchar_t kMessageClass[] = L"AGShot.MessageWindow";

    // Models are kept external under models/ocr/. Search the user data
    // directory first (%APPDATA%\AGShot, or beside the exe in portable mode),
    // then next to the executable, then the working directory. Prefer a pack
    // whose pack_format matches this build; fall back to any present pack so
    // an unsupported version still surfaces its exact manifest diagnostic.
    std::filesystem::path FindLocalOcrModels(const std::filesystem::path& dataDirectory)
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
        addCandidate(agshot::ExecutableDirectory());
        std::error_code cwdEc;
        if (const auto cwd = std::filesystem::current_path(cwdEc); !cwdEc)
        {
            addCandidate(cwd);
        }

        std::filesystem::path fallback;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto found = agshot::FindOcrModelDirectory(candidates[i]);
            if (found.empty())
            {
                continue;
            }
            if (agshot::HasSupportedOcrPackFormat(found))
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

    constexpr UINT kConfigChangedMessage = WM_APP + 1;
    constexpr UINT kConfigWatchLostMessage = WM_APP + 2;
    constexpr UINT kWakeExistingMessage = WM_APP + 3;
    constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\AGShot.SingleInstance";

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
        bool fatal{};
        bool* restartRequested{};
        wil::unique_hicon trayIcon;
        HWND hwnd{};
        agshot::Overlay overlay;
        agshot::ConfigWatcher watcher;

        ~AppHost()
        {
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
    };

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
    //
    // [window] is deliberately not applied: it sized the main window, and there
    // is no main window at the moment. The keys stay in the file so that they are
    // still there when it comes back.
    void ApplyConfig(AppHost& app, const agshot::Config& config)
    {
        ApplyHotkey(app, config.screenshotHotkey);
        app.overlay.SetDebugStats(config.debugShowStats);
    }

    // Opens the settings file in whatever the user has associated with it.
    //
    // This stands in for the settings window, which will edit these values
    // itself. Until then the file is the settings, so the useful thing this item
    // can do is put the file in front of the user rather than describe it.
    void ShowSettings(AppHost& app)
    {
        // The file is written at startup when it is missing, so this is only
        // reachable if something removed it while the application was running.
        std::error_code ec;
        if (!std::filesystem::exists(app.configPath, ec))
        {
            try
            {
                agshot::SaveConfig(app.config, app.configPath);
            }
            catch (const std::exception& error)
            {
                agshot::ShowWarning(
                    agshot::DescribeException(error, __FILEW__, __LINE__),
                    L"The settings file could not be created.");
                return;
            }
        }

        // ShellExecuteW rather than a hard-coded editor: which program opens a
        // .toml file is the user's choice, not this application's.
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            nullptr, L"open", app.configPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL));

        // ShellExecuteW reports success by returning a value above 32; anything
        // at or below that is an error code, not a handle.
        if (result <= 32)
        {
            agshot::ShowWarning(
                agshot::DescribeWin32(L"ShellExecuteW(open config)", static_cast<unsigned long>(result),
                                      __FILEW__, __LINE__),
                L"The settings file could not be opened.");
        }
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
            PostMessageW(hwnd, kConfigChangedMessage, 0, 0);
            return;
        }

        // PostMessageW cannot carry a string, so the reason travels as a heap
        // pointer that the handler takes back. Saying nothing would leave the
        // user editing a file that nothing is listening to.
        auto* text = new (std::nothrow) std::wstring{ detail };
        if (text != nullptr
            && !PostMessageW(hwnd, kConfigWatchLostMessage, 0, reinterpret_cast<LPARAM>(text)))
        {
            delete text;
        }
    }

    LRESULT HandleMessage(AppHost& app, HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case kWakeExistingMessage:
            app.overlay.Begin();
            return 0;

        case kConfigChangedMessage:
            ReloadConfig(app, false);
            return 0;

        case kConfigWatchLostMessage:
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
            const HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr, kMessageClass, nullptr);
            if (hwnd != nullptr)
            {
                DWORD processId{};
                GetWindowThreadProcessId(hwnd, &processId);
                if (processId != 0)
                {
                    AllowSetForegroundWindow(processId);
                }
                if (PostMessageW(hwnd, kWakeExistingMessage, 0, 0))
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
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD written = GetModuleFileNameW(
                nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (written == 0)
            {
                return agshot::LastErrorHresult();
            }
            if (written < path.size())
            {
                path.resize(written);
                break;
            }
            path.resize(path.size() * 2);
        }

        std::wstring commandLine = L"\"" + path + L"\"";
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
    // startup, but never attempt a model load on the UI thread.
    const auto dataDirectory = configPath.parent_path();
    const auto ocrModels = FindLocalOcrModels(dataDirectory);
    if (ocrModels.empty()) {
        const auto appDataHint = dataDirectory / L"models" / L"ocr";
        const std::wstring detail = (dataDirectory == agshot::ExecutableDirectory())
            ? L"Expected a local models\\ocr\\ directory beside AGShot."
            : L"Expected models\\ocr\\ beside AGShot or in " + appDataHint.wstring() + L".";
        agshot::ShowWarning(
            agshot::Failure{ {}, detail },
            L"AGShot OCR models are unavailable.");
    }
    const auto ocrCache = dataDirectory / L"cache";
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
    wc.lpszClassName = kMessageClass;
    AGSHOT_CHECK_WIN32(RegisterClassExW(&wc) != 0);

    // HWND_MESSAGE makes it a message-only window: no screen, no taskbar, no
    // Alt+Tab, and no pixels to keep in sync. It is here for the hotkey and the
    // watcher, nothing else.
    app.hwnd = CreateWindowExW(
        0, kMessageClass, L"AGShot", 0, 0, 0, 0, 0,
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
    wil::unique_handle singleInstance{ CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName) };
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
