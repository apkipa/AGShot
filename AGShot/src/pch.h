#pragma once

// Windows SDK / COM / Shell / UI
#include <windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <windowsx.h>
#include <unknwn.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <compressapi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uiautomation.h>

// DirectX / Direct2D / DirectWrite / DirectComposition
#include <d2d1_1.h>
#include <d2d1_3.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_4.h>

// WIL / WebView2 / C++/WinRT
#include <wil/resource.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>
#include <WebView2.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
// XAML Islands (unused currently; keep #undef GetCurrentTime above if re-enabled)
// #include <winrt/Windows.System.h>
// #include <winrt/Windows.UI.Xaml.h>
// #include <winrt/Windows.UI.Xaml.Controls.h>
// #include <winrt/Windows.UI.Xaml.Hosting.h>
// #include <winrt/Windows.UI.Xaml.Media.h>
// #include <Windows.UI.Xaml.Hosting.DesktopWindowXamlSource.h>

// C++ Standard Library
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
