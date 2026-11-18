#pragma once

#include <windows.h>

#include <memory>

namespace agshot
{
    struct FrameSignal;

    // Requests VSync-aligned frame tick messages for a window using a one-shot
    // rAF-style interface while keeping the underlying clock subscription warm.
    class CompositorClockClient
    {
    public:
        CompositorClockClient() = default;
        ~CompositorClockClient() noexcept { Stop(); }

        CompositorClockClient(const CompositorClockClient&) = delete;
        CompositorClockClient& operator=(const CompositorClockClient&) = delete;

        bool RequestFrame(HWND window, UINT frameMessage) noexcept;
        void BeginFrame() noexcept;
        void EndFrame() noexcept;
        void Cancel() noexcept;
        void Stop() noexcept;

    private:
        void DrainPendingMessages() noexcept;

        std::shared_ptr<FrameSignal> m_signal;
        HWND m_window{};
        UINT m_message{};
    };
}
