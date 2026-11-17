#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ppocr
{
    class OcrEngine;
}

namespace agshot
{
    constexpr UINT kOcrResultMessage = WM_APP + 0x4B;

    struct OcrCompletion
    {
        std::mutex mutex;
        bool ok{};
        std::wstring text;
        std::wstring error;
    };

    // Owns the slow NPU initialization and recognition work away from the UI
    // thread. The work runs as a single callback on the system thread pool; at
    // most one pending capture is kept, and a newer job replaces it before it
    // starts. A job that has started runs to completion.
    //
    // Owned by value on the UI thread; ~OcrRuntime() waits for any in-flight
    // callback before returning, so the callback never outlives what it touches.
    class OcrRuntime
    {
    public:
        OcrRuntime();
        ~OcrRuntime();

        OcrRuntime(const OcrRuntime&) = delete;
        OcrRuntime& operator=(const OcrRuntime&) = delete;

        void Start(const std::filesystem::path& models,
                   const std::filesystem::path& cache);
        bool available() const noexcept;
        bool hasLoadedEngine() const noexcept;

        // Copies the cropped BGRA rows out of the frozen capture and converts
        // to 24bpp BGR on the worker thread before calling the NPU engine.
        std::shared_ptr<OcrCompletion> SubmitRegion(
            HWND target, const void* pixels,
            int imageWidth, int imageHeight, int imageStride,
            const RECT& crop);

        // Non-blocking memory pressure trim: queues unloading of the NPU engine
        // once any pending/running recognition finishes, unless superseded by a
        // newer SubmitRegion() before the worker goes idle.
        void Trim() noexcept;

        // Refuses further requests, waits for a running recognition, and
        // releases the models. Safe to call more than once; the destructor does
        // it too. An application that owns the NPU engine should still call it
        // before static destruction begins, so the models are released while
        // that engine is alive.
        void Shutdown();

    private:
        struct BgraImage
        {
            int width{};
            int height{};
            std::vector<std::uint8_t> pixels; // tightly packed top-down BGRA
        };

        struct Job
        {
            HWND target{};
            std::shared_ptr<OcrCompletion> completion;
            BgraImage image;
        };

        static VOID CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK);

        // Loads the models on first use. Returns the reason it failed, or an
        // empty string once the engine is ready. Only the callback calls it.
        std::wstring EnsureEngine();
        void UnloadEngine() noexcept;
        void DrainJobs();

        std::mutex m_mutex;
        std::filesystem::path m_models;
        std::filesystem::path m_cache;

        // Latest capture wins: a request that arrives before the previous one
        // has started replaces it, so the callback never works through a
        // backlog.
        std::optional<Job> m_pending;
        bool m_stopping{};
        bool m_queued{};        // a work item is queued or running
        bool m_trimRequested{}; // drop loaded engine once queue drains

        // Only the callback touches these, and the queued flag is what keeps
        // two callbacks from overlapping.
        std::unique_ptr<ppocr::OcrEngine> m_engine;
        bool m_initializationAttempted{};
        std::wstring m_initializationError;
        std::atomic<bool> m_hasLoadedEngine{};

        // Touched only by the owning thread (Start, Submit, Shutdown).
        PTP_WORK m_work{};
    };
}
