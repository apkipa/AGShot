#include "pch.h"

#include "core/ui_element.h"

#include <dwmapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <limits>

namespace agshot
{
    namespace
    {
        constexpr DWORD kUiaTimeoutMs = 25;
        constexpr int kMaxQuerySteps = 32;
        constexpr int kMaxBatchExpansions = 6;
        constexpr std::size_t kMaxChildrenPerNode = 4096;
        constexpr std::size_t kMaxNodesPerWindow = 4096;
        constexpr std::size_t kMaxTotalNodes = 16384;
        constexpr std::size_t kMaxLadderRects = 12;

        struct FlagReset
        {
            bool& flag;
            ~FlagReset() { flag = false; }
        };

        struct ChildNode
        {
            winrt::com_ptr<IUIAutomationElement> element;
            RECT bounds{};
            std::vector<std::size_t> children;
            std::size_t parent{ std::numeric_limits<std::size_t>::max() };
            bool loaded{};
            bool failed{};
            bool structural{};
        };

        struct WindowSnapshot
        {
            HWND hwnd{};
            RECT bounds{};
            std::size_t zOrder{};
            RECT clientBounds{};
            std::vector<RECT> childWindowRects;
            std::vector<ChildNode> nodes;
        };

        struct Enumeration
        {
            HWND excluded{};
            std::vector<WindowSnapshot>* windows{};
        };

        bool IsCloaked(HWND hwnd) noexcept
        {
            DWORD cloaked = 0;
            return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
                && cloaked != 0;
        }

        RECT VisibleFrame(HWND hwnd) noexcept
        {
            RECT rect{};
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
            {
                GetWindowRect(hwnd, &rect);
            }
            return rect;
        }

        bool Contains(const RECT& rect, POINT point) noexcept
        {
            return point.x >= rect.left && point.x < rect.right
                && point.y >= rect.top && point.y < rect.bottom;
        }

        BOOL CALLBACK CollectChildWindow(HWND hwnd, LPARAM parameter)
        {
            auto& snapshot = *reinterpret_cast<WindowSnapshot*>(parameter);
            if (!IsWindowVisible(hwnd))
            {
                return TRUE;
            }

            RECT rect{};
            if (!GetWindowRect(hwnd, &rect))
            {
                return TRUE;
            }

            RECT clipped{};
            if (IntersectRect(&clipped, &rect, &snapshot.bounds) && !IsRectEmpty(&clipped)
                && !EqualRect(&clipped, &snapshot.bounds))
            {
                snapshot.childWindowRects.push_back(clipped);
            }
            return TRUE;
        }

        BOOL CALLBACK CollectTopWindow(HWND hwnd, LPARAM parameter)
        {
            auto& enumeration = *reinterpret_cast<Enumeration*>(parameter);
            if (hwnd == enumeration.excluded || !IsWindowVisible(hwnd) || IsIconic(hwnd))
            {
                return TRUE;
            }

            const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if ((exStyle & (WS_EX_LAYERED | WS_EX_TRANSPARENT))
                == (WS_EX_LAYERED | WS_EX_TRANSPARENT))
            {
                return TRUE;
            }

            RECT bounds = VisibleFrame(hwnd);
            if (IsRectEmpty(&bounds) || IsCloaked(hwnd))
            {
                return TRUE;
            }

            WindowSnapshot snapshot;
            snapshot.hwnd = hwnd;
            snapshot.bounds = bounds;
            snapshot.zOrder = enumeration.windows->size();
            ChildNode root;
            root.bounds = bounds;
            snapshot.nodes.push_back(std::move(root));

            RECT client{};
            POINT origin{ 0, 0 };
            if (GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &origin))
            {
                const RECT screenClient{ origin.x, origin.y,
                                         origin.x + client.right, origin.y + client.bottom };
                IntersectRect(&snapshot.clientBounds, &screenClient, &bounds);
            }
            EnumChildWindows(hwnd, CollectChildWindow, reinterpret_cast<LPARAM>(&snapshot));
            enumeration.windows->push_back(std::move(snapshot));
            return TRUE;
        }

        LONG Area(const RECT& rect) noexcept
        {
            return std::max<LONG>(0, rect.right - rect.left)
                * std::max<LONG>(0, rect.bottom - rect.top);
        }
    }

    struct ElementPicker::Impl
    {
        winrt::com_ptr<IUIAutomation> automation;
        winrt::com_ptr<IUIAutomation2> automation2;
        winrt::com_ptr<IUIAutomationCacheRequest> request;
        winrt::com_ptr<IUIAutomationCondition> controlView;
        std::vector<WindowSnapshot> windows;
        std::size_t totalNodes{};
        bool querying{};
    };

    ElementPicker::ElementPicker() : m_impl(std::make_unique<Impl>())
    {
    }

    ElementPicker::~ElementPicker() = default;

    void ElementPicker::Freeze(HWND excludedWindow)
    {
        m_impl->windows.clear();
        m_impl->totalNodes = 0;
        m_impl->request = nullptr;
        m_impl->controlView = nullptr;

        Enumeration enumeration{ excludedWindow, &m_impl->windows };
        EnumWindows(CollectTopWindow, reinterpret_cast<LPARAM>(&enumeration));

        // COM is already initialised as STA by the application. Keep UIA on this
        // thread; do not create a second worker just to move the first provider call.
        if (!m_impl->automation
            && FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(m_impl->automation.put()))))
        {
            return;
        }
        if (!m_impl->automation2)
        {
            m_impl->automation2 = m_impl->automation.try_as<IUIAutomation2>();
        }
        if (!m_impl->automation2)
        {
            return;
        }

        if (FAILED(m_impl->automation->CreateCacheRequest(m_impl->request.put()))
            || FAILED(m_impl->automation->get_ControlViewCondition(m_impl->controlView.put())))
        {
            m_impl->request = nullptr;
            m_impl->controlView = nullptr;
            return;
        }

        m_impl->request->put_TreeScope(static_cast<TreeScope>(TreeScope_Element | TreeScope_Children));
        m_impl->request->put_TreeFilter(m_impl->controlView.get());
        m_impl->request->put_AutomationElementMode(AutomationElementMode_Full);
        m_impl->request->AddProperty(UIA_BoundingRectanglePropertyId);
        m_impl->request->AddProperty(UIA_IsOffscreenPropertyId);
        m_impl->request->AddProperty(UIA_ControlTypePropertyId);
    }

    int ElementPicker::Ladder(POINT point, std::vector<RECT>& ladder, int expansionBudget)
    {
        if (m_impl->querying)
        {
            return -1;
        }

        ladder.clear();
        m_impl->querying = true;
        const FlagReset resetQuery{ m_impl->querying };
        // EnumWindows preserved front-to-back order. Pick exactly one window before
        // asking UIA anything, so a covered application's element can never win.
        WindowSnapshot* window = nullptr;
        for (auto& candidate : m_impl->windows)
        {
            if (Contains(candidate.bounds, point))
            {
                window = &candidate;
                break;
            }
        }
        if (window == nullptr)
        {
            return 0;
        }

        if (!m_impl->automation || !m_impl->automation2 || !m_impl->request)
        {
            for (const RECT& childRect : window->childWindowRects)
            {
                if (Contains(childRect, point))
                {
                    ladder.push_back(childRect);
                }
            }
            if (!IsRectEmpty(&window->clientBounds) && Contains(window->clientBounds, point))
            {
                ladder.push_back(window->clientBounds);
            }
            ladder.push_back(window->bounds);
            std::stable_sort(ladder.begin(), ladder.end(), [](const RECT& left, const RECT& right) {
                return Area(left) < Area(right);
            });
            return static_cast<int>(ladder.size());
        }

        m_impl->automation2->put_ConnectionTimeout(kUiaTimeoutMs);
        m_impl->automation2->put_TransactionTimeout(kUiaTimeoutMs);

        const int boundedExpansionBudget = std::clamp(
            expansionBudget, 0, kMaxBatchExpansions);
        int expansions = 0;
        std::size_t current = 0;
        for (int step = 0; step < kMaxQuerySteps; ++step)
        {
            if (!window->nodes[current].loaded && !window->nodes[current].failed)
            {
                if (expansions >= boundedExpansionBudget
                    || m_impl->totalNodes >= kMaxTotalNodes)
                {
                    break;
                }
                ++expansions;

                auto& node = window->nodes[current];
                winrt::com_ptr<IUIAutomationElement> updated;
                HRESULT hr = S_OK;
                if (current == 0)
                {
                    hr = m_impl->automation->ElementFromHandleBuildCache(
                        window->hwnd, m_impl->request.get(), updated.put());
                }
                else
                {
                    hr = node.element->BuildUpdatedCache(m_impl->request.get(), updated.put());
                }

                if (FAILED(hr) || !updated)
                {
                    node.failed = true;
                }
                else
                {
                    node.element = std::move(updated);
                    node.loaded = true;

                    winrt::com_ptr<IUIAutomationElementArray> children;
                    if (SUCCEEDED(node.element->GetCachedChildren(children.put())) && children)
                    {
                        int count = 0;
                        if (SUCCEEDED(children->get_Length(&count)) && count > 0)
                        {
                            const int boundedCount = std::min<int>(
                                count, static_cast<int>(kMaxChildrenPerNode));
                            node.children.reserve(static_cast<std::size_t>(boundedCount));
                            for (int i = 0; i < boundedCount
                                 && window->nodes.size() < kMaxNodesPerWindow; ++i)
                            {
                                winrt::com_ptr<IUIAutomationElement> child;
                                if (FAILED(children->GetElement(i, child.put())) || !child)
                                {
                                    continue;
                                }

                                BOOL offscreen = FALSE;
                                RECT childBounds{};
                                RECT clippedBounds{};
                                if (FAILED(child->get_CachedIsOffscreen(&offscreen)) || offscreen
                                    || FAILED(child->get_CachedBoundingRectangle(&childBounds))
                                    || IsRectEmpty(&childBounds)
                                    || !IntersectRect(&clippedBounds, &childBounds, &window->bounds)
                                    || IsRectEmpty(&clippedBounds))
                                {
                                    continue;
                                }

                                int controlType = 0;
                                child->get_CachedControlType(&controlType);
                                const std::size_t index = window->nodes.size();
                                ChildNode childNode;
                                childNode.element = std::move(child);
                                childNode.bounds = clippedBounds;
                                childNode.parent = current;
                                childNode.structural = controlType == UIA_PaneControlTypeId
                                    || controlType == UIA_GroupControlTypeId;
                                window->nodes.push_back(std::move(childNode));
                                window->nodes[current].children.push_back(index);
                                ++m_impl->totalNodes;
                            }
                        }
                    }
                }
            }

            const auto children = window->nodes[current].children;
            std::size_t hit = std::numeric_limits<std::size_t>::max();
            // UIA's later siblings are visually in front of earlier overlapping
            // siblings, matching SnowShot's hit_before traversal.
            for (auto it = children.rbegin(); it != children.rend(); ++it)
            {
                if (Contains(window->nodes[*it].bounds, point))
                {
                    hit = *it;
                    break;
                }
            }
            if (hit == std::numeric_limits<std::size_t>::max())
            {
                // UIA may expose layout-only Pane/Group nodes with the same
                // bounds as their parent. If that branch has no hit child, walk
                // back through equal-bounds structural nodes and try the next
                // overlapping sibling, as SnowShot's cache does.
                std::size_t structural = current;
                bool foundAlternative = false;
                while (structural != 0)
                {
                    const std::size_t parent = window->nodes[structural].parent;
                    if (parent == std::numeric_limits<std::size_t>::max()
                        || !window->nodes[structural].structural
                        || !EqualRect(&window->nodes[structural].bounds,
                                      &window->nodes[parent].bounds))
                    {
                        break;
                    }

                    const auto& siblings = window->nodes[parent].children;
                    const auto position = std::find(siblings.begin(), siblings.end(), structural);
                    auto earlier = position;
                    while (earlier != siblings.begin())
                    {
                        --earlier;
                        if (Contains(window->nodes[*earlier].bounds, point))
                        {
                            current = *earlier;
                            foundAlternative = true;
                            break;
                        }
                    }
                    if (foundAlternative)
                    {
                        break;
                    }
                    structural = parent;
                }
                if (foundAlternative)
                {
                    continue;
                }
                break;
            }
            current = hit;
        }

        // Return the cached hit path, innermost first, ending at the frozen window.
        std::vector<RECT> reversePath;
        std::size_t node = current;
        while (node != 0 && reversePath.size() < kMaxLadderRects - 1)
        {
            const RECT bounds = window->nodes[node].bounds;
            if (!EqualRect(&bounds, &window->bounds)
                && std::none_of(reversePath.begin(), reversePath.end(),
                                [&bounds](const RECT& existing) { return EqualRect(&bounds, &existing) != FALSE; }))
            {
                reversePath.push_back(bounds);
            }
            node = window->nodes[node].parent;
            if (node == std::numeric_limits<std::size_t>::max())
            {
                break;
            }
        }
        ladder = std::move(reversePath);

        if (!IsRectEmpty(&window->clientBounds) && Contains(window->clientBounds, point))
        {
            ladder.push_back(window->clientBounds);
        }
        if (ladder.empty())
        {
            // Classic HWND controls are a fallback only when UIA has no path.
            // Preserve hierarchy in the primary UIA path rather than globally
            // reordering unrelated rectangles by area.
            std::vector<RECT> childHits;
            for (const RECT& childRect : window->childWindowRects)
            {
                if (Contains(childRect, point))
                {
                    childHits.push_back(childRect);
                }
            }
            std::stable_sort(childHits.begin(), childHits.end(), [](const RECT& left, const RECT& right) {
                return Area(left) < Area(right);
            });
            for (const RECT& childRect : childHits)
            {
                ladder.push_back(childRect);
            }
        }
        if (!IsRectEmpty(&window->clientBounds) && Contains(window->clientBounds, point)
            && std::none_of(ladder.begin(), ladder.end(), [&window](const RECT& rect) {
                   return EqualRect(&rect, &window->clientBounds) != FALSE;
               }))
        {
            ladder.push_back(window->clientBounds);
        }
        if (std::none_of(ladder.begin(), ladder.end(), [&window](const RECT& rect) {
                return EqualRect(&rect, &window->bounds) != FALSE;
            }))
        {
            ladder.push_back(window->bounds);
        }

        ladder.erase(std::unique(ladder.begin(), ladder.end(), [](const RECT& left, const RECT& right) {
            return EqualRect(&left, &right) != FALSE;
        }), ladder.end());
        if (ladder.size() > kMaxLadderRects)
        {
            ladder.resize(kMaxLadderRects);
        }
        return static_cast<int>(ladder.size());
    }
}
