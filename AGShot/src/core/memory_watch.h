#pragma once

#include <windows.h>

namespace agshot
{
    // Monitors Windows LowMemoryResourceNotification on a threadpool wait and
    // posts a caller-supplied message to the owning window once per low-memory
    // transition.
    class SystemMemoryWatch
    {
    public:
        SystemMemoryWatch() = default;
        ~SystemMemoryWatch() noexcept { Destroy(); }

        SystemMemoryWatch(const SystemMemoryWatch&) = delete;
        SystemMemoryWatch& operator=(const SystemMemoryWatch&) = delete;

        void Initialize(HWND window, UINT message) noexcept;
        void Destroy() noexcept;

        void Arm() noexcept;
        void Acknowledge() noexcept;
        bool IsLow() const noexcept;

    private:
        void StopWait(bool closeHandle) noexcept;
        static VOID CALLBACK WaitCallback(PVOID context, BOOLEAN);

        HWND m_window{};
        UINT m_message{};
        HANDLE m_handle{};
        HANDLE m_wait{};
    };
}
