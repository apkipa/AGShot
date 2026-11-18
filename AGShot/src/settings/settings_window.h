#pragma once

#include <windows.h>

namespace agshot
{
    // Runs the standalone settings UI process (`AGShot.exe --settings`).
    // Returns the process exit code once the settings window closes.
    int RunSettingsProcess(HINSTANCE instance, int showCommand);
}
