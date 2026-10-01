#include "pch.h"
#include "resource.h"
#include "core/config.h"
#include "core/config_watcher.h"
#include "core/error.h"
#include "core/tray_icon.h"
#include "hud/overlay.h"

#include <memory>
#include <new>

using namespace winrt;

namespace
{
    // The application's own window, and the only one it creates that is never
    // seen. It exists to receive the hotkey and the watcher's notifications:
    // both need a window to be posted to, and neither needs a screen.
    constexpr wchar_t kMessageClass[] = L"AGShot.MessageWindow";

    constexpr UINT kConfigChangedMessage = WM_APP + 1;
    constexpr UINT kConfigWatchLostMessage = WM_APP + 2;

    // Names the capture combination in WM_HOTKEY. AGShot registers exactly one,
    // so the number only has to be distinct within this window.
    constexpr int kScreenshotHotkeyId = 1;

    HWND g_hwnd{};
    agshot::Overlay g_overlay;

    // The small icon is shared with the tray icon and must outlive it.
    HICON g_trayIcon{};

    // The settings currently in force, and where they came from. Kept so that a
    // reload can tell a real change from a rewrite of the same values.
    agshot::Config g_config;
    std::filesystem::path g_configPath;

    // The last settings problem already reported. A file that stays broken while
    // being saved would otherwise stack up one dialog per save.
    std::wstring g_reportedConfigError;

    // Whether the capture combination is currently registered. False also covers
    // "another application already had it", which is deliberately quiet.
    bool g_hotkeyRegistered{};

    // Set once a failure has been reported, so that a repeated fault cannot stack
    // up one dialog per message.
    bool g_fatal{};

    // Defined further down, next to the settings they act on.
    void ShowSettings();
    void ReloadConfig(bool force);

    // What the tray icon asks of the application. The tray itself, including its
    // hidden owner window and its menu, lives in core/tray_icon.cpp.
    void CaptureFromTray()
    {
        g_overlay.Begin();
    }

    void ExitFromTray()
    {
        if (g_hwnd != nullptr)
        {
            DestroyWindow(g_hwnd);
        }
    }

    void FatalFromTray()
    {
        g_fatal = true;
        PostQuitMessage(1);
    }

    void SettingsFromTray()
    {
        ShowSettings();
    }

    void ReloadFromTray()
    {
        // Forced: the point of the menu item is to re-apply settings that are
        // already loaded, so that anything which failed the first time - a hotkey
        // another application was holding, say - gets another go.
        ReloadConfig(true);
    }

    // Owns the capture combination for as long as the window lives. Called again
    // whenever the settings change, so a new combination replaces the old one.
    void ApplyHotkey(const agshot::Hotkey& hotkey)
    {
        if (g_hotkeyRegistered)
        {
            UnregisterHotKey(g_hwnd, kScreenshotHotkeyId);
            g_hotkeyRegistered = false;
        }

        if (!hotkey.valid() || g_hwnd == nullptr)
        {
            return;
        }

        // MOD_NOREPEAT matters: without it, holding the key down fires the hotkey
        // again and again, which for a capture is not what anyone means by one
        // press. It is a registration detail, not part of what the file stores.
        if (RegisterHotKey(g_hwnd, kScreenshotHotkeyId, hotkey.modifiers | MOD_NOREPEAT, hotkey.key))
        {
            g_hotkeyRegistered = true;
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
    void ApplyConfig(const agshot::Config& config)
    {
        ApplyHotkey(config.screenshotHotkey);
    }

    // Stands in for the settings window, which will edit what the file holds.
    void ShowSettings()
    {
        // Unowned on purpose: the application's only window is a message-only one,
        // which is no kind of owner for a dialog.
        const std::wstring message =
            L"The settings window is not built yet.\n\nFor now the settings are the file:\n"
            + g_configPath.wstring();

        MessageBoxW(nullptr, message.c_str(), L"AGShot", MB_OK | MB_ICONINFORMATION);
    }

    // Reads the settings file again and applies whatever changed. Runs on the UI
    // thread, from the message the watcher posts.
    //
    // "force" applies the settings even when the values are unchanged, which is
    // what the tray's Reload item is for.
    void ReloadConfig(bool force)
    {
        const auto load = agshot::LoadConfig(g_configPath);

        if (!load.ok)
        {
            // Once per distinct problem: an editor that keeps saving a file that
            // stays broken must not stack up one dialog per save.
            if (load.error != g_reportedConfigError)
            {
                g_reportedConfigError = load.error;
                agshot::ShowWarning(
                    agshot::Failure{ {}, g_configPath.wstring() + L"\n" + load.error },
                    L"The settings file changed, but AGShot could not read it, so the previous settings are still in use.");
            }
            return;
        }

        g_reportedConfigError.clear();

        // Rewriting the file without changing anything is not a change, and a
        // deleted file puts the defaults back in force.
        if (!force && load.config == g_config)
        {
            return;
        }

        g_config = load.config;
        ApplyConfig(g_config);
    }

    // Runs on the watcher thread. Posting is the whole job: applying a setting
    // touches windows, and the message queue is the only synchronisation this
    // application needs.
    void OnConfigWatchEvent(agshot::WatchEvent event, const std::wstring& detail)
    {
        if (g_hwnd == nullptr)
        {
            return;
        }

        if (event == agshot::WatchEvent::Changed)
        {
            PostMessageW(g_hwnd, kConfigChangedMessage, 0, 0);
            return;
        }

        // PostMessageW cannot carry a string, so the reason travels as a heap
        // pointer that the handler takes back. Saying nothing would leave the
        // user editing a file that nothing is listening to.
        auto* text = new (std::nothrow) std::wstring{ detail };
        if (text != nullptr
            && !PostMessageW(g_hwnd, kConfigWatchLostMessage, 0, reinterpret_cast<LPARAM>(text)))
        {
            delete text;
        }
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case kConfigChangedMessage:
            ReloadConfig(false);
            return 0;

        case kConfigWatchLostMessage:
        {
            // The watcher handed the reason over as a heap string; take it back.
            const std::unique_ptr<std::wstring> detail{ reinterpret_cast<std::wstring*>(lparam) };
            agshot::ShowWarning(
                agshot::Failure{ {}, g_configPath.wstring() + L"\n"
                                     + (detail ? *detail : std::wstring{}) },
                L"AGShot is no longer watching its settings file, so further changes will need a restart.");
            return 0;
        }

        case WM_HOTKEY:
            // RegisterHotKey posts to the window it was registered for, so the id
            // is the only thing worth checking.
            if (static_cast<int>(wparam) == kScreenshotHotkeyId)
            {
                g_overlay.Begin();
            }
            return 0;

        case WM_DESTROY:
            // The window is going away; hand the combination back rather than
            // leaving it to the process teardown.
            if (g_hotkeyRegistered)
            {
                UnregisterHotKey(hwnd, kScreenshotHotkeyId);
                g_hotkeyRegistered = false;
            }
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
        if (g_fatal)
        {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        try
        {
            return HandleMessage(hwnd, message, wparam, lparam);
        }
        catch (...)
        {
            if (!g_fatal)
            {
                g_fatal = true;
                agshot::ReportCurrentException(__FILEW__, __LINE__);
                PostQuitMessage(1);
            }
            return 0;
        }
    }
}

int RunApp(HINSTANCE instance, int showCommand)
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
    g_configPath = configPath;
    g_config = config.config;
    // The startup warning below covers this problem already, so do not repeat it
    // the first time the file is touched.
    g_reportedConfigError = config.ok ? std::wstring{} : config.error;

    // Shown at the bottom of every report, so it is obvious which file was in play.
    agshot::SetDiagnosticContext(L"Config: " + configPath.wstring());

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
    g_hwnd = CreateWindowExW(
        0, kMessageClass, L"AGShot", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, nullptr);
    AGSHOT_CHECK_WIN32(g_hwnd != nullptr);

    // The small icon is shared with the tray, so load it once and keep it.
    g_trayIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_AGSHOT), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    AGSHOT_CHECK_WIN32(g_trayIcon != nullptr);

    // The overlay is created up front and stays hidden. Building it on the first
    // F1 would put device creation in the way of the capture.
    if (!g_overlay.Create(instance))
    {
        agshot::ShowWarning(
            agshot::DescribeWin32(L"Overlay::Create", GetLastError(), __FILEW__, __LINE__),
            L"AGShot could not create its capture overlay, so the hotkey will do nothing.");
    }

    ApplyConfig(g_config);

    const agshot::TrayCallbacks trayCallbacks{
        CaptureFromTray, SettingsFromTray, ReloadFromTray, ExitFromTray, FatalFromTray };
    if (!agshot::StartTrayIcon(instance, g_trayIcon, trayCallbacks))
    {
        // Not fatal: the hotkey still works, but say so rather than leaving the
        // user wondering why the icon never showed up.
        agshot::ShowWarning(
            agshot::DescribeWin32(L"StartTrayIcon", GetLastError(), __FILEW__, __LINE__),
            L"AGShot could not add its notification-area icon.");
    }

    // From here on the settings file is live. Started after the write above, so
    // that AGShot's own save does not come straight back as a change.
    agshot::ConfigWatcher watcher;
    if (!watcher.Start(configPath, OnConfigWatchEvent))
    {
        // Not fatal: the application still runs, but say so rather than leaving
        // the user to wonder why editing the file does nothing.
        agshot::ShowWarning(
            agshot::DescribeWin32(L"ConfigWatcher::Start", GetLastError(), __FILEW__, __LINE__),
            L"AGShot could not watch its settings file, so changes will only take effect after a restart.");
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // The loop can also end through the fatal path, where WM_DESTROY never ran.
    // The watcher goes first: it posts to a window that has just been destroyed.
    watcher.Stop();
    agshot::StopTrayIcon();
    g_overlay.Destroy();

    if (g_trayIcon != nullptr)
    {
        DestroyIcon(g_trayIcon);
        g_trayIcon = nullptr;
    }

    return static_cast<int>(msg.wParam);
}

int __stdcall wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand)
{
    // The one place that catches everything escaping startup or the message
    // loop, so no failure can leave a silently dead process behind.
    try
    {
        return RunApp(instance, showCommand);
    }
    catch (...)
    {
        agshot::ReportCurrentException(__FILEW__, __LINE__);
        return 1;
    }
}
