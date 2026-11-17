#pragma once

#include <windows.h>

#include <memory>
#include <vector>

namespace agshot
{
    // A frozen z-order window snapshot with lazily expanded, cached UIA hit paths.
    class ElementPicker
    {
    public:
        ElementPicker();
        ~ElementPicker();

        ElementPicker(const ElementPicker&) = delete;
        ElementPicker& operator=(const ElementPicker&) = delete;

        // Snapshot visible top-level windows, omitting our overlay HWND.
        void Freeze(HWND excludedWindow);

        // Releases cached window snapshots and UIA element references while
        // keeping container capacity and automation objects ready for reuse.
        void Clear() noexcept;

        // Releases all cached nodes, container capacity, and automation COM
        // objects under memory pressure.
        void Trim() noexcept;

        // Resolve a point against the snapshot. At most two uncached UIA batches
        // are expanded; subsequent hits use only cached geometry and tree nodes.
        int Ladder(POINT point, std::vector<RECT>& ladder, int expansionBudget = 2);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
