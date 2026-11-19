#pragma once

#include "hud/ui/ui_types.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <optional>

namespace agshot::ui
{
    // Minimal non-owning HUD layer contract. Zero D2D/DWrite/HWND dependency:
    // concrete adapters in the host hold whatever rendering/document context they need.
    class IHudLayer
    {
    public:
        virtual ~IHudLayer() = default;

        virtual bool IsVisible() const noexcept = 0;
        virtual bool HitTest(POINT point) const = 0;

        virtual void OnHoverMove(POINT point, Invalidate& inv)
        {
            (void)point;
            (void)inv;
        }

        virtual void OnHoverLeave(Invalidate& inv)
        {
            (void)inv;
        }

        virtual OutsideClickPolicy OnOutsidePointer(
            PointerButton button, POINT point, const IHudLayer* hitBelow, Invalidate& inv)
        {
            (void)button;
            (void)point;
            (void)hitBelow;
            (void)inv;
            return OutsideClickPolicy::Ignore;
        }

        virtual EventOutcome OnLeftDown(const PointerInput& in, Invalidate& inv)
        {
            (void)in;
            (void)inv;
            return EventOutcome::Ignored;
        }

        virtual void OnCapturedMove(const PointerInput& in, Invalidate& inv)
        {
            (void)in;
            (void)inv;
        }

        virtual void OnCapturedUp(const PointerInput& in, Invalidate& inv)
        {
            (void)in;
            (void)inv;
        }

        virtual void OnCancelCapture(Invalidate& inv)
        {
            (void)inv;
        }

        virtual void OnCapturedModifiersChanged(POINT point, KeyModifiers mods, Invalidate& inv)
        {
            (void)point;
            (void)mods;
            (void)inv;
        }

        virtual EventOutcome OnRightUp(const PointerInput& in, Invalidate& inv)
        {
            (void)in;
            (void)inv;
            return EventOutcome::Ignored;
        }

        virtual EventOutcome OnWheel(const PointerInput& in, Invalidate& inv)
        {
            (void)in;
            (void)inv;
            return EventOutcome::Ignored;
        }

        virtual EventOutcome OnKeyDown(WPARAM key, KeyModifiers mods, Invalidate& inv)
        {
            (void)key;
            (void)mods;
            (void)inv;
            return EventOutcome::Ignored;
        }

        virtual EventOutcome OnChar(wchar_t ch, Invalidate& inv)
        {
            (void)ch;
            (void)inv;
            return EventOutcome::Ignored;
        }

        virtual std::optional<LPCWSTR> CursorForPoint(POINT point, bool captured) const
        {
            (void)point;
            (void)captured;
            return std::nullopt;
        }

        virtual ImePolicy QueryImePolicy() const noexcept
        {
            return ImePolicy::Pass;
        }
    };

    // Single Z-Order router owning capture, exclusive hover, light-dismiss, keyboard,
    // cursor, and IME arbitration across a fixed set of HUD layers (front-to-back).
    class LayerStack
    {
    public:
        static constexpr std::size_t kMaxLayers = 12;

        void Register(IHudLayer* layer) noexcept;

        IHudLayer* captured() const noexcept { return m_captured; }
        IHudLayer* hovered() const noexcept { return m_hovered; }
        bool hasCapture() const noexcept { return m_captured != nullptr; }

        bool DispatchLeftDown(const PointerInput& in, Invalidate& inv);
        void DispatchMove(const PointerInput& in, Invalidate& inv);
        bool DispatchLeftUp(const PointerInput& in, Invalidate& inv);
        bool DispatchRightUp(const PointerInput& in, Invalidate& inv);
        bool DispatchWheel(const PointerInput& in, Invalidate& inv);
        bool DispatchKeyDown(WPARAM key, KeyModifiers mods, Invalidate& inv);
        bool DispatchChar(wchar_t ch, Invalidate& inv);
        void DispatchModifiersChanged(POINT point, KeyModifiers mods, Invalidate& inv);

        void CancelCapture(Invalidate& inv);
        void ClearHover(Invalidate& inv);
        void ResetTransientState() noexcept
        {
            m_captured = nullptr;
            m_hovered = nullptr;
        }

        IHudLayer* TopmostHit(POINT point) const;
        LPCWSTR ResolveCursor(POINT point) const;
        const IHudLayer* FocusedImeLayer() const noexcept;
        bool ResolveWantIme() const noexcept { return FocusedImeLayer() != nullptr; }

    private:
        std::array<IHudLayer*, kMaxLayers> m_layers{};
        std::size_t m_count{};
        IHudLayer* m_captured{};
        IHudLayer* m_hovered{};
    };
}
