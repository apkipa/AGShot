#pragma once

// Level Zero is resolved at run time and never linked.
//
// The loader that can actually reach the NPU is the one the driver installed
// in System32. Other copies sit in the DriverStore (1.20.6, 1.26.3, 1.29.0)
// and which one wins depends on driver install order, so linking
// ze_loader.lib would bind this binary to whatever import library happened to
// be present at build time. Instead each entry point is looked up by name in
// the loader the machine actually has.
//
// The shape follows OpenVINO's src/common/zero_loader (Apache-2.0): a table of
// pointers plus one wrapper per entry point, so call sites keep their normal
// spelling and zeInit(...) still reads like zeInit(...).
//
// Resolution happens on first call rather than at static-init, so a machine
// with no NPU starts up and fails at first use instead of refusing to load.
//
// Include after <ze_api.h>: decltype(&::zeInit) needs the real declaration.

#include <ze_api.h>

#include <type_traits>
#include <utility>

namespace ppocr {

// Every Level Zero entry point this engine calls.
#define PPOCR_ZE_SYMBOLS(X)                    \
    X(zeInit)                                  \
    X(zeDriverGet)                             \
    X(zeDriverGetExtensionProperties)          \
    X(zeDriverGetExtensionFunctionAddress)     \
    X(zeDeviceGet)                             \
    X(zeDeviceGetProperties)                   \
    X(zeDeviceGetCommandQueueGroupProperties)  \
    X(zeDeviceGetGlobalTimestamps)             \
    X(zeContextCreate)                         \
    X(zeContextDestroy)                        \
    X(zeCommandQueueCreate)                    \
    X(zeCommandQueueDestroy)                   \
    X(zeCommandQueueExecuteCommandLists)       \
    X(zeCommandQueueSynchronize)               \
    X(zeCommandListCreate)                     \
    X(zeCommandListDestroy)                    \
    X(zeCommandListReset)                      \
    X(zeCommandListClose)                      \
    X(zeCommandListAppendWriteGlobalTimestamp) \
    X(zeFenceCreate)                           \
    X(zeFenceDestroy)                          \
    X(zeFenceReset)                            \
    X(zeFenceQueryStatus)                      \
    X(zeFenceHostSynchronize)                  \
    X(zeMemAllocShared)                        \
    X(zeMemFree)

class ZeRuntime {
public:
    // Opens ze_loader.dll on first use and resolves the table. Throws if the
    // loader is absent or lacks an entry point this engine needs.
    static ZeRuntime& instance();

    ZeRuntime(const ZeRuntime&) = delete;
    ZeRuntime& operator=(const ZeRuntime&) = delete;

    // True once the loader is open, without forcing it open.
    static bool available() noexcept;

#define PPOCR_ZE_DECLARE(symbol) decltype(&::symbol) symbol = nullptr;
    PPOCR_ZE_SYMBOLS(PPOCR_ZE_DECLARE)
#undef PPOCR_ZE_DECLARE

private:
    ZeRuntime();
    ~ZeRuntime();

    void* loader_ = nullptr;
};

// One wrapper per entry point, so a call resolves the loader lazily. The
// loader is stored for the process lifetime, so the pointer cannot dangle.
#define PPOCR_ZE_WRAP(symbol)                                              \
    template <typename... Args>                                            \
    inline typename std::invoke_result<decltype(&::symbol), Args...>::type \
        wrapped_##symbol(Args... args) {                                   \
        const decltype(&::symbol) target = ZeRuntime::instance().symbol;   \
        if (target == nullptr) {                                           \
            return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;                    \
        }                                                                  \
        return target(std::forward<Args>(args)...);                        \
    }
PPOCR_ZE_SYMBOLS(PPOCR_ZE_WRAP)
#undef PPOCR_ZE_WRAP

// ...and the entry point's own name becomes a pointer to its wrapper, which
// shadows the declaration in ze_api.h for code inside namespace ppocr.
#define PPOCR_ZE_BIND(symbol) inline decltype(&::symbol) symbol = wrapped_##symbol;
PPOCR_ZE_SYMBOLS(PPOCR_ZE_BIND)
#undef PPOCR_ZE_BIND

}  // namespace ppocr
