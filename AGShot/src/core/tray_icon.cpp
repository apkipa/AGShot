#include "pch.h"

#include "resource.h"

#include "core/error.h"
#include "core/tray_icon.h"

#include <shellapi.h>

// The icon belongs to a window that is never shown, not to the application's
// main window. A popup menu only dismisses correctly when its owner is
// foreground, and making the main window foreground would drag AGShot in front
// of whatever the user was actually working in.

namespace agshot
{
    namespace
    {
        constexpr UINT kCallback = WM_APP + 1;
        constexpr UINT kIconId = 1;
        constexpr wchar_t kWindowClass[] = L"AGShot.TrayWindow";

        HWND g_window{};
        HICON g_icon{};
        bool g_added{};
        bool g_failed{};
        UINT g_taskbarCreated{};   // registered message; zero until registered
        TrayCallbacks g_callbacks{};

        // Closes the menu on every exit path; the failure branches below would
        // otherwise leak it.
        struct MenuHandle
        {
            HMENU handle{};

            MenuHandle() = default;
            explicit MenuHandle(HMENU value) : handle{ value } {}
            ~MenuHandle()
            {
                if (handle != nullptr)
                {
                    DestroyMenu(handle);
                }
            }

            MenuHandle(const MenuHandle&) = delete;
            MenuHandle& operator=(const MenuHandle&) = delete;
        };

        bool AddIcon() noexcept
        {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = g_window;
            data.uID = kIconId;
            data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
            data.uCallbackMessage = kCallback;
            data.hIcon = g_icon;
            wcscpy_s(data.szTip, L"AGShot");

            if (!Shell_NotifyIconW(NIM_ADD, &data))
            {
                return false;
            }

            // Opt into the current protocol: the shell then positions the menu
            // correctly and names the click events instead of sending raw WM_*.
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);

            g_added = true;
            return true;
        }

        void RemoveIcon() noexcept
        {
            // Reached from two shutdown paths; the flag keeps the second from
            // addressing a shell entry that is already gone.
            if (!g_added || g_window == nullptr)
            {
                return;
            }

            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = g_window;
            data.uID = kIconId;
            Shell_NotifyIconW(NIM_DELETE, &data);

            g_added = false;
        }

        void ShowMenu() noexcept
        {
            try
            {
                MenuHandle menu{ CreatePopupMenu() };
                if (menu.handle == nullptr)
                {
                    ShowWarning(
                        DescribeWin32(L"CreatePopupMenu", GetLastError(), __FILEW__, __LINE__),
                        L"AGShot could not open its notification-area menu.");
                    return;
                }

                AGSHOT_CHECK_WIN32(AppendMenuW(menu.handle, MF_STRING, IDM_SETTINGS, L"Settings...") != 0);
                AGSHOT_CHECK_WIN32(AppendMenuW(menu.handle, MF_STRING, IDM_RELOAD, L"Reload settings") != 0);
                AGSHOT_CHECK_WIN32(AppendMenuW(menu.handle, MF_SEPARATOR, 0, nullptr) != 0);
                AGSHOT_CHECK_WIN32(AppendMenuW(menu.handle, MF_STRING, IDM_EXIT, L"Exit") != 0);

                // Required so the menu dismisses when the user clicks elsewhere.
                // Harmless here: the owner window has nothing to show, so nothing
                // moves on screen.
                SetForegroundWindow(g_window);

                POINT cursor{};
                AGSHOT_CHECK_WIN32(GetCursorPos(&cursor));

                const int command = TrackPopupMenu(
                    menu.handle,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                    cursor.x,
                    cursor.y,
                    0,
                    g_window,
                    nullptr);

                // Documented workaround: the menu can stay on screen until the
                // owning window processes one more message.
                PostMessageW(g_window, WM_NULL, 0, 0);

                // TrackPopupMenu returns the chosen id rather than sending a
                // command, so the dispatch is a switch rather than WM_COMMAND.
                switch (command)
                {
                case IDM_SETTINGS:
                    if (g_callbacks.settings != nullptr)
                    {
                        g_callbacks.settings();
                    }
                    break;

                case IDM_RELOAD:
                    if (g_callbacks.reload != nullptr)
                    {
                        g_callbacks.reload();
                    }
                    break;

                case IDM_EXIT:
                    if (g_callbacks.exit != nullptr)
                    {
                        g_callbacks.exit();
                    }
                    break;

                default:
                    break;
                }
            }
            catch (...)
            {
                // A menu that will not open is an inconvenience. Killing the whole
                // application over it would be far worse than the fault itself.
                ReportCurrentException(__FILEW__, __LINE__);
            }
        }

        LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            // Explorer broadcasts this after it restarts, and every tray icon that
            // existed before the restart went with it.
            if (g_taskbarCreated != 0 && message == g_taskbarCreated)
            {
                g_added = false;
                AddIcon();
                return 0;
            }

            switch (message)
            {
            case kCallback:
                switch (LOWORD(lparam))
                {
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    ShowMenu();
                    return 0;

                // Version 4 of the protocol names the click events, but the legacy
                // messages still arrive, so accept both.
                case NIN_SELECT:
                case WM_LBUTTONUP:
                    if (g_callbacks.activate != nullptr)
                    {
                        g_callbacks.activate();
                    }
                    return 0;

                default:
                    return 0;
                }

            case WM_DESTROY:
                RemoveIcon();
                return 0;
            }

            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            if (g_failed)
            {
                return DefWindowProcW(hwnd, message, wparam, lparam);
            }

            try
            {
                return HandleMessage(hwnd, message, wparam, lparam);
            }
            catch (...)
            {
                // Report once only: reporting pumps messages, so a repeated fault
                // would otherwise stack up one dialog per message.
                if (!g_failed)
                {
                    g_failed = true;
                    ReportCurrentException(__FILEW__, __LINE__);
                    if (g_callbacks.fatal != nullptr)
                    {
                        g_callbacks.fatal();
                    }
                }
                return 0;
            }
        }
    }

    bool StartTrayIcon(HINSTANCE instance, HICON icon, const TrayCallbacks& callbacks)
    {
        g_icon = icon;
        g_callbacks = callbacks;
        g_failed = false;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClass;
        if (RegisterClassExW(&wc) == 0)
        {
            return false;
        }

        // Never shown. It exists to receive the shell's notifications and to own
        // the popup menu.
        g_window = CreateWindowExW(
            0, kWindowClass, L"AGShot", 0, 0, 0, 0, 0,
            nullptr, nullptr, instance, nullptr);
        if (g_window == nullptr)
        {
            return false;
        }

        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        return AddIcon();
    }

    void StopTrayIcon() noexcept
    {
        RemoveIcon();
        if (g_window != nullptr)
        {
            DestroyWindow(g_window);
            g_window = nullptr;
        }
        g_icon = nullptr;
        g_callbacks = {};
    }
}
