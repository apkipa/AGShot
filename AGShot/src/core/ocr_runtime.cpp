#include "pch.h"

#include "core/ocr_runtime.h"
#include "core/ocr_manifest.h"
#include "../../../OcrEngine/src/ocr_engine.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <utility>

namespace agshot
{
    namespace
    {
        std::wstring Utf8ToWide(const std::string& value)
        {
            if (value.empty()) return {};
            const int length = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (length <= 0) return L"[invalid UTF-8 OCR result]";
            std::wstring result(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                result.data(), length);
            return result;
        }

        std::wstring ErrorText(const std::exception& error)
        {
            return Utf8ToWide(error.what());
        }

        void Finish(const std::shared_ptr<OcrCompletion>& completion, bool ok,
                    std::wstring text, std::wstring error)
        {
            std::lock_guard lock(completion->mutex);
            completion->ok = ok;
            completion->text = std::move(text);
            completion->error = std::move(error);
        }

        void PostResult(HWND target, std::shared_ptr<OcrCompletion> completion)
        {
            if (!IsWindow(target)) return;
            auto* message = new (std::nothrow) std::shared_ptr<OcrCompletion>(std::move(completion));
            if (message == nullptr
                || !PostMessageW(target, kOcrResultMessage, 0, reinterpret_cast<LPARAM>(message)))
            {
                delete message;
            }
        }
    }

    OcrRuntime::OcrRuntime() = default;

    OcrRuntime::~OcrRuntime()
    {
        Shutdown();
    }

    void OcrRuntime::Shutdown()
    {
        if (!m_work) return; // already shut down

        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_pending.reset();
            m_trimRequested = false;
        }

        // Drops work that has not started yet and waits for a callback that has
        // started, so no callback can touch this object after it returns. This is
        // process shutdown, not a user-facing OCR cancellation operation.
        WaitForThreadpoolWorkCallbacks(m_work, TRUE);
        CloseThreadpoolWork(m_work);
        m_work = nullptr;

        // Released on the calling thread, while the process-wide NPU engine is
        // still alive: letting the models go during static destruction would put
        // zeMemFree after that engine's own teardown.
        m_engine.reset();
        m_hasLoadedEngine.store(false, std::memory_order_release);
    }

    void OcrRuntime::Start(const std::filesystem::path& models,
                           const std::filesystem::path& cache)
    {
        if (models.empty() || m_work) return;
        m_models = models;
        m_cache = cache;

        // The system thread pool: one work item covers every request that is
        // waiting, and the callback runs on the pool the process already has.
        m_work = CreateThreadpoolWork(WorkCallback, this, nullptr);
    }

    bool OcrRuntime::available() const noexcept
    {
        return m_work != nullptr;
    }

    bool OcrRuntime::hasLoadedEngine() const noexcept
    {
        return m_hasLoadedEngine.load(std::memory_order_acquire);
    }

    void OcrRuntime::Trim() noexcept
    {
        if (!m_work) return;

        bool submitWork = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) return;
            if (!m_queued && !m_hasLoadedEngine.load(std::memory_order_acquire))
            {
                return;
            }
            m_trimRequested = true;
            if (!m_queued)
            {
                m_queued = true;
                submitWork = true;
            }
        }
        if (submitWork)
        {
            SubmitThreadpoolWork(m_work);
        }
    }

    std::shared_ptr<OcrCompletion> OcrRuntime::SubmitRegion(
        HWND target, const void* pixels,
        int imageWidth, int imageHeight, int imageStride,
        const RECT& crop)
    {
        if (!m_work || pixels == nullptr || imageWidth <= 0 || imageHeight <= 0 || imageStride <= 0)
        {
            return {};
        }

        const int left = std::clamp<int>(crop.left, 0, imageWidth);
        const int top = std::clamp<int>(crop.top, 0, imageHeight);
        const int right = std::clamp<int>(crop.right, left, imageWidth);
        const int bottom = std::clamp<int>(crop.bottom, top, imageHeight);
        const int width = right - left;
        const int height = bottom - top;
        if (width <= 0 || height <= 0)
        {
            return {};
        }

        // Copy only raw BGRA rows on the UI thread; alpha stripping happens on
        // the worker thread so entering OCR mode stays responsive.
        BgraImage region;
        region.width = width;
        region.height = height;
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
        region.pixels.resize(rowBytes * static_cast<std::size_t>(height));
        const auto* source = static_cast<const std::uint8_t*>(pixels);
        for (int y = 0; y < height; ++y)
        {
            const std::uint8_t* row = source
                + static_cast<std::size_t>(top + y) * static_cast<std::size_t>(imageStride)
                + static_cast<std::size_t>(left) * 4;
            std::memcpy(region.pixels.data() + static_cast<std::size_t>(y) * rowBytes, row, rowBytes);
        }

        auto completion = std::make_shared<OcrCompletion>();
        std::lock_guard lock(m_mutex);
        if (m_stopping) return {};
        // A newer submission means the engine is needed again, superseding any
        // prior idle Trim() request (though a Trim() arriving *after* this call
        // will still unload once this job drains).
        m_trimRequested = false;
        m_pending = Job{ target, completion, std::move(region) };
        if (!m_queued)
        {
            m_queued = true;
            SubmitThreadpoolWork(m_work);
        }
        return completion;
    }

    std::wstring OcrRuntime::EnsureEngine()
    {
        if (m_engine) return {};
        if (m_initializationAttempted) return m_initializationError;

        m_initializationAttempted = true;
        try
        {
            const OcrModelPack pack = LoadOcrModelManifest(m_models);

            ppocr::OcrEngineOptions options;
            options.det_model = pack.det.path.wstring();
            options.rec_model = pack.rec.path.wstring();
            options.dictionary = pack.dict.path.wstring();
            options.det_sha256 = pack.det.sha256;
            options.rec_sha256 = pack.rec.sha256;
            options.cache_dir = m_cache.wstring();

            auto engine = std::make_unique<ppocr::OcrEngine>();
            engine->initialize(options);
            m_engine = std::move(engine);
            m_hasLoadedEngine.store(true, std::memory_order_release);
        }
        catch (const std::exception& error)
        {
            m_initializationError = ErrorText(error);
        }
        catch (...)
        {
            m_initializationError = L"Unknown OCR initialization failure.";
        }
        return m_initializationError;
    }

    void OcrRuntime::UnloadEngine() noexcept
    {
        // Only reset a successfully loaded engine so a cached initialization
        // failure stays reported until models change or the app restarts.
        if (!m_engine) return;
        m_engine.reset();
        m_hasLoadedEngine.store(false, std::memory_order_release);
        m_initializationAttempted = false;
        m_initializationError.clear();
    }

    // Runs the queued jobs one at a time. The NPU engine is not reentrant and
    // already pipelines inside itself, so one callback at a time is all this
    // needs; that is also why the queued flag gates the next submission.
    void OcrRuntime::DrainJobs()
    {
        for (;;)
        {
            Job job;
            bool shouldUnload = false;
            {
                std::lock_guard lock(m_mutex);
                if (!m_pending)
                {
                    if (m_trimRequested)
                    {
                        m_trimRequested = false;
                        shouldUnload = true;
                    }
                    else
                    {
                        // Clearing the queued flag under the same lock as the check
                        // is what makes a Submit racing with this loop either find
                        // the callback running or queue its own work item; a job can
                        // never be left behind.
                        m_queued = false;
                        return;
                    }
                }
                else
                {
                    job = std::move(*m_pending);
                    m_pending.reset();
                }
            }

            if (shouldUnload)
            {
                UnloadEngine();
                continue;
            }

            if (const std::wstring error = EnsureEngine(); !error.empty())
            {
                Finish(job.completion, false, {}, error);
                PostResult(job.target, std::move(job.completion));
                continue;
            }

            try
            {
                ppocr::ImageBGR bgr;
                bgr.width = job.image.width;
                bgr.height = job.image.height;
                bgr.channels = 3;
                bgr.data.resize(static_cast<std::size_t>(job.image.width) * job.image.height * 3);
                const std::size_t pixelCount = static_cast<std::size_t>(job.image.width) * job.image.height;
                const std::uint8_t* src = job.image.pixels.data();
                std::uint8_t* dst = bgr.data.data();
                for (std::size_t i = 0; i < pixelCount; ++i)
                {
                    dst[i * 3 + 0] = src[i * 4 + 0];
                    dst[i * 3 + 1] = src[i * 4 + 1];
                    dst[i * 3 + 2] = src[i * 4 + 2];
                }

                const auto lines = m_engine->recognize(bgr);
                std::wstring text;
                for (const auto& line : lines)
                {
                    if (!text.empty()) text += L"\r\n";
                    text += Utf8ToWide(line.text);
                }
                Finish(job.completion, true, std::move(text), {});
                PostResult(job.target, std::move(job.completion));
            }
            catch (const std::exception& error)
            {
                Finish(job.completion, false, {}, ErrorText(error));
                PostResult(job.target, std::move(job.completion));
            }
            catch (...)
            {
                Finish(job.completion, false, {}, L"Unknown OCR recognition failure.");
                PostResult(job.target, std::move(job.completion));
            }
        }
    }

    VOID CALLBACK OcrRuntime::WorkCallback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK)
    {
        auto& self = *static_cast<OcrRuntime*>(context);
        {
            std::lock_guard lock(self.m_mutex);
            if (self.m_stopping)
            {
                self.m_queued = false;
                return;
            }
        }

        // Model loading and recognition hold this thread for seconds; the pool
        // is told so it does not size a replacement thread around it.
        CallbackMayRunLong(instance);

        self.DrainJobs();
    }
}
