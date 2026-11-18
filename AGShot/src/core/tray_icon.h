#pragma once

#include <windows.h>

namespace agshot
{
    // What the tray icon asks the application to do. Function pointers rather
    // than std::function: there is one tray icon and it never changes.
    struct TrayCallbacks
    {
        void* context = nullptr;
        void (*activate)(void* context) = nullptr; // left click: surface the main window
        void (*settings)(void* context) = nullptr; // Settings chosen from the menu
        void (*reload)(void* context) = nullptr;   // Reload settings chosen from the menu
        void (*restart)(void* context) = nullptr;  // Restart chosen from the menu
        void (*exit)(void* context) = nullptr;     // Exit chosen from the menu
        void (*fatal)(void* context) = nullptr;    // unrecoverable failure in the tray window
    };

    // Creates the hidden owner window and installs the icon. "icon" is borrowed
    // and must outlive StopTrayIcon(). Returns false if the shell refused the
    // icon or the window could not be created, in which case the caller decides
    // what to say about it; StopTrayIcon() is safe to call either way.
    bool StartTrayIcon(HINSTANCE instance, HICON icon, const TrayCallbacks& callbacks);

    // Idempotent. Removes the icon and destroys the hidden window.
    void StopTrayIcon() noexcept;
}
