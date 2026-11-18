#include "pch.h"

#include "hud/compositor_clock.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace agshot
{
    struct FrameSignal
    {
        HWND window{};
        UINT message{};
        bool registered{ true };
        bool requested{};
        bool inFlight{};
    };

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
            void Register(const std::shared_ptr<FrameSignal>& signal)
            {
                std::lock_guard lock(m_mutex);
                m_signals.push_back(signal);
            }

            void Unregister(const std::shared_ptr<FrameSignal>& signal) noexcept
            {
                std::lock_guard lock(m_mutex);
                signal->registered = false;
                signal->requested = false;
                signal->inFlight = false;
                std::erase(m_signals, signal);
            }

            void Request(const std::shared_ptr<FrameSignal>& signal) noexcept
            {
                std::lock_guard lock(m_mutex);
                signal->requested = true;
                if (!signal->inFlight)
                {
                    m_wake.notify_one();
                }
            }

            void BeginFrame(const std::shared_ptr<FrameSignal>& signal) noexcept
            {
                std::lock_guard lock(m_mutex);
                signal->requested = false;
            }

            void EndFrame(const std::shared_ptr<FrameSignal>& signal) noexcept
            {
                std::lock_guard lock(m_mutex);
                signal->inFlight = false;
                if (signal->requested)
                {
                    m_wake.notify_one();
                }
            }

            void Cancel(const std::shared_ptr<FrameSignal>& signal) noexcept
            {
                std::lock_guard lock(m_mutex);
                signal->requested = false;
                signal->inFlight = false;
            }

        private:
            CompositorClockService()
            {
                std::thread([this] { Run(); }).detach();
            }

            bool HasDispatchableLocked() const noexcept
            {
                return std::any_of(m_signals.begin(), m_signals.end(), [](const auto& signal)
                {
                    return signal->registered && signal->requested && !signal->inFlight;
                });
            }

            void Dispatch() noexcept
            {
                std::lock_guard lock(m_mutex);
                for (const auto& signal : m_signals)
                {
                    if (!signal->registered || !signal->requested || signal->inFlight)
                    {
                        continue;
                    }

                    signal->requested = false;
                    if (PostMessageW(signal->window, signal->message, 0, 0))
                    {
                        signal->inFlight = true;
                    }
                }
            }

            void Run() noexcept
            {
                const auto waitForClock = CompositorClockWaiter();
                for (;;)
                {
                    {
                        std::unique_lock lock(m_mutex);
                        m_wake.wait(lock, [this] { return HasDispatchableLocked(); });
                    }

                    if (waitForClock == nullptr)
                    {
                        Sleep(16);
                        Dispatch();
                        continue;
                    }

                    const DWORD status = waitForClock(0, nullptr, kCompositorClockTimeoutMs);
                    if (status == ERROR_SUCCESS)
                    {
                        Dispatch();
                    }
                    else if (status != WAIT_TIMEOUT)
                    {
                        Sleep(kClockRetryDelayMs);
                    }
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_wake;
            std::vector<std::shared_ptr<FrameSignal>> m_signals;

            friend CompositorClockService& GlobalCompositorClock();
        };

        CompositorClockService& GlobalCompositorClock()
        {
            static auto* service = new CompositorClockService();
            return *service;
        }
    }

    bool CompositorClockClient::RequestFrame(HWND window, UINT frameMessage) noexcept
    {
        if (window == nullptr || frameMessage == 0)
        {
            return false;
        }

        if (m_signal && (m_window != window || m_message != frameMessage))
        {
            Stop();
        }

        if (!m_signal)
        {
            try
            {
                auto signal = std::make_shared<FrameSignal>();
                signal->window = window;
                signal->message = frameMessage;
                GlobalCompositorClock().Register(signal);
                m_signal = std::move(signal);
                m_window = window;
                m_message = frameMessage;
            }
            catch (...)
            {
                return false;
            }
        }

        GlobalCompositorClock().Request(m_signal);
        return true;
    }

    void CompositorClockClient::BeginFrame() noexcept
    {
        if (m_signal)
        {
            GlobalCompositorClock().BeginFrame(m_signal);
        }
    }

    void CompositorClockClient::EndFrame() noexcept
    {
        if (m_signal)
        {
            GlobalCompositorClock().EndFrame(m_signal);
        }
    }

    void CompositorClockClient::Cancel() noexcept
    {
        if (m_signal)
        {
            GlobalCompositorClock().Cancel(m_signal);
        }
        DrainPendingMessages();
    }

    void CompositorClockClient::Stop() noexcept
    {
        if (m_signal)
        {
            GlobalCompositorClock().Unregister(m_signal);
            m_signal.reset();
        }
        DrainPendingMessages();
        m_window = nullptr;
        m_message = 0;
    }

    void CompositorClockClient::DrainPendingMessages() noexcept
    {
        if (m_window != nullptr && m_message != 0)
        {
            MSG pending{};
            while (PeekMessageW(&pending, m_window, m_message, m_message, PM_REMOVE))
            {
            }
        }
    }
}
