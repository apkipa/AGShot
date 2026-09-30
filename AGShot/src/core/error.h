#pragma once

#include <exception>
#include <string>
#include <string_view>

// Reporting has one rule: whatever fails, the user must be able to see which
// call it was, what the system says about it, and where in the source it
// happened. A bare HRESULT is not a report.

namespace agshot
{
    struct Failure
    {
        std::wstring what;      // one line: the call that failed
        std::wstring detail;    // code, system message, source location
    };

    enum class FailureKind { Error, Warning };

    // Thrown by the AGSHOT_CHECK_* macros below.
    class Error : public std::exception
    {
    public:
        explicit Error(Failure failure);

        const Failure& failure() const noexcept { return m_failure; }
        const char* what() const noexcept override { return m_narrow.c_str(); }

    private:
        Failure m_failure;
        std::string m_narrow;
    };

    // "context" is normally the stringised failing expression.
    Failure DescribeWin32(std::wstring_view context, unsigned long code,
                          const wchar_t* file, int line);
    Failure DescribeHresult(std::wstring_view context, long hr,
                            const wchar_t* file, int line,
                            std::wstring_view extra = {});
    Failure DescribeException(const std::exception& error, const wchar_t* file, int line);
    Failure DescribeUnknown(const wchar_t* file, int line);

    // Extra lines appended to every report. Set once at startup.
    void SetDiagnosticContext(std::wstring text);

    // Blocks until dismissed, offering a copy of the full text. noexcept: it is
    // called on the way out of a failure and must not throw on top of one.
    void ShowFailure(const Failure& failure, FailureKind kind = FailureKind::Error) noexcept;

    // ShowFailure with a caller-chosen headline, always as a warning. For
    // failures that are inconvenient rather than fatal.
    void ShowWarning(Failure failure, std::wstring_view headline) noexcept;

    // Reports the exception currently being handled, dispatching on its type so
    // the detail is as specific as possible.
    //
    // Precondition: called from inside a catch block. The bare rethrow below
    // requires a live exception, and calling this from anywhere else is a
    // programming error that terminates the process - deliberately, because a
    // mistake in the code is not something to paper over with a dialog.
    void ReportCurrentException(const wchar_t* file, int line) noexcept;
}

// Win32 BOOL-style results. Captures the expression text and the source line so
// the report can say "RegisterClassExW(&wc) != 0 failed" instead of just
// "0x0000057E".
#define AGSHOT_CHECK_WIN32(expr)                                                     \
    do {                                                                             \
        if (!(expr)) {                                                               \
            throw ::agshot::Error{ ::agshot::DescribeWin32(                           \
                L"" #expr, static_cast<unsigned long>(::GetLastError()),              \
                __FILEW__, __LINE__) };                                              \
        }                                                                            \
    } while (false)

// HRESULT-returning calls, including every C++/WinRT one.
#define AGSHOT_CHECK_HR(expr)                                                        \
    do {                                                                             \
        const HRESULT agshot_hr_ = (expr);                                           \
        if (FAILED(agshot_hr_)) {                                                    \
            throw ::agshot::Error{ ::agshot::DescribeHresult(                         \
                L"" #expr, agshot_hr_, __FILEW__, __LINE__) };                       \
        }                                                                            \
    } while (false)
