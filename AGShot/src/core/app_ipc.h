#pragma once

#include <windows.h>

namespace agshot
{
    inline constexpr wchar_t kMessageClass[] = L"AGShot.MessageWindow";
    inline constexpr wchar_t kSettingsWindowClass[] = L"AGShot.SettingsWindow";

    inline constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\AGShot.SingleInstance";
    inline constexpr wchar_t kSettingsInstanceMutexName[] = L"Local\\AGShot.Settings";
    inline constexpr wchar_t kSettingsArg[] = L"--settings";

    // Cross-process / tray messages on AGShot.MessageWindow.
    inline constexpr UINT kConfigChangedMessage = WM_APP + 1;
    inline constexpr UINT kConfigWatchLostMessage = WM_APP + 2;
    inline constexpr UINT kWakeExistingMessage = WM_APP + 3;
    inline constexpr UINT kPauseHotkeyMessage = WM_APP + 4;
    inline constexpr UINT kResumeHotkeyMessage = WM_APP + 5;
    inline constexpr UINT kQueryStatusMessage = WM_APP + 6;
    inline constexpr UINT kTrimOcrMemoryMessage = WM_APP + 7;
    inline constexpr UINT kReloadConfigMessage = WM_APP + 8;

    // Bitmask returned by kQueryStatusMessage.
    inline constexpr LRESULT kStatusOcrAvailable = 1 << 0;
    inline constexpr LRESULT kStatusOcrLoaded = 1 << 1;
    inline constexpr LRESULT kStatusOcrError = 1 << 2;
    inline constexpr LRESULT kStatusHotkeyRegistered = 1 << 3;
    inline constexpr LRESULT kStatusHotkeyPaused = 1 << 4;
}
