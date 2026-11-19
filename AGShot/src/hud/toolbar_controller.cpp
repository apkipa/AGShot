#include "pch.h"

#include "hud/toolbar_controller.h"

#include "hud/color_model.h"
#include "hud/text_edit_utils.h"

namespace agshot
{
    ToolbarController::ToolbarController()
    {
        m_mosaicEffectMenu.SetItems({ L"Mosaic", L"Gaussian Blur" });
        m_mosaicEffectMenu.SetMinWidthDip(114);
    }

    void ToolbarController::Reset() noexcept
    {
        m_mosaicEffectMenu.Close();
        m_drag = DragMode::None;
        m_pressedButton = ToolbarButton::None;
        m_pressedSubAction = SubToolbarAction::None;
        m_editingStepNumber = false;
        m_stepNumInput.Clear();
        m_stepNumInput.ResetClicks();
        m_stepNumSlot.Clear();
        m_visible = false;
        m_toolbar.hovered = ToolbarButton::None;
        m_toolbar.hoveredSub = SubToolbarAction::None;
    }

    void ToolbarController::Hide() noexcept
    {
        m_visible = false;
        m_mosaicEffectMenu.Close();
        m_toolbar.hovered = ToolbarButton::None;
        m_toolbar.hoveredSub = SubToolbarAction::None;
    }

    void ToolbarController::InvalidateLayouts() noexcept
    {
        m_stepNumSlot.InvalidateLayout();
        m_mosaicEffectMenu.InvalidateLayouts();
    }

    bool ToolbarController::ClearHover() noexcept
    {
        const bool had = (m_toolbar.hovered != ToolbarButton::None)
            || (m_toolbar.hoveredSub != SubToolbarAction::None)
            || (m_mosaicEffectMenu.hoveredIndex() >= 0);
        m_toolbar.hovered = ToolbarButton::None;
        m_toolbar.hoveredSub = SubToolbarAction::None;
        m_mosaicEffectMenu.ClearHover();
        return had;
    }

    bool ToolbarController::ArrangeMosaicEffectMenu(const ui::LayoutEnv& env) noexcept
    {
        const RECT effectAnchor = MosaicEffectAnchorRect();
        if (IsRectEmpty(&effectAnchor))
        {
            m_mosaicEffectMenu.Close();
            return false;
        }
        const bool subAboveMain = m_toolbar.subVisible
            && (m_toolbar.subPanel.bottom <= m_toolbar.panel.top);
        m_mosaicEffectMenu.SetPreferBelow(!subAboveMain);
        m_mosaicEffectMenu.Arrange(effectAnchor, env);
        return true;
    }

    void ToolbarController::Settle(
        const ToolbarState& state, const RECT& selection, const ui::LayoutEnv& env)
    {
        m_mosaicEffectMenu.Close();
        m_editingStepNumber = false;
        m_stepNumInput.Clear();
        m_visible = LayoutToolbar(m_toolbar, state, selection, env.viewport, env.dpi);
        SyncStepSlotLayout(env.dpi);
    }

    void ToolbarController::LayoutSub(const ToolbarState& state, const ui::LayoutEnv& env)
    {
        if (m_visible)
        {
            LayoutSubToolbar(m_toolbar, state, env.viewport, env.dpi);
        }
        SyncStepSlotLayout(env.dpi);
        if (m_mosaicEffectMenu.isOpen() && ArrangeMosaicEffectMenu(env))
        {
            m_mosaicEffectMenu.SetSelectedIndex(
                state.activeStyle.mosaicEffect == AnnotationMosaicEffect::GaussianBlur ? 1 : 0);
        }
    }

    void ToolbarController::UpdateHover(POINT point) noexcept
    {
        if (!m_visible)
        {
            ClearHover();
            return;
        }
        const ToolbarHit hit = HitTestToolbarTarget(m_toolbar, point);
        m_toolbar.hovered = (hit.target == ToolbarHitTarget::Button && m_toolbar.isEnabled(hit.button))
            ? hit.button
            : ToolbarButton::None;
        m_toolbar.hoveredSub = (hit.target == ToolbarHitTarget::SubButton && m_toolbar.isSubEnabled(hit.subAction))
            ? hit.subAction
            : SubToolbarAction::None;
    }

    RECT ToolbarController::PickerAnchorRect(SubToolbarAction anchorAction) const noexcept
    {
        if (const SubToolbarItem* item = m_toolbar.findSubItem(anchorAction))
        {
            return item->rect;
        }
        return m_toolbar.subVisible2 ? m_toolbar.subPanel2 : m_toolbar.subPanel;
    }

    RECT ToolbarController::PickerAvoidBand() const noexcept
    {
        RECT avoid = m_toolbar.panel;
        if (m_toolbar.subVisible2 && m_toolbar.subVisible)
        {
            RECT combined{};
            UnionRect(&combined, &avoid, &m_toolbar.subPanel);
            return combined;
        }
        return avoid;
    }

    RECT ToolbarController::StepNumberItemRect() const noexcept
    {
        if (m_visible)
        {
            if (const SubToolbarItem* item = m_toolbar.findSubItem(SubToolbarAction::BadgeNumberValue))
            {
                return item->rect;
            }
        }
        return RECT{};
    }

    RECT ToolbarController::MosaicEffectAnchorRect() const noexcept
    {
        if (m_visible)
        {
            if (const SubToolbarItem* item = m_toolbar.findSubItem(SubToolbarAction::MosaicEffectDropdown))
            {
                const RECT& panel = (m_toolbar.subVisible2
                    && item->rect.top >= m_toolbar.subPanel2.top
                    && item->rect.bottom <= m_toolbar.subPanel2.bottom)
                    ? m_toolbar.subPanel2
                    : m_toolbar.subPanel;
                return RECT{ item->rect.left, panel.top, item->rect.right, panel.bottom };
            }
        }
        return RECT{};
    }

    void ToolbarController::SyncStepSlotLayout(int dpi) noexcept
    {
        const RECT stepBox = StepNumberItemRect();
        if (!IsRectEmpty(&stepBox))
        {
            m_stepNumSlot.Arrange(stepBox, dpi, L"#", nullptr);
        }
        else
        {
            m_stepNumSlot.Clear();
            if (m_editingStepNumber)
            {
                m_editingStepNumber = false;
                m_stepNumInput.Clear();
                if (m_annot != nullptr)
                {
                    m_annot->ClearLiveEditSnapshot();
                }
            }
        }
    }

    void ToolbarController::BeginStepNumberEdit(ui::Invalidate& inv)
    {
        if (!IsBound())
        {
            return;
        }
        const RECT box = StepNumberItemRect();
        if (IsRectEmpty(&box))
        {
            return;
        }
        m_editingStepNumber = true;
        m_stepNumberStart = m_annot->ActiveStepNumber(*m_toolMode);
        m_annot->BeginLiveEditSnapshot();
        m_stepNumSlot.Arrange(box, m_host->CurrentLayoutEnv().dpi, L"#", nullptr);
        m_stepNumInput.SetText(std::to_wstring(m_stepNumberStart), true);
        if (HWND hwnd = m_host->HostWindow())
        {
            SetFocus(hwnd);
        }
        inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
    }

    bool ToolbarController::ApplyLiveStepNumberEdit()
    {
        if (!IsBound())
        {
            return false;
        }
        int val = 0;
        if (!ParseUnsignedIntDigits(m_stepNumInput.text(), val) || val < 1)
        {
            return false;
        }
        return m_annot->SetActiveStepNumber(val, *m_toolMode, false);
    }

    void ToolbarController::CommitStepNumberEdit(ui::Invalidate& inv)
    {
        if (!m_editingStepNumber || !IsBound())
        {
            return;
        }
        int val = 0;
        if (ParseUnsignedIntDigits(m_stepNumInput.text(), val) && val >= 1)
        {
            m_annot->SetActiveStepNumber(val, *m_toolMode, true);
        }
        else
        {
            m_annot->SetActiveStepNumber(m_stepNumberStart, *m_toolMode, false);
            m_annot->ClearLiveEditSnapshot();
        }
        m_editingStepNumber = false;
        m_stepNumInput.Clear();
        m_stepNumInput.EndDrag();
        inv |= ui::Invalidate::Layout | ui::Invalidate::Ime | ui::Invalidate::Paint;
    }

    void ToolbarController::CancelStepNumberEdit(ui::Invalidate& inv)
    {
        if (!m_editingStepNumber || !IsBound())
        {
            return;
        }
        m_annot->SetActiveStepNumber(m_stepNumberStart, *m_toolMode, false);
        m_annot->ClearLiveEditSnapshot();
        m_editingStepNumber = false;
        m_stepNumInput.Clear();
        m_stepNumInput.EndDrag();
        inv |= ui::Invalidate::Ime | ui::Invalidate::Paint;
    }

    void ToolbarController::ToggleMosaicEffectMenu(ui::Invalidate& inv)
    {
        if (!IsBound())
        {
            return;
        }
        if (m_mosaicEffectMenu.isOpen())
        {
            m_mosaicEffectMenu.Close();
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            return;
        }
        m_host->DismissColorPicker(false, inv);
        const ui::LayoutEnv env = m_host->CurrentLayoutEnv();
        if (!ArrangeMosaicEffectMenu(env))
        {
            return;
        }
        const ToolbarState st = m_host->CurrentToolbarState();
        m_mosaicEffectMenu.SetSelectedIndex(
            st.activeStyle.mosaicEffect == AnnotationMosaicEffect::GaussianBlur ? 1 : 0);
        m_mosaicEffectMenu.Open();
        inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
    }

    void ToolbarController::ApplyMosaicSliderPoint(POINT point, bool commitUndo)
    {
        if (!IsBound())
        {
            return;
        }
        const SubToolbarItem* item = m_toolbar.findSubItem(SubToolbarAction::MosaicIntensitySlider);
        if (item == nullptr)
        {
            return;
        }
        ui::Slider1D slider;
        slider.Arrange(MosaicSliderTrackRect(item->rect, m_host->CurrentLayoutEnv().dpi));
        const float norm = slider.ValueFromPoint(point);
        m_annot->SetMosaicIntensityNormalized(norm, *m_toolMode, commitUndo);
    }

    void ToolbarController::Draw(
        HudRenderer& renderer, const RenderDevice& device,
        ToolbarButton activeMode, bool allowTooltip) const
    {
        if (!IsVisible() || !IsBound())
        {
            return;
        }
        ID2D1DeviceContext* context = device.d2dContext();
        renderer.DrawToolbar(device, m_toolbar, activeMode);
        if (!IsRectEmpty(&m_stepNumSlot.box))
        {
            m_stepNumSlot.hovered = (m_toolbar.hoveredSub == SubToolbarAction::BadgeNumberValue);
            const std::wstring stepText = m_editingStepNumber
                ? m_stepNumInput.text()
                : std::to_wstring(m_annot->ActiveStepNumber(*m_toolMode));
            m_stepNumInput.DrawSlot(
                context, device.dwriteFactory(), device.textFormat(),
                device.brushes(), device.dpi(), m_stepNumSlot,
                ScaleByDpi(4.0f, device.dpi()), m_editingStepNumber, stepText);
        }
        if (m_mosaicEffectMenu.isOpen())
        {
            m_mosaicEffectMenu.Draw(
                context, device.dwriteFactory(), device.textFormat(),
                device.brushes(), device.dpi());
        }
        if (allowTooltip && !m_mosaicEffectMenu.isOpen() && !m_editingStepNumber)
        {
            const ui::LayoutEnv env = m_host->CurrentLayoutEnv();
            renderer.DrawTooltip(
                device,
                static_cast<UINT>(env.viewport.right - env.viewport.left),
                static_cast<UINT>(env.viewport.bottom - env.viewport.top),
                m_toolbar);
        }
    }

    // -------------------------------------------------------------------------
    // ui::IHudLayer
    // -------------------------------------------------------------------------
    bool ToolbarController::IsVisible() const noexcept
    {
        return m_visible && (m_host == nullptr || !m_host->IsToolbarSuppressed());
    }

    bool ToolbarController::HitTest(POINT point) const
    {
        if (m_mosaicEffectMenu.isOpen() && m_mosaicEffectMenu.HitTest(point))
        {
            return true;
        }
        return HitTestToolbarTarget(m_toolbar, point).target != ToolbarHitTarget::None;
    }

    void ToolbarController::OnHoverMove(POINT point, ui::Invalidate& inv)
    {
        if (m_mosaicEffectMenu.isOpen())
        {
            if (m_mosaicEffectMenu.UpdateHover(point))
            {
                inv |= ui::Invalidate::Paint;
            }
            if (m_mosaicEffectMenu.HitTest(point))
            {
                if (m_toolbar.hovered != ToolbarButton::None
                    || m_toolbar.hoveredSub != SubToolbarAction::None)
                {
                    m_toolbar.hovered = ToolbarButton::None;
                    m_toolbar.hoveredSub = SubToolbarAction::None;
                    inv |= ui::Invalidate::Paint;
                }
                inv |= ui::Invalidate::Cursor;
                return;
            }
        }
        const ToolbarButton prevBtn = m_toolbar.hovered;
        const SubToolbarAction prevSub = m_toolbar.hoveredSub;
        UpdateHover(point);
        if (m_toolbar.hovered != prevBtn || m_toolbar.hoveredSub != prevSub)
        {
            inv |= ui::Invalidate::Paint;
        }
        inv |= ui::Invalidate::Cursor;
    }

    void ToolbarController::OnHoverLeave(ui::Invalidate& inv)
    {
        if (ClearHover())
        {
            inv |= ui::Invalidate::Paint;
        }
    }

    ui::OutsideClickPolicy ToolbarController::OnOutsidePointer(
        ui::PointerButton, POINT, const ui::IHudLayer*, ui::Invalidate& inv)
    {
        if (m_mosaicEffectMenu.isOpen())
        {
            m_mosaicEffectMenu.Close();
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            return ui::OutsideClickPolicy::DismissAndAbsorb;
        }
        if (m_editingStepNumber)
        {
            CommitStepNumberEdit(inv);
            return ui::OutsideClickPolicy::DismissAndPass;
        }
        return ui::OutsideClickPolicy::Ignore;
    }

    ui::EventOutcome ToolbarController::OnLeftDown(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        if (!IsBound())
        {
            return ui::EventOutcome::Ignored;
        }
        if (m_mosaicEffectMenu.isOpen())
        {
            int chosen = -1;
            if (m_mosaicEffectMenu.OnLeftDown(in.point, chosen))
            {
                if (chosen >= 0)
                {
                    const AnnotationMosaicEffect eff = (chosen == 1)
                        ? AnnotationMosaicEffect::GaussianBlur
                        : AnnotationMosaicEffect::Pixelate;
                    m_annot->SetMosaicEffect(eff, *m_toolMode);
                }
                else
                {
                    m_mosaicEffectMenu.Close();
                }
                inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
                return ui::EventOutcome::Handled;
            }
            const ToolbarHit preHit = HitTestToolbarTarget(m_toolbar, in.point);
            m_mosaicEffectMenu.Close();
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            if (preHit.target == ToolbarHitTarget::SubButton
                && preHit.subAction == SubToolbarAction::MosaicEffectDropdown)
            {
                return ui::EventOutcome::Handled;
            }
        }

        const ToolbarHit hit = HitTestToolbarTarget(m_toolbar, in.point);
        if (hit.target == ToolbarHitTarget::SubButton
            && hit.subAction == SubToolbarAction::MosaicIntensitySlider
            && m_toolbar.isSubEnabled(hit.subAction))
        {
            if (m_editingStepNumber)
            {
                CommitStepNumberEdit(inv);
            }
            m_host->DismissColorPicker(false, inv);
            m_annot->BeginLiveEditSnapshot();
            m_drag = DragMode::MosaicIntensity;
            m_pressedButton = ToolbarButton::None;
            m_pressedSubAction = SubToolbarAction::None;
            ApplyMosaicSliderPoint(in.point, false);
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            return ui::EventOutcome::Capture;
        }

        if (hit.target == ToolbarHitTarget::SubButton
            && hit.subAction == SubToolbarAction::BadgeNumberValue)
        {
            const bool alreadyFocused = m_editingStepNumber;
            if (!alreadyFocused)
            {
                BeginStepNumberEdit(inv);
            }
            IDWriteFactory* dwrite = (m_device != nullptr) ? m_device->dwriteFactory() : nullptr;
            IDWriteTextFormat* textFmt = (m_device != nullptr) ? m_device->textFormat() : nullptr;
            const bool draggingSel = m_stepNumInput.OnLeftButtonDown(
                in.point, alreadyFocused, m_stepNumSlot, dwrite, textFmt, in.mods);
            inv |= ui::Invalidate::Paint;
            if (draggingSel)
            {
                m_drag = DragMode::StepNumberSelect;
                return ui::EventOutcome::Capture;
            }
            return ui::EventOutcome::Handled;
        }

        if (m_editingStepNumber)
        {
            CommitStepNumberEdit(inv);
        }

        switch (hit.target)
        {
        case ToolbarHitTarget::Button:
            if (m_toolbar.isEnabled(hit.button))
            {
                m_drag = DragMode::Press;
                m_pressedButton = hit.button;
                m_pressedSubAction = SubToolbarAction::None;
                inv |= ui::Invalidate::Paint;
                return ui::EventOutcome::Capture;
            }
            inv |= ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;

        case ToolbarHitTarget::SubButton:
            if (m_toolbar.isSubEnabled(hit.subAction))
            {
                m_drag = DragMode::Press;
                m_pressedButton = ToolbarButton::None;
                m_pressedSubAction = hit.subAction;
                inv |= ui::Invalidate::Paint;
                return ui::EventOutcome::Capture;
            }
            inv |= ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;

        case ToolbarHitTarget::Handle:
            m_drag = DragMode::Move;
            m_dragOrigin = in.point;
            m_dragStart = m_toolbar;
            m_toolbar.hovered = ToolbarButton::None;
            m_toolbar.hoveredSub = SubToolbarAction::None;
            inv |= ui::Invalidate::Paint;
            return ui::EventOutcome::Capture;

        case ToolbarHitTarget::Panel:
        case ToolbarHitTarget::SubPanel:
            inv |= ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;

        case ToolbarHitTarget::None:
        default:
            return ui::EventOutcome::Ignored;
        }
    }

    void ToolbarController::OnCapturedMove(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        if (!IsBound())
        {
            return;
        }
        if (m_drag == DragMode::MosaicIntensity)
        {
            ApplyMosaicSliderPoint(in.point, false);
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            return;
        }
        if (m_drag == DragMode::StepNumberSelect)
        {
            IDWriteFactory* dwrite = (m_device != nullptr) ? m_device->dwriteFactory() : nullptr;
            IDWriteTextFormat* textFmt = (m_device != nullptr) ? m_device->textFormat() : nullptr;
            m_stepNumInput.OnMouseMove(in.point, m_stepNumSlot, dwrite, textFmt);
            inv |= ui::Invalidate::Paint;
            return;
        }
        if (m_drag == DragMode::Press)
        {
            const ToolbarHit h = HitTestToolbarTarget(m_toolbar, in.point);
            if (m_pressedButton != ToolbarButton::None)
            {
                m_toolbar.hovered =
                    (h.target == ToolbarHitTarget::Button && h.button == m_pressedButton)
                    ? m_pressedButton
                    : ToolbarButton::None;
                m_toolbar.hoveredSub = SubToolbarAction::None;
            }
            else if (m_pressedSubAction != SubToolbarAction::None)
            {
                m_toolbar.hovered = ToolbarButton::None;
                m_toolbar.hoveredSub =
                    (h.target == ToolbarHitTarget::SubButton && h.subAction == m_pressedSubAction)
                    ? m_pressedSubAction
                    : SubToolbarAction::None;
            }
            inv |= ui::Invalidate::Paint;
            return;
        }
        if (m_drag == DragMode::Move)
        {
            const ui::LayoutEnv env = m_host->CurrentLayoutEnv();
            DragToolbar(m_toolbar, m_dragStart, m_dragOrigin, in.point, env.viewport);
            m_toolbar.hovered = ToolbarButton::None;
            m_toolbar.hoveredSub = SubToolbarAction::None;
            m_host->OnToolbarMoved();
            if (m_mosaicEffectMenu.isOpen())
            {
                ArrangeMosaicEffectMenu(env);
            }
            const RECT stepBox = StepNumberItemRect();
            if (!IsRectEmpty(&stepBox))
            {
                m_stepNumSlot.Arrange(stepBox, env.dpi, L"#", nullptr);
            }
            inv |= ui::Invalidate::Paint;
        }
    }

    void ToolbarController::OnCapturedUp(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        const DragMode drag = m_drag;
        const ToolbarButton pressedBtn = m_pressedButton;
        const SubToolbarAction pressedSub = m_pressedSubAction;
        m_drag = DragMode::None;
        m_pressedButton = ToolbarButton::None;
        m_pressedSubAction = SubToolbarAction::None;

        if (m_host == nullptr)
        {
            return;
        }
        if (drag == DragMode::MosaicIntensity)
        {
            ApplyMosaicSliderPoint(in.point, true);
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            return;
        }
        if (drag == DragMode::StepNumberSelect)
        {
            m_stepNumInput.EndDrag();
            inv |= ui::Invalidate::Paint;
            return;
        }
        if (drag == DragMode::Press)
        {
            const ToolbarHit hit = m_visible
                ? HitTestToolbarTarget(m_toolbar, in.point)
                : ToolbarHit{};
            if (pressedBtn != ToolbarButton::None)
            {
                if (hit.target == ToolbarHitTarget::Button && hit.button == pressedBtn
                    && m_toolbar.isEnabled(pressedBtn))
                {
                    m_host->OnToolbarButtonInvoked(pressedBtn, inv);
                    return;
                }
            }
            else if (pressedSub != SubToolbarAction::None)
            {
                if (hit.target == ToolbarHitTarget::SubButton && hit.subAction == pressedSub
                    && m_toolbar.isSubEnabled(pressedSub))
                {
                    if (pressedSub == SubToolbarAction::MosaicEffectDropdown)
                    {
                        ToggleMosaicEffectMenu(inv);
                    }
                    else
                    {
                        m_mosaicEffectMenu.Close();
                        m_host->OnSubToolbarActionInvoked(pressedSub, inv);
                    }
                    return;
                }
            }
        }
        if (in.kind == ui::PointerKind::Touch)
        {
            ClearHover();
        }
        else
        {
            UpdateHover(in.point);
        }
        inv |= ui::Invalidate::Paint;
    }

    void ToolbarController::OnCancelCapture(ui::Invalidate& inv)
    {
        const DragMode drag = m_drag;
        m_drag = DragMode::None;
        m_pressedButton = ToolbarButton::None;
        m_pressedSubAction = SubToolbarAction::None;
        m_stepNumInput.EndDrag();
        if (drag == DragMode::MosaicIntensity && m_annot != nullptr)
        {
            m_annot->CommitLiveEdit();
            inv |= ui::Invalidate::Layout;
        }
        inv |= ui::Invalidate::Paint;
    }

    ui::EventOutcome ToolbarController::OnRightUp(const ui::PointerInput&, ui::Invalidate&)
    {
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome ToolbarController::OnWheel(
        const ui::PointerInput& in, ui::Invalidate& inv)
    {
        if (!IsBound())
        {
            return ui::EventOutcome::Handled;
        }
        const ToolbarHit th = HitTestToolbarTarget(m_toolbar, in.point);
        if (th.target == ToolbarHitTarget::SubButton
            && m_toolbar.isSubEnabled(th.subAction)
            && m_annot->OnSubToolbarWheel(th.subAction, in.wheelDelta, *m_toolMode))
        {
            if (th.subAction == SubToolbarAction::BadgeNumberValue && m_editingStepNumber)
            {
                m_stepNumberStart = m_annot->ActiveStepNumber(*m_toolMode);
                m_stepNumInput.SetText(std::to_wstring(m_stepNumberStart), false);
            }
            if (th.subAction == SubToolbarAction::MosaicEffectDropdown && m_mosaicEffectMenu.isOpen())
            {
                const ToolbarState st = m_host->CurrentToolbarState();
                m_mosaicEffectMenu.SetSelectedIndex(
                    st.activeStyle.mosaicEffect == AnnotationMosaicEffect::GaussianBlur ? 1 : 0);
            }
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
        }
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome ToolbarController::OnKeyDown(
        WPARAM key, KeyModifiers mods, ui::Invalidate& inv)
    {
        if (!IsBound())
        {
            return ui::EventOutcome::Ignored;
        }
        if (m_mosaicEffectMenu.isOpen())
        {
            int chosen = -1;
            if (!m_mosaicEffectMenu.OnKeyDown(key, chosen))
            {
                return ui::EventOutcome::Ignored;
            }
            if (chosen >= 0)
            {
                const AnnotationMosaicEffect eff = (chosen == 1)
                    ? AnnotationMosaicEffect::GaussianBlur
                    : AnnotationMosaicEffect::Pixelate;
                m_annot->SetMosaicEffect(eff, *m_toolMode);
            }
            inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;
        }
        if (!m_editingStepNumber)
        {
            return ui::EventOutcome::Ignored;
        }
        if (key == VK_ESCAPE)
        {
            CancelStepNumberEdit(inv);
            return ui::EventOutcome::Handled;
        }
        if (key == VK_RETURN)
        {
            CommitStepNumberEdit(inv);
            return ui::EventOutcome::Handled;
        }
        if (key == VK_UP || key == VK_DOWN)
        {
            const int step = (mods.shift ? 10 : 1) * (key == VK_UP ? 1 : -1);
            if (m_annot->StepActiveStepNumber(step, *m_toolMode))
            {
                m_stepNumberStart = m_annot->ActiveStepNumber(*m_toolMode);
                m_stepNumInput.SetText(std::to_wstring(m_stepNumberStart), false);
                inv |= ui::Invalidate::Layout | ui::Invalidate::Paint;
            }
            return ui::EventOutcome::Handled;
        }
        if (mods.ctrl && (key == 'C' || key == 'X'))
        {
            if (m_stepNumInput.HasSelection())
            {
                const auto [s, e] = m_stepNumInput.SelectionRange();
                CopyTextToClipboard(m_host->HostWindow(), m_stepNumInput.text().substr(s, e - s));
                if (key == 'X')
                {
                    m_stepNumInput.ReplaceSelectionBounded({}, 3, false);
                    ApplyLiveStepNumberEdit();
                    inv |= ui::Invalidate::Paint;
                }
            }
            return ui::EventOutcome::Handled;
        }
        if (mods.ctrl && key == 'V')
        {
            const std::wstring filtered = SanitizeDigitsClipboardText(
                ReadTextFromClipboard(m_host->HostWindow(), false));
            if (!filtered.empty())
            {
                m_stepNumInput.ReplaceSelectionBounded(filtered, 3, false);
                ApplyLiveStepNumberEdit();
                inv |= ui::Invalidate::Paint;
            }
            return ui::EventOutcome::Handled;
        }

        bool mutated = false;
        if (m_stepNumInput.HandleEditingKey(key, mods, mutated))
        {
            if (mutated)
            {
                ApplyLiveStepNumberEdit();
            }
            inv |= ui::Invalidate::Paint;
            return ui::EventOutcome::Handled;
        }
        return ui::EventOutcome::Handled;
    }

    ui::EventOutcome ToolbarController::OnChar(wchar_t ch, ui::Invalidate& inv)
    {
        if (!m_editingStepNumber)
        {
            return ui::EventOutcome::Ignored;
        }
        if (ch >= L'0' && ch <= L'9')
        {
            m_stepNumInput.ReplaceSelectionBounded(std::wstring_view(&ch, 1), 3, true);
            ApplyLiveStepNumberEdit();
            inv |= ui::Invalidate::Paint;
        }
        return ui::EventOutcome::Handled;
    }

    std::optional<LPCWSTR> ToolbarController::CursorForPoint(POINT point, bool captured) const
    {
        if (captured)
        {
            if (m_drag == DragMode::StepNumberSelect)
            {
                return IDC_IBEAM;
            }
            return (m_drag == DragMode::Move) ? IDC_SIZEALL : IDC_HAND;
        }
        if (m_mosaicEffectMenu.isOpen() && m_mosaicEffectMenu.HitTest(point))
        {
            return m_mosaicEffectMenu.CursorForPoint(point);
        }
        const ToolbarHit hit = HitTestToolbarTarget(m_toolbar, point);
        switch (hit.target)
        {
        case ToolbarHitTarget::Handle:    return IDC_SIZEALL;
        case ToolbarHitTarget::Button:    return m_toolbar.isEnabled(hit.button) ? IDC_HAND : IDC_ARROW;
        case ToolbarHitTarget::SubButton:
            if (!m_toolbar.isSubEnabled(hit.subAction))
            {
                return IDC_ARROW;
            }
            return (hit.subAction == SubToolbarAction::BadgeNumberValue) ? IDC_IBEAM : IDC_HAND;
        case ToolbarHitTarget::Panel:
        case ToolbarHitTarget::SubPanel:  return IDC_ARROW;
        case ToolbarHitTarget::None:
        default:                          return std::nullopt;
        }
    }

    ui::ImePolicy ToolbarController::QueryImePolicy() const noexcept
    {
        return m_editingStepNumber ? ui::ImePolicy::Disable : ui::ImePolicy::Pass;
    }
}
