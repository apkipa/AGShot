#pragma once

#include "hud/annotation_controller.h"
#include "hud/annotations.h"
#include "hud/hud_renderer.h"
#include "hud/render_device.h"
#include "hud/toolbar.h"
#include "hud/ui/ui_dropdown.h"
#include "hud/ui/ui_layer_stack.h"
#include "hud/ui/ui_layout.h"
#include "hud/ui/ui_slider.h"
#include "hud/ui/ui_text_field.h"

#include <windows.h>

#include <optional>

namespace agshot
{
    // Minimal window/cross-layer callbacks implemented by Overlay so ToolbarController
    // stays decoupled from window, capture, and color-picker ownership.
    struct IToolbarHost
    {
        virtual ~IToolbarHost() = default;
        virtual HWND HostWindow() const noexcept = 0;
        virtual bool IsToolbarSuppressed() const noexcept = 0;
        virtual ToolbarState CurrentToolbarState() const noexcept = 0;
        virtual ui::LayoutEnv CurrentLayoutEnv() const noexcept = 0;
        virtual void OnToolbarButtonInvoked(ToolbarButton button, ui::Invalidate& inv) = 0;
        virtual void OnSubToolbarActionInvoked(SubToolbarAction action, ui::Invalidate& inv) = 0;
        virtual void DismissColorPicker(bool commitEdit, ui::Invalidate& inv) = 0;
        virtual void OnToolbarMoved() = 0;
    };

    // Owns the floating toolbar, contextual sub-toolbar rows, inline step-number
    // editor slot, mosaic/blur effect dropdown, and continuous intensity slider.
    class ToolbarController final : public ui::IHudLayer
    {
    public:
        ToolbarController();

        void Bind(IToolbarHost* host, AnnotationController* annot,
                  const RenderDevice* device, const ToolbarButton* toolMode) noexcept
        {
            m_host = host;
            m_annot = annot;
            m_device = device;
            m_toolMode = toolMode;
        }

        void Reset() noexcept;
        void Hide() noexcept;
        void InvalidateLayouts() noexcept;
        bool ClearHover() noexcept;
        void CloseMosaicEffectMenu() noexcept { m_mosaicEffectMenu.Close(); }

        void Settle(const ToolbarState& state, const RECT& selection, const ui::LayoutEnv& env);
        void LayoutSub(const ToolbarState& state, const ui::LayoutEnv& env);
        void UpdateHover(POINT point) noexcept;

        void BeginStepNumberEdit(ui::Invalidate& inv);
        void CommitStepNumberEdit(ui::Invalidate& inv);
        void CancelStepNumberEdit(ui::Invalidate& inv);
        void ToggleMosaicEffectMenu(ui::Invalidate& inv);

        void Draw(HudRenderer& renderer, const RenderDevice& device,
                  ToolbarButton activeMode, bool allowTooltip) const;

        bool visible() const noexcept { return m_visible; }
        bool isDraggingMove() const noexcept { return m_drag == DragMode::Move; }
        bool isMosaicMenuOpen() const noexcept { return m_mosaicEffectMenu.isOpen(); }
        bool isEditingStepNumber() const noexcept { return m_editingStepNumber; }
        const Toolbar& toolbar() const noexcept { return m_toolbar; }

        RECT PickerAnchorRect(SubToolbarAction anchorAction) const noexcept;
        RECT PickerAvoidBand() const noexcept;
        RECT StepNumberItemRect() const noexcept;
        RECT MosaicEffectAnchorRect() const noexcept;

        // ui::IHudLayer overrides
        bool IsVisible() const noexcept override;
        bool HitTest(POINT point) const override;
        void OnHoverMove(POINT point, ui::Invalidate& inv) override;
        void OnHoverLeave(ui::Invalidate& inv) override;
        ui::OutsideClickPolicy OnOutsidePointer(
            ui::PointerButton button, POINT point, const ui::IHudLayer* hitBelow,
            ui::Invalidate& inv) override;
        ui::EventOutcome OnLeftDown(const ui::PointerInput& in, ui::Invalidate& inv) override;
        void OnCapturedMove(const ui::PointerInput& in, ui::Invalidate& inv) override;
        void OnCapturedUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
        void OnCancelCapture(ui::Invalidate& inv) override;
        ui::EventOutcome OnRightUp(const ui::PointerInput& in, ui::Invalidate& inv) override;
        ui::EventOutcome OnWheel(const ui::PointerInput& in, ui::Invalidate& inv) override;
        ui::EventOutcome OnKeyDown(WPARAM key, KeyModifiers mods, ui::Invalidate& inv) override;
        ui::EventOutcome OnChar(wchar_t ch, ui::Invalidate& inv) override;
        std::optional<LPCWSTR> CursorForPoint(POINT point, bool captured) const override;
        ui::ImePolicy QueryImePolicy() const noexcept override;

    private:
        enum class DragMode
        {
            None,
            Press,
            Move,
            StepNumberSelect,
            MosaicIntensity,
        };

        bool IsBound() const noexcept
        {
            return m_host != nullptr && m_annot != nullptr && m_toolMode != nullptr;
        }
        bool ArrangeMosaicEffectMenu(const ui::LayoutEnv& env) noexcept;
        bool ApplyLiveStepNumberEdit();
        void ApplyMosaicSliderPoint(POINT point, bool commitUndo);
        void SyncStepSlotLayout(int dpi) noexcept;

        IToolbarHost* m_host{};
        AnnotationController* m_annot{};
        const RenderDevice* m_device{};
        const ToolbarButton* m_toolMode{};
        Toolbar m_toolbar{};
        Toolbar m_dragStart{};
        POINT m_dragOrigin{};
        bool m_visible{};
        DragMode m_drag{ DragMode::None };
        ToolbarButton m_pressedButton{ ToolbarButton::None };
        SubToolbarAction m_pressedSubAction{ SubToolbarAction::None };

        bool m_editingStepNumber{};
        int m_stepNumberStart{ 1 };
        mutable ui::TextFieldSlot m_stepNumSlot{};
        ui::SingleLineInputState m_stepNumInput;
        ui::DropdownMenu m_mosaicEffectMenu;
    };
}
