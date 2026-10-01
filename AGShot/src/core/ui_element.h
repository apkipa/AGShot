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

        // Resolve a point against the snapshot. At most two uncached UIA batches
        // are expanded; subsequent hits use only cached geometry and tree nodes.
        int Ladder(POINT point, std::vector<RECT>& ladder, int expansionBudget = 2);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
