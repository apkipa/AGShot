#include "pch.h"

#include "core/error.h"
#include "hud/annotation_controller.h"
#include "hud/color_model.h"

namespace agshot
{
    namespace
    {
        float ValidRestoreAlpha(float candidate) noexcept
        {
            return (candidate >= kVisibleAlphaThreshold) ? std::clamp(candidate, 0.01f, 1.0f) : 1.0f;
        }

        bool LiveEditableStateEquals(const AnnotationItem& a, const AnnotationItem& b) noexcept
        {
            return a.id == b.id
                && a.stepNumber == b.stepNumber
                && SameRgbaColor(a.style.color, b.style.color, 1e-4f)
                && SameRgbaColor(a.style.fillColor, b.style.fillColor, 1e-4f)
                && a.style.strokeStyle == b.style.strokeStyle
                && a.style.mosaicBlockDip == b.style.mosaicBlockDip
                && std::fabs(a.style.mosaicBlurRadiusDip - b.style.mosaicBlurRadiusDip) <= 0.01f;
        }

        bool ApplySubActionToStyle(SubToolbarAction action, AnnotationStyle& style,
                                   bool isShape, bool mosaicBrushWidth,
                                   bool includeWhitePalette) noexcept
        {
            switch (action)
            {
            case SubToolbarAction::ShapeRect:
                style.shapeKind = AnnotationShapeKind::Rectangle;
                return true;
            case SubToolbarAction::ShapeEllipse:
                style.shapeKind = AnnotationShapeKind::Ellipse;
                return true;
            case SubToolbarAction::ShapeDiamond:
                style.shapeKind = AnnotationShapeKind::Diamond;
                return true;
            case SubToolbarAction::LineArrow:
                style.lineKind = AnnotationLineKind::Arrow;
                return true;
            case SubToolbarAction::LineStraight:
                style.lineKind = AnnotationLineKind::Line;
                return true;
            case SubToolbarAction::MosaicBrush:
                style.mosaicKind = AnnotationMosaicKind::Brush;
                return true;
            case SubToolbarAction::MosaicRect:
                style.mosaicKind = AnnotationMosaicKind::Rectangle;
                return true;
            case SubToolbarAction::BrushHighlighterToggle:
                style.brushHighlighter = !style.brushHighlighter;
                return true;
            case SubToolbarAction::Color0:
            case SubToolbarAction::Color1:
            case SubToolbarAction::Color2:
            case SubToolbarAction::Color3:
            case SubToolbarAction::Color4:
            case SubToolbarAction::Color5:
            {
                const std::size_t idx = static_cast<std::size_t>(action)
                    - static_cast<std::size_t>(SubToolbarAction::Color0);
                style.color = includeWhitePalette
                    ? kAnnotationPalette[idx]
                    : kStrokeQuickPalette[std::min<std::size_t>(idx, kStrokeQuickPalette.size() - 1)];
                style.lastStrokeAlpha = style.color.a;
                return true;
            }
            case SubToolbarAction::FillModeNone:
                if (!isShape) return false;
                if (HasVisibleColor(style.fillColor))
                {
                    style.lastFillAlpha = style.fillColor.a;
                }
                style.fillColor.a = 0.0f;
                return true;
            case SubToolbarAction::FillModeSolid:
                if (!isShape) return false;
                if (!HasVisibleColor(style.fillColor))
                {
                    style.fillColor.a = ValidRestoreAlpha(style.lastFillAlpha);
                }
                style.fillPattern = AnnotationFillPattern::Solid;
                return true;
            case SubToolbarAction::FillPatternCrossHatch:
                if (!isShape) return false;
                if (!HasVisibleColor(style.fillColor))
                {
                    style.fillColor.a = ValidRestoreAlpha(style.lastFillAlpha);
                }
                style.fillPattern = AnnotationFillPattern::CrossHatch;
                return true;
            case SubToolbarAction::FillPatternDiagHatch:
                if (!isShape) return false;
                if (!HasVisibleColor(style.fillColor))
                {
                    style.fillColor.a = ValidRestoreAlpha(style.lastFillAlpha);
                }
                style.fillPattern = AnnotationFillPattern::DiagonalHatch;
                return true;
            case SubToolbarAction::CornerRadiusCycle:
            {
                if (!isShape || style.shapeKind == AnnotationShapeKind::Ellipse)
                {
                    return false;
                }
                constexpr std::array<float, 4> kRadii = { 0.0f, 6.0f, 12.0f, 20.0f };
                const std::size_t rIdx = ClosestValueIndex(kRadii, style.cornerRadiusDip);
                style.cornerRadiusDip = kRadii[(rIdx + 1) % 4];
                return true;
            }
            case SubToolbarAction::Width0:
            case SubToolbarAction::Width1:
            case SubToolbarAction::Width2:
            case SubToolbarAction::Width3:
            {
                const std::size_t idx = static_cast<std::size_t>(action)
                    - static_cast<std::size_t>(SubToolbarAction::Width0);
                if (mosaicBrushWidth)
                {
                    style.mosaicBrushDip = kAnnotationMosaicBrushDip[idx];
                }
                else
                {
                    style.strokeWidthDip = kAnnotationStrokeWidthsDip[idx];
                }
                return true;
            }
            case SubToolbarAction::Font0:
            case SubToolbarAction::Font1:
            case SubToolbarAction::Font2:
            case SubToolbarAction::Font3:
            {
                const std::size_t idx = static_cast<std::size_t>(action)
                    - static_cast<std::size_t>(SubToolbarAction::Font0);
                style.fontSizeDip = kAnnotationFontSizesDip[idx];
                return true;
            }
            case SubToolbarAction::BadgeShapeOutlineCircle:
                style.badgeShape = AnnotationBadgeShape::OutlineCircle;
                return true;
            case SubToolbarAction::BadgeShapeFilledCircle:
                style.badgeShape = AnnotationBadgeShape::FilledCircle;
                return true;
            case SubToolbarAction::BadgeShapeOutlineSquare:
                style.badgeShape = AnnotationBadgeShape::OutlineSquare;
                return true;
            case SubToolbarAction::BadgeShapeFilledSquare:
                style.badgeShape = AnnotationBadgeShape::FilledSquare;
                return true;
            case SubToolbarAction::BadgeNumArabic:
                style.badgeNumberStyle = AnnotationBadgeNumberStyle::Arabic;
                return true;
            case SubToolbarAction::BadgeNumRoman:
                style.badgeNumberStyle = AnnotationBadgeNumberStyle::Roman;
                return true;
            case SubToolbarAction::BadgeNumLowerAlpha:
                style.badgeNumberStyle = AnnotationBadgeNumberStyle::LowerAlpha;
                return true;
            case SubToolbarAction::BadgeNumUpperAlpha:
                style.badgeNumberStyle = AnnotationBadgeNumberStyle::UpperAlpha;
                return true;
            case SubToolbarAction::BadgeNumChinese:
                style.badgeNumberStyle = AnnotationBadgeNumberStyle::Chinese;
                return true;
            case SubToolbarAction::BadgeSize0:
            case SubToolbarAction::BadgeSize1:
            case SubToolbarAction::BadgeSize2:
            case SubToolbarAction::BadgeSize3:
            {
                const std::size_t idx = static_cast<std::size_t>(action)
                    - static_cast<std::size_t>(SubToolbarAction::BadgeSize0);
                style.badgeRadiusDip = kAnnotationBadgeRadiiDip[idx];
                return true;
            }
            default:
                return false;
            }
        }
    }

    AnnotationController::AnnotationController()
    {
        for (AnnotationStyle& s : m_toolStyles)
        {
            s = AnnotationStyle{};
        }
        m_toolStyles[static_cast<std::size_t>(AnnotationKind::Mosaic)].mosaicKind = AnnotationMosaicKind::Brush;
    }

    void AnnotationController::ResetSession(HWND window) noexcept
    {
        m_textEditor.Cancel(window);
        m_doc.Clear();
        ClearTransientDragState();
        m_liveEditSnapshot.clear();
        m_nextStepNumberOverride.reset();
        m_requestStepNumberEdit = false;
        m_subWheelAccum.Reset();
    }

    void AnnotationController::ClearTransientDragState() noexcept
    {
        m_interaction = InteractionKind::None;
        m_hasDraft = false;
        m_draft = AnnotationItem{};
        m_dragHandle = AnnotationHandle::None;
        m_dragPrimaryId = 0;
        m_dragMultiStart.clear();
        m_snapshotBeforeTransform.clear();
        m_marqueeBaseIds.clear();
        m_snapGuidesX.clear();
        m_snapGuidesY.clear();
        m_textEditor.OnLeftButtonUp();
    }

    AnnotationStyle& AnnotationController::ToolStyle(AnnotationKind kind) noexcept
    {
        const std::size_t idx = std::min<std::size_t>(
            static_cast<std::size_t>(kind), m_toolStyles.size() - 1);
        return m_toolStyles[idx];
    }

    const AnnotationStyle& AnnotationController::ToolStyle(AnnotationKind kind) const noexcept
    {
        const std::size_t idx = std::min<std::size_t>(
            static_cast<std::size_t>(kind), m_toolStyles.size() - 1);
        return m_toolStyles[idx];
    }

    std::optional<AnnotationKind> AnnotationController::SelectedCommonKind() const noexcept
    {
        const auto& ids = m_doc.selectedIds();
        if (ids.empty())
        {
            return std::nullopt;
        }
        const AnnotationItem* first = m_doc.FindItem(ids.front());
        if (first == nullptr)
        {
            return std::nullopt;
        }
        const AnnotationKind kind = first->kind;
        for (std::size_t i = 1; i < ids.size(); ++i)
        {
            const AnnotationItem* item = m_doc.FindItem(ids[i]);
            if (item == nullptr || item->kind != kind)
            {
                return std::nullopt;
            }
        }
        return kind;
    }

    bool AnnotationController::SelectionMatchesTool(ToolbarButton toolMode) const noexcept
    {
        if (m_doc.selectionCount() == 0)
        {
            return false;
        }
        if (toolMode == ToolbarButton::SelectObject)
        {
            return true;
        }
        const std::optional<AnnotationKind> toolKind = ToolAnnotationKind(toolMode);
        const std::optional<AnnotationKind> commonKind = SelectedCommonKind();
        return toolKind.has_value() && commonKind.has_value() && (*toolKind == *commonKind);
    }

    void AnnotationController::PopulateToolbarState(
        ToolbarState& state, ToolbarButton toolMode) const noexcept
    {
        state.canUndo = m_doc.canUndo();
        state.canRedo = m_doc.canRedo();
        state.canBringForward = m_doc.CanBringSelectedForward();
        state.canSendBackward = m_doc.CanSendSelectedBackward();
        state.selectedCount = m_doc.selectionCount();
        state.selectedKind = SelectedCommonKind();
        state.activeStepNumber = ActiveStepNumber(toolMode);

        if (m_textEditor.active())
        {
            state.activeStyle = m_textEditor.item().style;
            return;
        }
        if (SelectionMatchesTool(toolMode))
        {
            if (const AnnotationItem* sel = m_doc.SelectedItem())
            {
                state.activeStyle = sel->style;
                return;
            }
        }
        if (const std::optional<AnnotationKind> toolKind = ToolAnnotationKind(toolMode))
        {
            state.activeStyle = ToolStyle(*toolKind);
            return;
        }
        state.activeStyle = ToolStyle(AnnotationKind::Shape);
    }

    LPCWSTR AnnotationController::CursorForActiveEditor(
        POINT point, IDWriteFactory* dwrite, int dpi) const
    {
        if (m_interaction == InteractionKind::TextEditor || m_textEditor.active())
        {
            switch (m_textEditor.HitTestZone(point, dwrite, dpi))
            {
            case TextEditorZone::Border:
                return IDC_SIZEALL;
            case TextEditorZone::InsideText:
                return IDC_IBEAM;
            case TextEditorZone::Outside:
            default:
                return IDC_ARROW;
            }
        }
        return IDC_ARROW;
    }

    LPCWSTR AnnotationController::CursorForSelectedHandle(
        POINT point, ToolbarButton toolMode, IDWriteFactory* dwrite, int dpi) const
    {
        if (m_interaction == InteractionKind::Transform)
        {
            return CursorForAnnotationHandle(m_dragHandle);
        }
        if (m_doc.selectionCount() == 1 && toolMode != ToolbarButton::EditSelection)
        {
            if (const AnnotationItem* sel = m_doc.SelectedItem())
            {
                const AnnotationHandle h = HitTestAnnotationHandle(
                    *sel, point, dwrite, dpi, ScaleByDpi(8, dpi));
                if (h != AnnotationHandle::None && h != AnnotationHandle::Body)
                {
                    return CursorForAnnotationHandle(h);
                }
            }
        }
        return nullptr;
    }

    LPCWSTR AnnotationController::CursorForCanvasPoint(
        POINT point, ToolbarButton toolMode, TransformModifiers mods,
        IDWriteFactory* dwrite, int dpi) const
    {
        if (toolMode == ToolbarButton::SelectObject || mods.ctrl)
        {
            if (m_doc.HitTestTopmost(point, dwrite, dpi) != 0)
            {
                return IDC_SIZEALL;
            }
            return IDC_ARROW;
        }
        if (toolMode == ToolbarButton::Text)
        {
            return IDC_IBEAM;
        }
        if (!m_doc.selectedIds().empty() && SelectionMatchesTool(toolMode))
        {
            for (std::uint64_t id : m_doc.selectedIds())
            {
                if (const AnnotationItem* sel = m_doc.FindItem(id);
                    sel != nullptr && HitTestAnnotationBody(*sel, point, dwrite, dpi))
                {
                    return IDC_SIZEALL;
                }
            }
        }
        return IDC_CROSS;
    }

    bool AnnotationController::InvokeSubAction(
        SubToolbarAction action, ToolbarButton toolMode,
        IDWriteFactory* dwrite, int dpi)
    {
        if (action == SubToolbarAction::DeleteSelected)
        {
            return m_doc.DeleteSelected();
        }
        if (action == SubToolbarAction::DuplicateSelected)
        {
            const float offset = ScaleByDpi(14.0f, dpi);
            return m_doc.DuplicateSelected(offset, offset);
        }
        if (action == SubToolbarAction::OrderSendToBack)
        {
            return m_doc.ReorderSelected(AnnotationZOrderOp::SendToBack);
        }
        if (action == SubToolbarAction::OrderSendBackward)
        {
            return m_doc.ReorderSelected(AnnotationZOrderOp::SendBackward);
        }
        if (action == SubToolbarAction::OrderBringForward)
        {
            return m_doc.ReorderSelected(AnnotationZOrderOp::BringForward);
        }
        if (action == SubToolbarAction::OrderBringToFront)
        {
            return m_doc.ReorderSelected(AnnotationZOrderOp::BringToFront);
        }

        if (action == SubToolbarAction::AlignLeft
            || action == SubToolbarAction::AlignCenterH
            || action == SubToolbarAction::AlignRight
            || action == SubToolbarAction::AlignTop
            || action == SubToolbarAction::AlignCenterV
            || action == SubToolbarAction::AlignBottom
            || action == SubToolbarAction::DistributeH
            || action == SubToolbarAction::DistributeV)
        {
            struct Entry
            {
                std::uint64_t id{};
                D2D1_RECT_F bounds{};
            };
            std::vector<Entry> entries;
            entries.reserve(m_doc.selectedIds().size());
            for (std::uint64_t id : m_doc.selectedIds())
            {
                if (const AnnotationItem* item = m_doc.FindItem(id))
                {
                    entries.push_back(Entry{ id, AnnotationBounds(*item, dwrite, dpi) });
                }
            }
            const bool isDistribute =
                (action == SubToolbarAction::DistributeH || action == SubToolbarAction::DistributeV);
            if (entries.size() < (isDistribute ? 3u : 2u))
            {
                return false;
            }

            std::vector<AnnotationItem> snapshot = m_doc.items();
            bool movedAny = false;
            constexpr float kEps = 0.05f;

            if (!isDistribute)
            {
                float minL = entries[0].bounds.left;
                float minT = entries[0].bounds.top;
                float maxR = entries[0].bounds.right;
                float maxB = entries[0].bounds.bottom;
                for (std::size_t i = 1; i < entries.size(); ++i)
                {
                    minL = std::min(minL, entries[i].bounds.left);
                    minT = std::min(minT, entries[i].bounds.top);
                    maxR = std::max(maxR, entries[i].bounds.right);
                    maxB = std::max(maxB, entries[i].bounds.bottom);
                }
                const float cx = (minL + maxR) * 0.5f;
                const float cy = (minT + maxB) * 0.5f;

                for (const Entry& e : entries)
                {
                    float dx = 0.0f;
                    float dy = 0.0f;
                    switch (action)
                    {
                    case SubToolbarAction::AlignLeft:
                        dx = minL - e.bounds.left;
                        break;
                    case SubToolbarAction::AlignCenterH:
                        dx = cx - (e.bounds.left + e.bounds.right) * 0.5f;
                        break;
                    case SubToolbarAction::AlignRight:
                        dx = maxR - e.bounds.right;
                        break;
                    case SubToolbarAction::AlignTop:
                        dy = minT - e.bounds.top;
                        break;
                    case SubToolbarAction::AlignCenterV:
                        dy = cy - (e.bounds.top + e.bounds.bottom) * 0.5f;
                        break;
                    case SubToolbarAction::AlignBottom:
                        dy = maxB - e.bounds.bottom;
                        break;
                    default:
                        break;
                    }
                    if (std::fabs(dx) > kEps || std::fabs(dy) > kEps)
                    {
                        if (AnnotationItem* item = m_doc.FindItem(e.id))
                        {
                            MoveAnnotation(*item, dx, dy);
                            movedAny = true;
                        }
                    }
                }
            }
            else
            {
                const bool horiz = (action == SubToolbarAction::DistributeH);
                const auto axisStart = [horiz](const D2D1_RECT_F& r) noexcept { return horiz ? r.left : r.top; };
                const auto axisEnd = [horiz](const D2D1_RECT_F& r) noexcept { return horiz ? r.right : r.bottom; };

                std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b)
                {
                    const float ca = (axisStart(a.bounds) + axisEnd(a.bounds)) * 0.5f;
                    const float cb = (axisStart(b.bounds) + axisEnd(b.bounds)) * 0.5f;
                    if (std::fabs(ca - cb) > 0.01f) return ca < cb;
                    if (std::fabs(axisStart(a.bounds) - axisStart(b.bounds)) > 0.01f)
                    {
                        return axisStart(a.bounds) < axisStart(b.bounds);
                    }
                    return a.id < b.id;
                });
                float sumSize = 0.0f;
                for (const Entry& e : entries)
                {
                    sumSize += (axisEnd(e.bounds) - axisStart(e.bounds));
                }
                const float span = axisEnd(entries.back().bounds) - axisStart(entries.front().bounds);
                const float gap = (span - sumSize) / static_cast<float>(entries.size() - 1);
                float cursor = axisEnd(entries.front().bounds) + gap;
                for (std::size_t i = 1; i + 1 < entries.size(); ++i)
                {
                    const float delta = cursor - axisStart(entries[i].bounds);
                    if (std::fabs(delta) > kEps)
                    {
                        if (AnnotationItem* item = m_doc.FindItem(entries[i].id))
                        {
                            MoveAnnotation(*item, horiz ? delta : 0.0f, horiz ? 0.0f : delta);
                            movedAny = true;
                        }
                    }
                    cursor += (axisEnd(entries[i].bounds) - axisStart(entries[i].bounds)) + gap;
                }
            }

            if (movedAny)
            {
                m_doc.CommitPreviousState(std::move(snapshot));
            }
            return movedAny;
        }

        const std::optional<AnnotationKind> toolKind = ToolAnnotationKind(toolMode);

        if (action == SubToolbarAction::MosaicBrush || action == SubToolbarAction::MosaicRect)
        {
            m_doc.ClearSelection();
            return ApplySubActionToStyle(action, ToolStyle(AnnotationKind::Mosaic), false, false, false);
        }

        const bool isKindSwitch = (action == SubToolbarAction::ShapeRect
            || action == SubToolbarAction::ShapeEllipse
            || action == SubToolbarAction::ShapeDiamond
            || action == SubToolbarAction::LineArrow
            || action == SubToolbarAction::LineStraight);
        if (toolMode != ToolbarButton::SelectObject && isKindSwitch && toolKind.has_value())
        {
            m_doc.ClearSelection();
            AnnotationStyle& ts = ToolStyle(*toolKind);
            return ApplySubActionToStyle(action, ts, *toolKind == AnnotationKind::Shape, false, false);
        }

        if (m_textEditor.active())
        {
            AnnotationStyle updated = m_textEditor.item().style;
            if (ApplySubActionToStyle(action, updated, false, false, true))
            {
                ApplySubActionToStyle(action, ToolStyle(AnnotationKind::Text), false, false, true);
                m_textEditor.UpdateStyle(updated);
                return true;
            }
            return false;
        }

        if (SelectionMatchesTool(toolMode))
        {
            const std::optional<AnnotationKind> selKind = SelectedCommonKind();
            if (selKind.has_value())
            {
                const bool isShape = (*selKind == AnnotationKind::Shape);
                const bool isText = (*selKind == AnnotationKind::Text);
                const bool isMosaicBrush = (*selKind == AnnotationKind::Mosaic)
                    && (m_doc.SelectedItem() != nullptr
                        && m_doc.SelectedItem()->style.mosaicKind == AnnotationMosaicKind::Brush);
                AnnotationStyle targetStyle = m_doc.SelectedItem()->style;
                if (ApplySubActionToStyle(action, targetStyle, isShape, isMosaicBrush, isText))
                {
                    std::vector<AnnotationItem> snapshot = m_doc.items();
                    for (std::uint64_t id : m_doc.selectedIds())
                    {
                        if (AnnotationItem* item = m_doc.FindItem(id))
                        {
                            ApplySubActionToStyle(action, item->style, isShape,
                                item->kind == AnnotationKind::Mosaic
                                && item->style.mosaicKind == AnnotationMosaicKind::Brush,
                                isText);
                        }
                    }
                    m_doc.CommitPreviousState(std::move(snapshot));
                    ApplySubActionToStyle(action, ToolStyle(*selKind), isShape, isMosaicBrush, isText);
                    return true;
                }
                return false;
            }
        }

        if (!toolKind.has_value())
        {
            return false;
        }
        const bool isShape = (*toolKind == AnnotationKind::Shape);
        const bool isText = (*toolKind == AnnotationKind::Text);
        AnnotationStyle& ts = ToolStyle(*toolKind);
        const bool isMosaicBrush = (*toolKind == AnnotationKind::Mosaic)
            && (ts.mosaicKind == AnnotationMosaicKind::Brush);
        if (ApplySubActionToStyle(action, ts, isShape, isMosaicBrush, isText))
        {
            if (!SelectionMatchesTool(toolMode))
            {
                m_doc.ClearSelection();
            }
            return true;
        }
        return false;
    }

    int AnnotationController::ActiveStepNumber(ToolbarButton toolMode) const noexcept
    {
        if (SelectionMatchesTool(toolMode) && SelectedCommonKind() == AnnotationKind::StepBadge)
        {
            if (const AnnotationItem* sel = m_doc.SelectedItem())
            {
                return std::clamp(sel->stepNumber, 1, 999);
            }
        }
        return std::clamp(m_nextStepNumberOverride.value_or(m_doc.NextStepNumber()), 1, 999);
    }

    void AnnotationController::BeginLiveEditSnapshot()
    {
        if (m_liveEditSnapshot.empty() && !m_doc.selectedIds().empty())
        {
            m_liveEditSnapshot = m_doc.items();
        }
    }

    bool AnnotationController::CommitLiveEdit()
    {
        if (m_liveEditSnapshot.empty())
        {
            return false;
        }
        bool differs = (m_liveEditSnapshot.size() != m_doc.items().size());
        if (!differs)
        {
            for (std::size_t i = 0; i < m_liveEditSnapshot.size(); ++i)
            {
                if (!LiveEditableStateEquals(m_liveEditSnapshot[i], m_doc.items()[i]))
                {
                    differs = true;
                    break;
                }
            }
        }
        if (differs)
        {
            m_doc.CommitPreviousState(std::move(m_liveEditSnapshot));
        }
        m_liveEditSnapshot.clear();
        return differs;
    }

    bool AnnotationController::SetActiveStepNumber(
        int number, ToolbarButton toolMode, bool commitUndo)
    {
        const int clamped = std::clamp(number, 1, 999);
        if (SelectionMatchesTool(toolMode) && SelectedCommonKind() == AnnotationKind::StepBadge)
        {
            BeginLiveEditSnapshot();
            bool anyChanged = false;
            int seq = clamped;
            for (std::uint64_t id : m_doc.selectedIds())
            {
                if (AnnotationItem* item = m_doc.FindItem(id);
                    item != nullptr && item->kind == AnnotationKind::StepBadge)
                {
                    if (item->stepNumber != seq)
                    {
                        item->stepNumber = seq;
                        anyChanged = true;
                    }
                    seq = std::min(999, seq + 1);
                }
            }
            m_nextStepNumberOverride.reset();
            if (commitUndo)
            {
                CommitLiveEdit();
            }
            return anyChanged;
        }

        const int prev = ActiveStepNumber(toolMode);
        if (clamped == m_doc.NextStepNumber())
        {
            m_nextStepNumberOverride.reset();
        }
        else
        {
            m_nextStepNumberOverride = clamped;
        }
        return clamped != prev;
    }

    bool AnnotationController::StepActiveStepNumber(int deltaSteps, ToolbarButton toolMode)
    {
        if (deltaSteps == 0)
        {
            return false;
        }
        m_liveEditSnapshot.clear();
        return SetActiveStepNumber(ActiveStepNumber(toolMode) + deltaSteps, toolMode, true);
    }

    bool AnnotationController::SetMosaicEffect(
        AnnotationMosaicEffect effect, ToolbarButton toolMode)
    {
        bool changed = false;
        if (SelectionMatchesTool(toolMode) && SelectedCommonKind() == AnnotationKind::Mosaic)
        {
            std::vector<AnnotationItem> snapshot = m_doc.items();
            for (std::uint64_t id : m_doc.selectedIds())
            {
                if (AnnotationItem* item = m_doc.FindItem(id);
                    item != nullptr && item->kind == AnnotationKind::Mosaic)
                {
                    if (item->style.mosaicEffect != effect)
                    {
                        item->style.mosaicEffect = effect;
                        changed = true;
                    }
                }
            }
            if (changed)
            {
                m_doc.CommitPreviousState(std::move(snapshot));
            }
            ToolStyle(AnnotationKind::Mosaic).mosaicEffect = effect;
            return changed;
        }

        AnnotationStyle& ts = ToolStyle(AnnotationKind::Mosaic);
        if (ts.mosaicEffect != effect)
        {
            ts.mosaicEffect = effect;
            changed = true;
        }
        return changed;
    }

    bool AnnotationController::SetMosaicIntensityNormalized(
        float normalized, ToolbarButton toolMode, bool commitUndo)
    {
        const float t = std::clamp(normalized, 0.0f, 1.0f);
        const auto applyToStyle = [t](AnnotationStyle& st) -> bool
        {
            if (st.mosaicEffect == AnnotationMosaicEffect::GaussianBlur)
            {
                const float raw = kMosaicBlurMinDip + t * (kMosaicBlurMaxDip - kMosaicBlurMinDip);
                const float next = std::round(raw * 2.0f) * 0.5f;
                const float clamped = std::clamp(next, kMosaicBlurMinDip, kMosaicBlurMaxDip);
                if (std::fabs(st.mosaicBlurRadiusDip - clamped) > 0.01f)
                {
                    st.mosaicBlurRadiusDip = clamped;
                    return true;
                }
                return false;
            }
            const int next = static_cast<int>(std::lround(
                static_cast<float>(kMosaicBlockMinDip)
                + t * static_cast<float>(kMosaicBlockMaxDip - kMosaicBlockMinDip)));
            const int clamped = std::clamp(next, kMosaicBlockMinDip, kMosaicBlockMaxDip);
            if (st.mosaicBlockDip != clamped)
            {
                st.mosaicBlockDip = clamped;
                return true;
            }
            return false;
        };

        if (SelectionMatchesTool(toolMode) && SelectedCommonKind() == AnnotationKind::Mosaic)
        {
            BeginLiveEditSnapshot();
            bool anyChanged = false;
            for (std::uint64_t id : m_doc.selectedIds())
            {
                if (AnnotationItem* item = m_doc.FindItem(id);
                    item != nullptr && item->kind == AnnotationKind::Mosaic)
                {
                    if (applyToStyle(item->style))
                    {
                        anyChanged = true;
                    }
                }
            }
            applyToStyle(ToolStyle(AnnotationKind::Mosaic));
            if (commitUndo)
            {
                CommitLiveEdit();
            }
            return anyChanged;
        }

        m_liveEditSnapshot.clear();
        return applyToStyle(ToolStyle(AnnotationKind::Mosaic));
    }

    bool AnnotationController::OnSubToolbarWheel(
        SubToolbarAction action, int delta, ToolbarButton toolMode)
    {
        const int step = m_subWheelAccum.Feed(delta, static_cast<std::uint64_t>(action));
        if (step == 0)
        {
            return false;
        }

        if (action == SubToolbarAction::BadgeNumberValue)
        {
            return StepActiveStepNumber(step, toolMode);
        }

        const auto adjustStyle = [&](AnnotationStyle& style, AnnotationKind kind) -> bool
        {
            if (action == SubToolbarAction::StrokeWidthValue
                && (kind == AnnotationKind::Shape || kind == AnnotationKind::Line || kind == AnnotationKind::Brush))
            {
                style.strokeWidthDip = std::clamp(std::round(style.strokeWidthDip) + step, 1.0f, 50.0f);
                return true;
            }
            if (action == SubToolbarAction::FontSizeValue && kind == AnnotationKind::Text)
            {
                style.fontSizeDip = std::clamp(std::round(style.fontSizeDip) + step * 2.0f, 10.0f, 96.0f);
                return true;
            }
            if (action == SubToolbarAction::BadgeSizeValue && kind == AnnotationKind::StepBadge)
            {
                style.badgeRadiusDip = std::clamp(std::round(style.badgeRadiusDip) + step, 8.0f, 40.0f);
                return true;
            }
            if (action == SubToolbarAction::CornerRadiusCycle
                && kind == AnnotationKind::Shape && style.shapeKind != AnnotationShapeKind::Ellipse)
            {
                style.cornerRadiusDip = std::clamp(std::round(style.cornerRadiusDip) + step * 2.0f, 0.0f, 64.0f);
                return true;
            }
            if (action == SubToolbarAction::MosaicBrushValue
                && kind == AnnotationKind::Mosaic && style.mosaicKind == AnnotationMosaicKind::Brush)
            {
                style.mosaicBrushDip = std::clamp(std::round(style.mosaicBrushDip) + step * 2.0f, 6.0f, 96.0f);
                return true;
            }
            if (action == SubToolbarAction::MosaicEffectDropdown && kind == AnnotationKind::Mosaic)
            {
                style.mosaicEffect = (style.mosaicEffect == AnnotationMosaicEffect::Pixelate)
                    ? AnnotationMosaicEffect::GaussianBlur
                    : AnnotationMosaicEffect::Pixelate;
                return true;
            }
            if (action == SubToolbarAction::MosaicIntensitySlider && kind == AnnotationKind::Mosaic)
            {
                if (style.mosaicEffect == AnnotationMosaicEffect::GaussianBlur)
                {
                    const float fineZone = (style.mosaicBlurRadiusDip < 4.0f
                        || (style.mosaicBlurRadiusDip <= 4.0f && step < 0))
                        ? 0.5f
                        : 2.0f;
                    const float next = std::round((style.mosaicBlurRadiusDip + step * fineZone) * 2.0f) * 0.5f;
                    style.mosaicBlurRadiusDip = std::clamp(next, kMosaicBlurMinDip, kMosaicBlurMaxDip);
                }
                else
                {
                    const int deltaBlock = (style.mosaicBlockDip < 8
                        || (style.mosaicBlockDip <= 8 && step < 0))
                        ? 1
                        : 2;
                    style.mosaicBlockDip = std::clamp(
                        style.mosaicBlockDip + step * deltaBlock,
                        kMosaicBlockMinDip, kMosaicBlockMaxDip);
                }
                return true;
            }
            return false;
        };

        if (m_textEditor.active())
        {
            AnnotationStyle updated = m_textEditor.item().style;
            if (adjustStyle(updated, AnnotationKind::Text))
            {
                adjustStyle(ToolStyle(AnnotationKind::Text), AnnotationKind::Text);
                m_textEditor.UpdateStyle(updated);
                return true;
            }
            return false;
        }

        if (SelectionMatchesTool(toolMode))
        {
            if (const auto selKind = SelectedCommonKind())
            {
                AnnotationStyle probe = m_doc.SelectedItem()->style;
                if (adjustStyle(probe, *selKind))
                {
                    std::vector<AnnotationItem> snapshot = m_doc.items();
                    for (std::uint64_t id : m_doc.selectedIds())
                    {
                        if (AnnotationItem* item = m_doc.FindItem(id))
                        {
                            adjustStyle(item->style, *selKind);
                        }
                    }
                    m_doc.CommitPreviousState(std::move(snapshot));
                    adjustStyle(ToolStyle(*selKind), *selKind);
                    return true;
                }
            }
        }

        if (const auto toolKind = ToolAnnotationKind(toolMode))
        {
            return adjustStyle(ToolStyle(*toolKind), *toolKind);
        }
        return false;
    }

    ColorPickerOpenInfo AnnotationController::ResolvePickerOpenInfo(
        ColorPickerTarget target, ToolbarButton toolMode) const noexcept
    {
        AnnotationKind effectiveKind = ToolAnnotationKind(toolMode).value_or(AnnotationKind::Shape);
        const AnnotationStyle* srcStyle = &ToolStyle(effectiveKind);

        if (m_textEditor.active())
        {
            effectiveKind = AnnotationKind::Text;
            srcStyle = &m_textEditor.item().style;
        }
        else if (SelectionMatchesTool(toolMode))
        {
            if (const AnnotationItem* sel = m_doc.SelectedItem())
            {
                effectiveKind = sel->kind;
                srcStyle = &sel->style;
            }
        }

        const bool isShape = (effectiveKind == AnnotationKind::Shape);
        const bool isLine = (effectiveKind == AnnotationKind::Line);

        ColorPickerOpenInfo info{};
        info.allowNone = (target == ColorPickerTarget::Fill) || isShape;
        info.showStrokeStyles = (target == ColorPickerTarget::Stroke) && (isShape || isLine);
        info.strokeStyle = srcStyle->strokeStyle;
        info.initialColor = (target == ColorPickerTarget::Fill) ? srcStyle->fillColor : srcStyle->color;
        info.restoreAlpha = (target == ColorPickerTarget::Fill)
            ? ValidRestoreAlpha(srcStyle->lastFillAlpha)
            : ValidRestoreAlpha(srcStyle->lastStrokeAlpha);
        return info;
    }

    void AnnotationController::ApplyColorFromPicker(
        const ColorPickerPopover& picker, ToolbarButton toolMode, bool commitUndo)
    {
        const ColorPickerTarget target = picker.target();
        if (target == ColorPickerTarget::None)
        {
            return;
        }

        const D2D1_COLOR_F color = picker.color();
        const AnnotationStrokeStyle strokeStyle = picker.strokeStyle();
        const bool updateStrokeStyle = picker.showsStrokeStyles();
        const float restoreA = ValidRestoreAlpha(picker.restoreAlpha());
        const bool updateLastAlpha = !picker.dragging();

        const auto applyTo = [&](AnnotationStyle& style, bool isShape)
        {
            if (target == ColorPickerTarget::Stroke)
            {
                style.color = color;
                if (updateStrokeStyle)
                {
                    style.strokeStyle = strokeStyle;
                }
                if (!isShape && !HasVisibleColor(style.color))
                {
                    style.color.a = 0.01f;
                }
                if (updateLastAlpha)
                {
                    style.lastStrokeAlpha = HasVisibleColor(style.color) ? style.color.a : restoreA;
                }
            }
            else if (target == ColorPickerTarget::Fill)
            {
                style.fillColor = color;
                if (updateLastAlpha)
                {
                    style.lastFillAlpha = HasVisibleColor(style.fillColor) ? style.fillColor.a : restoreA;
                }
            }
        };

        if (m_textEditor.active())
        {
            AnnotationStyle updated = m_textEditor.item().style;
            applyTo(updated, false);
            applyTo(ToolStyle(AnnotationKind::Text), false);
            m_textEditor.UpdateStyle(updated);
            return;
        }

        if (SelectionMatchesTool(toolMode))
        {
            if (const auto selKind = SelectedCommonKind())
            {
                BeginLiveEditSnapshot();
                const bool isShape = (*selKind == AnnotationKind::Shape);
                for (std::uint64_t id : m_doc.selectedIds())
                {
                    if (AnnotationItem* item = m_doc.FindItem(id))
                    {
                        applyTo(item->style, isShape);
                    }
                }
                applyTo(ToolStyle(*selKind), isShape);
                if (commitUndo)
                {
                    CommitLiveEdit();
                }
                return;
            }
        }

        if (commitUndo)
        {
            m_liveEditSnapshot.clear();
        }
        if (const auto toolKind = ToolAnnotationKind(toolMode))
        {
            applyTo(ToolStyle(*toolKind), *toolKind == AnnotationKind::Shape);
        }
    }

    void AnnotationController::CommitTextEditor(HWND window)
    {
        if (!m_textEditor.active())
        {
            return;
        }
        m_textEditor.Commit(window, m_doc);
        if (m_interaction == InteractionKind::TextEditor)
        {
            m_interaction = InteractionKind::None;
        }
    }

    void AnnotationController::BeginTextEditAt(
        HWND window, POINT point, IDWriteFactory* dwrite, int dpi)
    {
        m_textEditor.StartNew(
            window,
            D2D1::Point2F(static_cast<float>(point.x), static_cast<float>(point.y)),
            ToolStyle(AnnotationKind::Text),
            dwrite, dpi);
        SetFocus(window);
    }

    bool AnnotationController::TryBeginSelectedHandleDrag(
        POINT point, ToolbarButton toolMode, IDWriteFactory* dwrite, int dpi,
        float handleSizeDip)
    {
        if (m_doc.selectionCount() != 1 || toolMode == ToolbarButton::EditSelection)
        {
            return false;
        }
        const AnnotationItem* sel = m_doc.SelectedItem();
        if (sel == nullptr)
        {
            return false;
        }
        const AnnotationHandle handle = HitTestAnnotationHandle(
            *sel, point, dwrite, dpi, ScaleByDpi(handleSizeDip, dpi));
        if (handle == AnnotationHandle::None || handle == AnnotationHandle::Body)
        {
            return false;
        }
        m_snapshotBeforeTransform = m_doc.items();
        m_interaction = InteractionKind::Transform;
        m_dragHandle = handle;
        m_dragPrimaryId = sel->id;
        m_dragPrimaryStart = *sel;
        m_dragMultiStart.clear();
        m_dragMultiStart.emplace_back(sel->id, *sel);
        m_dragOrigin = point;
        m_lastPointer = point;
        return true;
    }

    bool AnnotationController::HandleActiveTextEditorLeftDown(
        HWND window, POINT point, IDWriteFactory* dwrite, int dpi,
        bool& outNeedToolbarSync, bool& outNeedImeSync, KeyModifiers mods)
    {
        outNeedToolbarSync = false;
        outNeedImeSync = false;
        if (!m_textEditor.active())
        {
            return false;
        }
        if (m_textEditor.OnLeftButtonDown(window, point, dwrite, dpi, mods))
        {
            m_interaction = InteractionKind::TextEditor;
            return true;
        }
        CommitTextEditor(window);
        outNeedToolbarSync = true;
        outNeedImeSync = true;
        return true;
    }

    void AnnotationController::OnCanvasLeftButtonDown(
        HWND window, POINT point, ToolbarButton toolMode,
        const RECT& selectionRect, TransformModifiers mods,
        bool isDoubleClick, IDWriteFactory* dwrite, int dpi,
        bool& outNeedToolbarSync, bool& outNeedImeSync)
    {
        outNeedToolbarSync = false;
        outNeedImeSync = false;
        m_lastPointer = point;
        m_snapCanvasBounds = selectionRect;

        const auto editTextAnnotation = [&](const AnnotationItem& textItem)
        {
            m_doc.SetSelectedId(textItem.id);
            m_textEditor.StartEdit(window, textItem, m_doc.items(), dwrite, dpi);
            if (m_textEditor.OnLeftButtonDown(window, point, dwrite, dpi, mods))
            {
                m_interaction = InteractionKind::TextEditor;
            }
            outNeedImeSync = true;
            outNeedToolbarSync = true;
        };

        const auto beginMoveSelection = [&]()
        {
            m_snapshotBeforeTransform = m_doc.items();
            m_interaction = InteractionKind::Transform;
            m_dragHandle = AnnotationHandle::Body;
            m_dragPrimaryId = m_doc.selectedId();
            if (const AnnotationItem* primary = m_doc.SelectedItem())
            {
                m_dragPrimaryStart = *primary;
            }
            m_dragMultiStart.clear();
            for (std::uint64_t id : m_doc.selectedIds())
            {
                if (const AnnotationItem* it = m_doc.FindItem(id))
                {
                    m_dragMultiStart.emplace_back(id, *it);
                }
            }
            m_dragOrigin = point;
        };

        if (toolMode == ToolbarButton::SelectObject || mods.ctrl)
        {
            const std::uint64_t hitId = m_doc.HitTestTopmost(point, dwrite, dpi);
            if (mods.ctrl || mods.shift)
            {
                if (hitId != 0)
                {
                    m_doc.ToggleSelectedId(hitId);
                    outNeedToolbarSync = true;
                    if (m_doc.IsSelected(hitId) && !mods.ctrl)
                    {
                        beginMoveSelection();
                    }
                    return;
                }
                if (toolMode == ToolbarButton::SelectObject)
                {
                    m_interaction = InteractionKind::MarqueeSelect;
                    m_dragOrigin = point;
                    m_marqueeRect = D2D1::RectF(
                        static_cast<float>(point.x), static_cast<float>(point.y),
                        static_cast<float>(point.x), static_cast<float>(point.y));
                    m_marqueeBaseIds = m_doc.selectedIds();
                    m_marqueeToggle = mods.ctrl;
                }
                return;
            }

            if (hitId != 0)
            {
                if (!m_doc.IsSelected(hitId))
                {
                    m_doc.SetSelectedId(hitId);
                    outNeedToolbarSync = true;
                }
                if (const AnnotationItem* item = m_doc.FindItem(hitId))
                {
                    if (isDoubleClick && item->kind == AnnotationKind::Text)
                    {
                        editTextAnnotation(*item);
                        return;
                    }
                    if (isDoubleClick && item->kind == AnnotationKind::StepBadge)
                    {
                        m_requestStepNumberEdit = true;
                        outNeedToolbarSync = true;
                        return;
                    }
                    beginMoveSelection();
                }
                return;
            }

            m_doc.ClearSelection();
            outNeedToolbarSync = true;
            if (toolMode == ToolbarButton::SelectObject)
            {
                m_interaction = InteractionKind::MarqueeSelect;
                m_dragOrigin = point;
                m_marqueeRect = D2D1::RectF(
                    static_cast<float>(point.x), static_cast<float>(point.y),
                    static_cast<float>(point.x), static_cast<float>(point.y));
                m_marqueeBaseIds.clear();
                m_marqueeToggle = false;
            }
            return;
        }

        if (toolMode == ToolbarButton::Text)
        {
            const std::uint64_t hitId = m_doc.HitTestTopmost(point, dwrite, dpi);
            if (hitId != 0)
            {
                if (const AnnotationItem* hitItem = m_doc.FindItem(hitId);
                    hitItem != nullptr && hitItem->kind == AnnotationKind::Text)
                {
                    editTextAnnotation(*hitItem);
                    return;
                }
            }
            m_doc.ClearSelection();
            BeginTextEditAt(window, point, dwrite, dpi);
            outNeedImeSync = true;
            outNeedToolbarSync = true;
            return;
        }

        if (toolMode == ToolbarButton::StepBadge && isDoubleClick)
        {
            const std::uint64_t hitId = m_doc.HitTestTopmost(point, dwrite, dpi);
            if (hitId != 0)
            {
                if (const AnnotationItem* hitItem = m_doc.FindItem(hitId);
                    hitItem != nullptr && hitItem->kind == AnnotationKind::StepBadge)
                {
                    m_doc.SetSelectedId(hitId);
                    m_requestStepNumberEdit = true;
                    outNeedToolbarSync = true;
                    return;
                }
            }
        }

        if (SelectionMatchesTool(toolMode))
        {
            for (std::uint64_t id : m_doc.selectedIds())
            {
                if (const AnnotationItem* sel = m_doc.FindItem(id);
                    sel != nullptr && HitTestAnnotationBody(*sel, point, dwrite, dpi))
                {
                    beginMoveSelection();
                    return;
                }
            }
        }

        m_doc.ClearSelection();
        const D2D1_POINT_2F pt{ static_cast<float>(point.x), static_cast<float>(point.y) };

        if (toolMode == ToolbarButton::StepBadge)
        {
            const int placedNumber = std::clamp(
                m_nextStepNumberOverride.value_or(m_doc.NextStepNumber()), 1, 999);
            AnnotationItem item{};
            item.kind = AnnotationKind::StepBadge;
            item.style = ToolStyle(AnnotationKind::StepBadge);
            item.origin = pt;
            item.stepNumber = placedNumber;
            const std::uint64_t newId = m_doc.AddItem(std::move(item));
            if (m_nextStepNumberOverride.has_value())
            {
                const int next = std::min(999, placedNumber + 1);
                if (next == m_doc.NextStepNumber())
                {
                    m_nextStepNumberOverride.reset();
                }
                else
                {
                    m_nextStepNumberOverride = next;
                }
            }
            m_doc.SetSelectedId(newId);
            outNeedToolbarSync = true;
            return;
        }

        const std::optional<AnnotationKind> toolKind = ToolAnnotationKind(toolMode);
        if (!toolKind.has_value())
        {
            return;
        }

        m_interaction = InteractionKind::DrawDraft;
        m_dragOrigin = point;
        m_hasDraft = true;
        m_draft = AnnotationItem{};
        m_draft.kind = *toolKind;
        m_draft.style = ToolStyle(*toolKind);

        switch (*toolKind)
        {
        case AnnotationKind::Shape:
            m_draft.rect = D2D1::RectF(pt.x, pt.y, pt.x, pt.y);
            break;
        case AnnotationKind::Line:
            m_draft.start = pt;
            m_draft.end = pt;
            break;
        case AnnotationKind::Brush:
            m_draft.points.push_back(pt);
            break;
        case AnnotationKind::Mosaic:
            m_draft.rect = D2D1::RectF(pt.x, pt.y, pt.x, pt.y);
            if (m_draft.style.mosaicKind == AnnotationMosaicKind::Brush)
            {
                m_draft.points.push_back(pt);
            }
            break;
        default:
            break;
        }
        outNeedToolbarSync = true;
    }

    void AnnotationController::ApplyTransformDrag(
        POINT point, TransformModifiers mods, const RECT& canvasBounds,
        IDWriteFactory* dwrite, int dpi)
    {
        m_snapGuidesX.clear();
        m_snapGuidesY.clear();
        m_snapCanvasBounds = canvasBounds;

        if (m_dragHandle == AnnotationHandle::Body)
        {
            float dx = static_cast<float>(point.x - m_dragOrigin.x);
            float dy = static_cast<float>(point.y - m_dragOrigin.y);

            if (mods.ctrl && !m_dragMultiStart.empty())
            {
                D2D1_RECT_F movingBox = AnnotationBounds(m_dragMultiStart.front().second, dwrite, dpi);
                for (std::size_t i = 1; i < m_dragMultiStart.size(); ++i)
                {
                    const D2D1_RECT_F b = AnnotationBounds(m_dragMultiStart[i].second, dwrite, dpi);
                    movingBox.left = std::min(movingBox.left, b.left);
                    movingBox.top = std::min(movingBox.top, b.top);
                    movingBox.right = std::max(movingBox.right, b.right);
                    movingBox.bottom = std::max(movingBox.bottom, b.bottom);
                }
                movingBox.left += dx;
                movingBox.right += dx;
                movingBox.top += dy;
                movingBox.bottom += dy;

                std::vector<float> snapXs;
                std::vector<float> snapYs;
                snapXs.push_back(static_cast<float>(canvasBounds.left));
                snapXs.push_back(static_cast<float>(canvasBounds.left + canvasBounds.right) * 0.5f);
                snapXs.push_back(static_cast<float>(canvasBounds.right));
                snapYs.push_back(static_cast<float>(canvasBounds.top));
                snapYs.push_back(static_cast<float>(canvasBounds.top + canvasBounds.bottom) * 0.5f);
                snapYs.push_back(static_cast<float>(canvasBounds.bottom));

                for (const AnnotationItem& other : m_doc.items())
                {
                    if (m_doc.IsSelected(other.id))
                    {
                        continue;
                    }
                    const D2D1_RECT_F b = AnnotationBounds(other, dwrite, dpi);
                    snapXs.push_back(b.left);
                    snapXs.push_back((b.left + b.right) * 0.5f);
                    snapXs.push_back(b.right);
                    snapYs.push_back(b.top);
                    snapYs.push_back((b.top + b.bottom) * 0.5f);
                    snapYs.push_back(b.bottom);
                }

                const float thresh = ScaleByDpi(6.0f, dpi);
                float bestAbsX = thresh;
                float snapDx = 0.0f;
                bool hasSnapX = false;
                float guideX = 0.0f;
                const float mx[3] = { movingBox.left, (movingBox.left + movingBox.right) * 0.5f, movingBox.right };
                for (float tx : snapXs)
                {
                    for (float c : mx)
                    {
                        const float diff = tx - c;
                        if (std::fabs(diff) < bestAbsX)
                        {
                            bestAbsX = std::fabs(diff);
                            snapDx = diff;
                            guideX = tx;
                            hasSnapX = true;
                        }
                    }
                }

                float bestAbsY = thresh;
                float snapDy = 0.0f;
                bool hasSnapY = false;
                float guideY = 0.0f;
                const float my[3] = { movingBox.top, (movingBox.top + movingBox.bottom) * 0.5f, movingBox.bottom };
                for (float ty : snapYs)
                {
                    for (float c : my)
                    {
                        const float diff = ty - c;
                        if (std::fabs(diff) < bestAbsY)
                        {
                            bestAbsY = std::fabs(diff);
                            snapDy = diff;
                            guideY = ty;
                            hasSnapY = true;
                        }
                    }
                }

                if (hasSnapX)
                {
                    dx += snapDx;
                    m_snapGuidesX.push_back(guideX);
                }
                if (hasSnapY)
                {
                    dy += snapDy;
                    m_snapGuidesY.push_back(guideY);
                }
            }

            for (const auto& [id, startItem] : m_dragMultiStart)
            {
                if (AnnotationItem* item = m_doc.FindItem(id))
                {
                    *item = startItem;
                    MoveAnnotation(*item, dx, dy);
                }
            }
            return;
        }

        if (AnnotationItem* item = m_doc.FindItem(m_dragPrimaryId))
        {
            *item = DragAnnotationHandle(m_dragPrimaryStart, m_dragHandle, m_dragOrigin, point, mods);
        }
    }

    void AnnotationController::UpdateMarqueeSelection(
        POINT point, TransformModifiers mods, IDWriteFactory* dwrite, int dpi)
    {
        m_marqueeRect = NormalizeAnnotationRect(D2D1::RectF(
            static_cast<float>(m_dragOrigin.x),
            static_cast<float>(m_dragOrigin.y),
            static_cast<float>(point.x),
            static_cast<float>(point.y)));
        const std::vector<std::uint64_t> inside = m_doc.HitTestRect(m_marqueeRect, dwrite, dpi);
        if (!mods.ctrl && !mods.shift)
        {
            m_doc.SetSelectedIds(inside);
            return;
        }

        std::vector<std::uint64_t> merged = m_marqueeBaseIds;
        for (std::uint64_t id : inside)
        {
            const auto it = std::find(merged.begin(), merged.end(), id);
            if (m_marqueeToggle && it != merged.end())
            {
                merged.erase(it);
            }
            else if (it == merged.end())
            {
                merged.push_back(id);
            }
        }
        m_doc.SetSelectedIds(std::move(merged));
    }

    void AnnotationController::OnMouseMove(
        HWND window, POINT point, TransformModifiers mods,
        const RECT& canvasBounds, IDWriteFactory* dwrite, int dpi)
    {
        m_lastPointer = point;
        switch (m_interaction)
        {
        case InteractionKind::TextEditor:
            m_textEditor.OnMouseMove(window, point, dwrite, dpi);
            break;

        case InteractionKind::DrawDraft:
            if (m_hasDraft)
            {
                const D2D1_POINT_2F pt{ static_cast<float>(point.x), static_cast<float>(point.y) };
                if (m_draft.kind == AnnotationKind::Shape
                    || (m_draft.kind == AnnotationKind::Mosaic
                        && m_draft.style.mosaicKind == AnnotationMosaicKind::Rectangle))
                {
                    const bool lockSquare = mods.shift && (m_draft.kind == AnnotationKind::Shape);
                    const bool fromCenter = mods.alt && (m_draft.kind == AnnotationKind::Shape);
                    m_draft.rect = ConstrainDraftRect(m_dragOrigin, point, lockSquare, fromCenter);
                }
                else if (m_draft.kind == AnnotationKind::Line)
                {
                    m_draft.end = ConstrainLineEndpoint(m_draft.start, pt, mods.shift);
                }
                else if (!m_draft.points.empty())
                {
                    const D2D1_POINT_2F& prev = m_draft.points.back();
                    if (std::hypot(pt.x - prev.x, pt.y - prev.y) >= 2.0f)
                    {
                        m_draft.points.push_back(pt);
                    }
                }
            }
            break;

        case InteractionKind::Transform:
            ApplyTransformDrag(point, mods, canvasBounds, dwrite, dpi);
            break;

        case InteractionKind::MarqueeSelect:
            UpdateMarqueeSelection(point, mods, dwrite, dpi);
            break;

        case InteractionKind::None:
        default:
            break;
        }
    }

    bool AnnotationController::OnModifiersChanged(
        POINT point, TransformModifiers mods, const RECT& canvasBounds,
        IDWriteFactory* dwrite, int dpi)
    {
        if (m_interaction != InteractionKind::DrawDraft
            && m_interaction != InteractionKind::Transform
            && m_interaction != InteractionKind::MarqueeSelect)
        {
            return false;
        }
        OnMouseMove(nullptr, point, mods, canvasBounds, dwrite, dpi);
        return true;
    }

    void AnnotationController::OnLeftButtonUp(
        POINT point, const RECT& canvasBounds, IDWriteFactory* dwrite, int dpi,
        bool& outNeedToolbarSync)
    {
        outNeedToolbarSync = false;
        (void)canvasBounds;
        (void)dwrite;
        (void)dpi;

        switch (m_interaction)
        {
        case InteractionKind::TextEditor:
            m_interaction = InteractionKind::None;
            m_textEditor.OnLeftButtonUp();
            break;

        case InteractionKind::DrawDraft:
            m_interaction = InteractionKind::None;
            if (m_hasDraft)
            {
                m_hasDraft = false;
                bool keep = false;
                if (m_draft.kind == AnnotationKind::Shape
                    || (m_draft.kind == AnnotationKind::Mosaic
                        && m_draft.style.mosaicKind == AnnotationMosaicKind::Rectangle))
                {
                    m_draft.rect = NormalizeAnnotationRect(m_draft.rect);
                    keep = (m_draft.rect.right - m_draft.rect.left >= 3.0f)
                        && (m_draft.rect.bottom - m_draft.rect.top >= 3.0f);
                }
                else if (m_draft.kind == AnnotationKind::Line)
                {
                    const float len = std::hypot(
                        m_draft.end.x - m_draft.start.x,
                        m_draft.end.y - m_draft.start.y);
                    keep = len >= 3.0f;
                }
                else
                {
                    keep = !m_draft.points.empty();
                }

                if (keep)
                {
                    const bool autoSelect = m_draft.kind == AnnotationKind::Shape
                        || m_draft.kind == AnnotationKind::Line
                        || (m_draft.kind == AnnotationKind::Mosaic
                            && m_draft.style.mosaicKind == AnnotationMosaicKind::Rectangle);
                    const std::uint64_t newId = m_doc.AddItem(std::move(m_draft));
                    if (autoSelect)
                    {
                        m_doc.SetSelectedId(newId);
                    }
                    outNeedToolbarSync = true;
                }
                m_draft = AnnotationItem{};
            }
            break;

        case InteractionKind::Transform:
            m_interaction = InteractionKind::None;
            m_dragHandle = AnnotationHandle::None;
            m_snapGuidesX.clear();
            m_snapGuidesY.clear();
            if (point.x != m_dragOrigin.x || point.y != m_dragOrigin.y)
            {
                m_doc.CommitPreviousState(std::move(m_snapshotBeforeTransform));
                outNeedToolbarSync = true;
            }
            m_snapshotBeforeTransform.clear();
            m_dragMultiStart.clear();
            m_dragPrimaryId = 0;
            break;

        case InteractionKind::MarqueeSelect:
            m_interaction = InteractionKind::None;
            m_marqueeBaseIds.clear();
            outNeedToolbarSync = true;
            break;

        case InteractionKind::None:
        default:
            break;
        }
    }

    bool AnnotationController::OnRightButtonUp(
        HWND window, POINT point, IDWriteFactory* dwrite, int dpi,
        bool& outNeedToolbarSync, bool& outNeedImeSync)
    {
        outNeedToolbarSync = false;
        outNeedImeSync = false;
        if (!m_textEditor.active())
        {
            return false;
        }
        if (m_textEditor.ShowContextMenu(window, point, dwrite, dpi))
        {
            outNeedToolbarSync = true;
            outNeedImeSync = true;
            return true;
        }
        CommitTextEditor(window);
        outNeedToolbarSync = true;
        outNeedImeSync = true;
        return true;
    }

    bool AnnotationController::OnKeyDown(
        HWND window, WPARAM key, ToolbarButton toolMode,
        IDWriteFactory* dwrite, int dpi,
        bool& outNeedToolbarSync, bool& outNeedImeSync,
        KeyModifiers mods)
    {
        outNeedToolbarSync = false;
        outNeedImeSync = false;

        if (m_textEditor.active())
        {
            bool committed = false;
            bool needsRender = false;
            if (m_textEditor.OnKeyDown(window, key, dwrite, dpi, committed, needsRender, mods))
            {
                if (committed)
                {
                    CommitTextEditor(window);
                    outNeedToolbarSync = true;
                    outNeedImeSync = true;
                }
                else if (!m_textEditor.active())
                {
                    if (m_interaction == InteractionKind::TextEditor)
                    {
                        m_interaction = InteractionKind::None;
                    }
                    outNeedToolbarSync = true;
                    outNeedImeSync = true;
                }
                return true;
            }
            return false;
        }

        const bool ctrl = mods.ctrl;
        const bool shift = mods.shift;

        if (ctrl && key == 'Z' && !shift)
        {
            if (m_doc.Undo())
            {
                outNeedToolbarSync = true;
            }
            return true;
        }
        if (ctrl && (key == 'Y' || (key == 'Z' && shift)))
        {
            if (m_doc.Redo())
            {
                outNeedToolbarSync = true;
            }
            return true;
        }
        if (ctrl && key == 'A' && !m_doc.empty() && toolMode != ToolbarButton::EditSelection)
        {
            m_doc.SelectAll();
            outNeedToolbarSync = true;
            return true;
        }
        if (ctrl && key == 'D' && m_doc.selectionCount() > 0 && toolMode != ToolbarButton::EditSelection)
        {
            const float offset = ScaleByDpi(14.0f, dpi);
            if (m_doc.DuplicateSelected(offset, offset))
            {
                outNeedToolbarSync = true;
            }
            return true;
        }

        if ((key == VK_DELETE || key == VK_BACK) && m_doc.selectionCount() > 0)
        {
            if (m_doc.DeleteSelected())
            {
                outNeedToolbarSync = true;
            }
            return true;
        }

        return false;
    }

    bool AnnotationController::OnChar(
        HWND window, wchar_t ch, IDWriteFactory* dwrite, int dpi)
    {
        return m_textEditor.active() && m_textEditor.OnChar(window, ch, dwrite, dpi);
    }

    bool AnnotationController::OnImeStartComposition(
        HWND window, IDWriteFactory* dwrite, int dpi) const
    {
        return m_textEditor.OnImeStartComposition(window, dwrite, dpi);
    }

    bool AnnotationController::OnImeComposition(
        HWND window, LPARAM lparam, IDWriteFactory* dwrite, int dpi)
    {
        return m_textEditor.OnImeComposition(window, lparam, dwrite, dpi);
    }

    bool AnnotationController::OnImeEndComposition(HWND window) noexcept
    {
        return m_textEditor.OnImeEndComposition(window);
    }

    void AnnotationController::DrawCanvasContent(
        ID2D1Factory1* factory, ID2D1DeviceContext* context,
        ID2D1StrokeStyle* roundStroke, IDWriteFactory* dwrite,
        ID2D1Bitmap* sourceBitmap, int dpi)
    {
        const std::uint64_t hiddenId = m_textEditor.active() ? m_textEditor.editingId() : 0;
        m_renderer.DrawAnnotations(
            factory, context, roundStroke, dwrite, sourceBitmap, dpi,
            m_doc.items(),
            m_hasDraft ? &m_draft : nullptr,
            m_doc.selectedIds(),
            /*showSelectionChrome=*/false,
            hiddenId);
    }

    void AnnotationController::DrawOverlayChrome(
        ID2D1Factory1* factory, ID2D1DeviceContext* context,
        IDWriteFactory* dwrite, int dpi,
        const HudBrushes& brushes)
    {
        if (context == nullptr)
        {
            return;
        }
        const std::uint64_t hiddenId = m_textEditor.active() ? m_textEditor.editingId() : 0;
        if (!m_textEditor.active() && m_doc.selectionCount() > 0)
        {
            m_renderer.DrawSelectionChrome(
                factory, context, dwrite, dpi,
                m_doc.items(), m_doc.selectedIds(), hiddenId);
        }

        if (m_interaction == InteractionKind::MarqueeSelect)
        {
            const D2D1_RECT_F mr = NormalizeAnnotationRect(m_marqueeRect);
            if (mr.right - mr.left >= 1.0f || mr.bottom - mr.top >= 1.0f)
            {
                context->FillRectangle(mr, brushes.WithColor(D2D1::ColorF(0.18f, 0.49f, 0.94f, 0.16f)));
                context->DrawRectangle(
                    mr, brushes.WithColor(D2D1::ColorF(0.18f, 0.49f, 0.94f, 0.92f)), ScaleByDpi(1.25f, dpi));
            }
        }

        if (!m_snapGuidesX.empty() || !m_snapGuidesY.empty())
        {
            ID2D1SolidColorBrush* guideBrush = brushes.WithColor(D2D1::ColorF(0.0f, 0.83f, 1.0f, 0.88f));
            const float gw = std::max(1.0f, ScaleByDpi(1.0f, dpi));
            for (float gx : m_snapGuidesX)
            {
                context->DrawLine(
                    D2D1::Point2F(gx, static_cast<float>(m_snapCanvasBounds.top)),
                    D2D1::Point2F(gx, static_cast<float>(m_snapCanvasBounds.bottom)),
                    guideBrush, gw);
            }
            for (float gy : m_snapGuidesY)
            {
                context->DrawLine(
                    D2D1::Point2F(static_cast<float>(m_snapCanvasBounds.left), gy),
                    D2D1::Point2F(static_cast<float>(m_snapCanvasBounds.right), gy),
                    guideBrush, gw);
            }
        }

        if (m_textEditor.active())
        {
            m_textEditor.Draw(factory, context, dwrite, dpi, brushes);
        }
    }

    void AnnotationController::ExportToClipboard(
        HWND owner, const RenderDevice& device, const ScreenImage& sourceImage,
        const RECT& bounds, const RECT& selection) const
    {
        m_renderer.ExportSelectionToClipboard(
            owner, device, sourceImage, bounds, selection, m_doc.items());
    }
}
