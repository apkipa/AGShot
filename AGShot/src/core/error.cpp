#include "pch.h"

#include "core/error.h"
#include "core/text.h"

#include <iomanip>
#include <sstream>

#include <commctrl.h>

namespace agshot
{
    namespace
    {
        std::wstring g_context;

        // RtlGetVersion is the only version query the compatibility manifest
        // does not shim. Declared locally to avoid dragging in <winternl.h>.
        struct OsVersionInfo
        {
            ULONG size;
            ULONG major;
            ULONG minor;
            ULONG build;
            ULONG platform;
            wchar_t servicePack[128];
        };

        std::wstring Trim(std::wstring text)
        {
            while (!text.empty() &&
                   (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            {
                text.pop_back();
            }
            return text;
        }

        std::wstring Hex(unsigned long value)
        {
            std::wostringstream out;
            out << L"0x" << std::hex << std::uppercase << std::setw(8)
                << std::setfill(L'0') << value;
            return out.str();
        }

        std::wstring SystemMessage(DWORD code)
        {
            LPWSTR raw = nullptr;
            const DWORD length = FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                code,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                reinterpret_cast<LPWSTR>(&raw),
                0,
                nullptr);

            std::wstring text;
            if (length != 0 && raw != nullptr)
            {
                text.assign(raw, length);
                LocalFree(raw);
            }
            return Trim(std::move(text));
        }

        std::wstring Location(const wchar_t* file, int line)
        {
            if (file == nullptr)
            {
                return {};
            }
            std::wostringstream out;
            out << L"at " << file << L":" << line;
            return out.str();
        }

        // Facts that make a report actionable once it is pasted into a bug
        // report: which binary, on what OS, at what DPI.
        std::wstring EnvironmentBlock()
        {
            std::wostringstream out;

            wchar_t path[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
            out << L"Executable: " << std::wstring_view{ path, length } << L"\n";

            out << L"Architecture: " << (sizeof(void*) == 8 ? L"x64" : L"x86") << L"\n";

            OsVersionInfo version{};
            version.size = sizeof(version);
            if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
            {
                using RtlGetVersionFn = LONG(WINAPI*)(OsVersionInfo*);
                auto fn = reinterpret_cast<RtlGetVersionFn>(
                    reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
                if (fn != nullptr && fn(&version) == 0)
                {
                    out << L"Windows: " << version.major << L'.' << version.minor
                        << L" build " << version.build << L"\n";
                }
            }

            out << L"System DPI: " << GetDpiForSystem();
            return out.str();
        }

        bool CopyToClipboard(std::wstring_view text) noexcept
        {
            if (text.empty() || !OpenClipboard(nullptr))
            {
                return false;
            }

            bool ok = false;
            if (EmptyClipboard())
            {
                const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
                {
                    if (void* target = GlobalLock(memory))
                    {
                        memcpy(target, text.data(), bytes);
                        GlobalUnlock(memory);
                        if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr)
                        {
                            ok = true;
                            memory = nullptr;   // the clipboard owns it now
                        }
                    }
                    if (memory != nullptr)
                    {
                        GlobalFree(memory);
                    }
                }
            }
            CloseClipboard();
            return ok;
        }
    }

    Error::Error(Failure failure)
        : m_failure{ std::move(failure) }
        , m_narrow{ ToUtf8(m_failure.what.empty() ? m_failure.detail : m_failure.what) }
    {
    }

    Failure DescribeWin32(std::wstring_view context, unsigned long code,
                          const wchar_t* file, int line)
    {
        Failure failure;
        failure.what = std::wstring{ context } + L"  failed";

        std::wostringstream detail;
        detail << L"Win32 error " << code << L"  (" << Hex(code) << L")";
        if (const std::wstring message = SystemMessage(static_cast<DWORD>(code)); !message.empty())
        {
            detail << L"\n" << message;
        }
        if (const std::wstring location = Location(file, line); !location.empty())
        {
            detail << L"\n\n" << location;
        }

        failure.detail = detail.str();
        return failure;
    }

    Failure DescribeHresult(std::wstring_view context, long hr,
                            const wchar_t* file, int line, std::wstring_view extra)
    {
        Failure failure;
        failure.what = std::wstring{ context } + L"  failed";

        std::wostringstream detail;
        detail << L"HRESULT " << Hex(static_cast<unsigned long>(hr));
        if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
        {
            detail << L"  (Win32 " << HRESULT_CODE(hr) << L")";
        }
        detail << L"\n";

        // WinRT usually knows more than the system message table does.
        if (!extra.empty())
        {
            detail << extra << L"\n";
        }
        else if (const std::wstring message = SystemMessage(static_cast<DWORD>(hr)); !message.empty())
        {
            detail << message << L"\n";
        }

        if (const std::wstring location = Location(file, line); !location.empty())
        {
            detail << L"\n" << location;
        }

        failure.detail = detail.str();
        return failure;
    }

    Failure DescribeException(const std::exception& error, const wchar_t* file, int line)
    {
        Failure failure;
        failure.what = L"An unexpected C++ exception reached the top of the stack.";

        std::wostringstream detail;
        detail << L"what(): " << FromUtf8(error.what() == nullptr ? "" : error.what());
        if (const std::wstring location = Location(file, line); !location.empty())
        {
            detail << L"\n\n" << location;
        }

        failure.detail = detail.str();
        return failure;
    }

    Failure DescribeUnknown(const wchar_t* file, int line)
    {
        Failure failure;
        failure.what = L"Something failed that AGShot cannot describe.";

        std::wostringstream detail;
        detail << L"A non-C++ exception was thrown, so there is no message to show.";
        if (const std::wstring location = Location(file, line); !location.empty())
        {
            detail << L"\n\n" << location;
        }

        failure.detail = detail.str();
        return failure;
    }

    void SetDiagnosticContext(std::wstring text)
    {
        g_context = std::move(text);
    }

    void ShowWarning(Failure failure, std::wstring_view headline) noexcept
    {
        failure.what = std::wstring{ headline };
        ShowFailure(failure, FailureKind::Warning);
    }

    void ReportCurrentException(const wchar_t* file, int line) noexcept
    {
        try
        {
            throw;
        }
        catch (const Error& e)
        {
            ShowFailure(e.failure());
        }
        catch (const winrt::hresult_error& e)
        {
            ShowFailure(DescribeHresult(
                L"a Windows Runtime call", e.code(), file, line, e.message().c_str()));
        }
        catch (const std::exception& e)
        {
            ShowFailure(DescribeException(e, file, line));
        }
        catch (...)
        {
            ShowFailure(DescribeUnknown(file, line));
        }
    }

    void ShowFailure(const Failure& failure, FailureKind kind) noexcept
    {
        try
        {
            std::wstring content = failure.detail;
            if (const std::wstring environment = EnvironmentBlock(); !environment.empty())
            {
                content += L"\n\n" + environment;
            }
            if (!g_context.empty())
            {
                content += L"\n" + g_context;
            }

            enum : int { kCopyDetails = 1001 };
            const TASKDIALOG_BUTTON buttons[] = {
                { kCopyDetails, L"Copy details" },
                { IDCANCEL, L"Close" },
            };

            TASKDIALOGCONFIG config{};
            config.cbSize = sizeof(config);
            config.dwFlags = TDF_SIZE_TO_CONTENT | TDF_ALLOW_DIALOG_CANCELLATION;
            config.pszWindowTitle = L"AGShot";
            config.pszMainIcon = (kind == FailureKind::Error) ? TD_ERROR_ICON : TD_WARNING_ICON;
            config.pszMainInstruction = failure.what.c_str();
            config.pszContent = content.c_str();
            config.cButtons = ARRAYSIZE(buttons);
            config.pButtons = buttons;
            config.nDefaultButton = kCopyDetails;

            int pressed = 0;
            const HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
            if (FAILED(hr))
            {
                // Older or hobbled comctl32: a plain message box still tells the
                // user something went wrong, which beats exiting silently.
                const std::wstring plain = failure.what + L"\n\n" + content;
                MessageBoxW(nullptr, plain.c_str(), L"AGShot", MB_ICONERROR | MB_OK);
                return;
            }
            if (pressed == kCopyDetails)
            {
                CopyToClipboard(failure.what + L"\n\n" + content);
            }
        }
        catch (...)
        {
            // The reporter itself must not be the thing that takes the process
            // down, but going silent would be worse: a fatal error would become
            // an unexplained exit. MessageBoxW is used here because it is the
            // one call in this function that cannot throw or allocate.
            MessageBoxW(nullptr, failure.what.c_str(), L"AGShot", MB_ICONERROR | MB_OK);
        }
    }
}
