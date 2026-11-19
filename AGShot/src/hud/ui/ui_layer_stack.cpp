#include "pch.h"

#include "hud/ui/ui_layer_stack.h"

namespace agshot::ui
{
    void LayerStack::Register(IHudLayer* layer) noexcept
    {
        if (layer != nullptr && m_count < kMaxLayers)
        {
            m_layers[m_count++] = layer;
        }
    }

    IHudLayer* LayerStack::TopmostHit(POINT point) const
    {
        for (std::size_t i = 0; i < m_count; ++i)
        {
            IHudLayer* layer = m_layers[i];
            if (layer != nullptr && layer->IsVisible() && layer->HitTest(point))
            {
                return layer;
            }
        }
        return nullptr;
    }

    bool LayerStack::DispatchLeftDown(const PointerInput& in, Invalidate& inv)
    {
        IHudLayer* hit = TopmostHit(in.point);

        for (std::size_t i = 0; i < m_count; ++i)
        {
            IHudLayer* layer = m_layers[i];
            if (layer == nullptr || !layer->IsVisible())
            {
                continue;
            }
            if (layer == hit)
            {
                break;
            }
            const OutsideClickPolicy policy =
                layer->OnOutsidePointer(PointerButton::Left, in.point, hit, inv);
            if (policy == OutsideClickPolicy::DismissAndAbsorb)
            {
                if (m_hovered == layer)
                {
                    m_hovered = nullptr;
                }
                return true;
            }
            if (policy == OutsideClickPolicy::DismissAndPass && m_hovered == layer)
            {
                m_hovered = nullptr;
            }
        }

        if (hit == nullptr)
        {
            return false;
        }

        const EventOutcome outcome = hit->OnLeftDown(in, inv);
        if (outcome == EventOutcome::Capture)
        {
            m_captured = hit;
            return true;
        }
        return outcome == EventOutcome::Handled;
    }

    void LayerStack::DispatchMove(const PointerInput& in, Invalidate& inv)
    {
        if (m_captured != nullptr)
        {
            m_captured->OnCapturedMove(in, inv);
            return;
        }

        IHudLayer* hit = TopmostHit(in.point);
        if (hit != m_hovered)
        {
            if (m_hovered != nullptr)
            {
                m_hovered->OnHoverLeave(inv);
            }
            m_hovered = hit;
            inv |= Invalidate::Cursor;
        }
        if (m_hovered != nullptr)
        {
            m_hovered->OnHoverMove(in.point, inv);
        }
    }

    bool LayerStack::DispatchLeftUp(const PointerInput& in, Invalidate& inv)
    {
        if (m_captured == nullptr)
        {
            return false;
        }
        IHudLayer* target = m_captured;
        m_captured = nullptr;
        target->OnCapturedUp(in, inv);
        return true;
    }

    bool LayerStack::DispatchRightUp(const PointerInput& in, Invalidate& inv)
    {
        IHudLayer* hit = TopmostHit(in.point);

        for (std::size_t i = 0; i < m_count; ++i)
        {
            IHudLayer* layer = m_layers[i];
            if (layer == nullptr || !layer->IsVisible())
            {
                continue;
            }
            if (layer == hit)
            {
                break;
            }
            const OutsideClickPolicy policy =
                layer->OnOutsidePointer(PointerButton::Right, in.point, hit, inv);
            if (policy == OutsideClickPolicy::DismissAndAbsorb)
            {
                if (m_hovered == layer)
                {
                    m_hovered = nullptr;
                }
                return true;
            }
            if (policy == OutsideClickPolicy::DismissAndPass && m_hovered == layer)
            {
                m_hovered = nullptr;
            }
        }

        if (hit == nullptr)
        {
            return false;
        }
        return hit->OnRightUp(in, inv) != EventOutcome::Ignored;
    }

    bool LayerStack::DispatchWheel(const PointerInput& in, Invalidate& inv)
    {
        if (m_captured != nullptr)
        {
            return true;
        }
        for (std::size_t i = 0; i < m_count; ++i)
        {
            IHudLayer* layer = m_layers[i];
            if (layer == nullptr || !layer->IsVisible() || !layer->HitTest(in.point))
            {
                continue;
            }
            if (layer->OnWheel(in, inv) != EventOutcome::Ignored)
            {
                return true;
            }
        }
        return false;
    }

    bool LayerStack::DispatchKeyDown(WPARAM key, KeyModifiers mods, Invalidate& inv)
    {
        for (std::size_t i = 0; i < m_count; ++i)
        {
            IHudLayer* layer = m_layers[i];
            if (layer == nullptr || !layer->IsVisible())
            {
                continue;
            }
            if (layer->OnKeyDown(key, mods, inv) != EventOutcome::Ignored)
            {
                return true;
            }
        }
        return false;
    }

    bool LayerStack::DispatchChar(wchar_t ch, Invalidate& inv)
    {
        for (std::size_t i = 0; i < m_count; ++i)
        {
            IHudLayer* layer = m_layers[i];
            if (layer == nullptr || !layer->IsVisible())
            {
                continue;
            }
            if (layer->OnChar(ch, inv) != EventOutcome::Ignored)
            {
                return true;
            }
        }
        return false;
    }

    void LayerStack::DispatchModifiersChanged(POINT point, KeyModifiers mods, Invalidate& inv)
    {
        if (m_captured != nullptr)
        {
            m_captured->OnCapturedModifiersChanged(point, mods, inv);
        }
        inv |= Invalidate::Cursor;
    }

    void LayerStack::CancelCapture(Invalidate& inv)
    {
        if (m_captured != nullptr)
        {
            IHudLayer* target = m_captured;
            m_captured = nullptr;
            target->OnCancelCapture(inv);
        }
    }

    void LayerStack::ClearHover(Invalidate& inv)
    {
        m_hovered = nullptr;
        for (std::size_t i = 0; i < m_count; ++i)
        {
            if (m_layers[i] != nullptr)
            {
                m_layers[i]->OnHoverLeave(inv);
            }
        }
    }

    LPCWSTR LayerStack::ResolveCursor(POINT point) const
    {
        if (m_captured != nullptr)
        {
            if (const auto cursor = m_captured->CursorForPoint(point, true))
            {
                return *cursor;
            }
            return IDC_ARROW;
        }
        for (std::size_t i = 0; i < m_count; ++i)
        {
            IHudLayer* layer = m_layers[i];
            if (layer != nullptr && layer->IsVisible() && layer->HitTest(point))
            {
                if (const auto cursor = layer->CursorForPoint(point, false))
                {
                    return *cursor;
                }
            }
        }
        return IDC_ARROW;
    }

    const IHudLayer* LayerStack::FocusedImeLayer() const noexcept
    {
        for (std::size_t i = 0; i < m_count; ++i)
        {
            const IHudLayer* layer = m_layers[i];
            if (layer == nullptr || !layer->IsVisible())
            {
                continue;
            }
            const ImePolicy policy = layer->QueryImePolicy();
            if (policy == ImePolicy::Disable)
            {
                return nullptr;
            }
            if (policy == ImePolicy::Enable)
            {
                return layer;
            }
        }
        return nullptr;
    }
}
