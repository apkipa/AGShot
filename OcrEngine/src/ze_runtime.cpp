#include "ze_runtime.hpp"

#include <windows.h>

#include <stdexcept>

namespace ppocr {

ZeRuntime::ZeRuntime() {
    // Bare name on purpose: let the normal search order find the copy the
    // driver installed in System32. Naming a path would pin this to one
    // driver's private copy, which is the coupling this exists to avoid.
    loader_ = ::LoadLibraryW(L"ze_loader.dll");
    if (loader_ == nullptr) {
        throw std::runtime_error(
            "ze_loader.dll could not be loaded; install the Intel NPU driver");
    }

#define PPOCR_ZE_RESOLVE(symbol)                                          \
    symbol = reinterpret_cast<decltype(&::symbol)>(                        \
        ::GetProcAddress(static_cast<HMODULE>(loader_), #symbol));
    PPOCR_ZE_SYMBOLS(PPOCR_ZE_RESOLVE)
#undef PPOCR_ZE_RESOLVE

    // These carry everything else: driver discovery, extension lookup and
    // memory allocation. A loader without them is not one this engine can
    // use, so say so here rather than returning a puzzling error later.
    if (zeInit == nullptr || zeDriverGet == nullptr ||
        zeDriverGetExtensionFunctionAddress == nullptr) {
        throw std::runtime_error(
            "ze_loader.dll is present but missing required Level Zero entry points");
    }
}

ZeRuntime::~ZeRuntime() {
    // The loader is kept for the process lifetime: entry points stay valid
    // for as long as any caller might use them.
}

ZeRuntime& ZeRuntime::instance() {
    // Function-local static: built on first use, thread-safe, and never before
    // main, so a machine without an NPU starts normally and fails only when
    // something actually asks for the NPU.
    static ZeRuntime runtime;
    return runtime;
}

bool ZeRuntime::available() noexcept {
    // A load attempt is the only honest test. The result is cached by the
    // static, so a failure does not retry on every call.
    return ::GetModuleHandleW(L"ze_loader.dll") != nullptr;
}

}  // namespace ppocr
