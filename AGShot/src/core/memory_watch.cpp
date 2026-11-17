#include "pch.h"

#include "core/memory_watch.h"

namespace agshot
{
    void SystemMemoryWatch::Initialize(HWND window, UINT message) noexcept
    {
        StopWait(false);
        m_window = window;
        m_message = message;
        if (m_handle == nullptr)
        {
            m_handle = CreateMemoryResourceNotification(LowMemoryResourceNotification);
        }
    }

    void SystemMemoryWatch::Destroy() noexcept
    {
        StopWait(true);
        m_window = nullptr;
        m_message = 0;
    }

    void SystemMemoryWatch::Arm() noexcept
    {
        if (m_window == nullptr || m_message == 0 || m_handle == nullptr || m_wait != nullptr)
        {
            return;
        }

        RegisterWaitForSingleObject(
            &m_wait,
            m_handle,
            &SystemMemoryWatch::WaitCallback,
            this,
            INFINITE,
            WT_EXECUTEONLYONCE);
    }

    void SystemMemoryWatch::Acknowledge() noexcept
    {
        StopWait(false);
    }

    bool SystemMemoryWatch::IsLow() const noexcept
    {
        BOOL lowMemory = FALSE;
        return m_handle != nullptr
            && QueryMemoryResourceNotification(m_handle, &lowMemory)
            && lowMemory != FALSE;
    }

    void SystemMemoryWatch::StopWait(bool closeHandle) noexcept
    {
        if (m_wait != nullptr)
        {
            UnregisterWaitEx(m_wait, INVALID_HANDLE_VALUE);
            m_wait = nullptr;
        }
        if (closeHandle && m_handle != nullptr)
        {
            CloseHandle(m_handle);
            m_handle = nullptr;
        }
    }

    VOID CALLBACK SystemMemoryWatch::WaitCallback(PVOID context, BOOLEAN)
    {
        auto* self = static_cast<SystemMemoryWatch*>(context);
        if (self != nullptr && self->m_window != nullptr && self->m_message != 0)
        {
            PostMessageW(self->m_window, self->m_message, 0, 0);
        }
    }
}
