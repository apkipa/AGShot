#include "pch.h"

#include "hud/compositor_clock.h"

#include <winrt/base.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace agshot
{
    namespace
    {
        constexpr DWORD kCompositorClockTimeoutMs = 50;
        constexpr DWORD kClockRetryDelayMs = 50;

        using WaitForCompositorClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

        WaitForCompositorClockFn CompositorClockWaiter() noexcept
        {
            static const auto wait = reinterpret_cast<WaitForCompositorClockFn>(
                GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionWaitForCompositorClock"));
            return wait;
        }

        class CompositorClockService
        {
        public:
            winrt::event_token Add(winrt::delegate<void()> callback)
            {
                winrt::event_token token{};
                {
                    std::lock_guard lock(m_waitMutex);
                    token = m_clockTick.add(callback);
                }
                m_wake.notify_one();
                return token;
            }

            void Remove(winrt::event_token token)
            {
                {
                    std::lock_guard lock(m_waitMutex);
                    m_clockTick.remove(token);
                }
                m_wake.notify_one();
            }

        private:
            CompositorClockService()
            {
                std::thread([this] { Run(); }).detach();
            }

            void Run() noexcept
            {
                const auto waitForClock = CompositorClockWaiter();
                for (;;)
                {
                    {
                        std::unique_lock lock(m_waitMutex);
                        m_wake.wait(lock, [this] { return static_cast<bool>(m_clockTick); });
                    }

                    if (waitForClock == nullptr)
                    {
                        Sleep(16);
                        m_clockTick();
                        continue;
                    }

                    const DWORD status = waitForClock(0, nullptr, kCompositorClockTimeoutMs);
                    if (status == ERROR_SUCCESS)
                    {
                        m_clockTick();
                    }
                    else if (status != WAIT_TIMEOUT)
                    {
                        Sleep(kClockRetryDelayMs);
                    }
                }
            }

            std::mutex m_waitMutex;
            std::condition_variable m_wake;
            winrt::event<winrt::delegate<void()>> m_clockTick;

            friend CompositorClockService& GlobalCompositorClock();
        };

        CompositorClockService& GlobalCompositorClock()
        {
            static auto* service = new CompositorClockService();
            return *service;
        }
    }

    bool CompositorClockClient::Start(HWND window, UINT frameMessage) noexcept
    {
        if (m_signal)
        {
            return true;
        }

        try
        {
            auto signal = std::make_shared<FrameSignal>();
            signal->window = window;
            signal->message = frameMessage;
            m_signal = signal;
            m_window = window;
            m_message = frameMessage;
            m_token = GlobalCompositorClock().Add(winrt::delegate<void()>{ [signal]()
            {
                std::lock_guard lock(signal->mutex);
                if (!signal->active || signal->posted)
                {
                    return;
                }

                signal->posted = true;
                if (!PostMessageW(signal->window, signal->message, 0, 0))
                {
                    signal->posted = false;
                }
            } });
            return true;
        }
        catch (...)
        {
            m_signal.reset();
            return false;
        }
    }

    void CompositorClockClient::MarkFrameHandled() noexcept
    {
        if (m_signal)
        {
            std::lock_guard lock(m_signal->mutex);
            m_signal->posted = false;
        }
    }

    void CompositorClockClient::Stop() noexcept
    {
        if (m_signal)
        {
            {
                std::lock_guard lock(m_signal->mutex);
                m_signal->active = false;
            }
            GlobalCompositorClock().Remove(m_token);
            m_token = {};
            m_signal.reset();
        }

        if (m_window != nullptr && m_message != 0)
        {
            MSG pending{};
            while (PeekMessageW(&pending, m_window, m_message, m_message, PM_REMOVE))
            {
            }
        }
    }
}
