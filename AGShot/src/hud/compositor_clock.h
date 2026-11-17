#pragma once

#include <windows.h>
#include <winrt/base.h>

#include <memory>
#include <mutex>

namespace agshot
{
    // Posts VSync-aligned frame tick messages to a window while at least one
    // client holds an active subscription.
    class CompositorClockClient
    {
    public:
        CompositorClockClient() = default;
        ~CompositorClockClient() noexcept { Stop(); }

        CompositorClockClient(const CompositorClockClient&) = delete;
        CompositorClockClient& operator=(const CompositorClockClient&) = delete;

        bool Start(HWND window, UINT frameMessage) noexcept;
        void MarkFrameHandled() noexcept;
        void Stop() noexcept;
        bool active() const noexcept { return static_cast<bool>(m_signal); }

    private:
        struct FrameSignal
        {
            std::mutex mutex;
            HWND window{};
            UINT message{};
            bool active{ true };
            bool posted{};
        };

        std::shared_ptr<FrameSignal> m_signal;
        winrt::event_token m_token{};
        HWND m_window{};
        UINT m_message{};
    };
}
