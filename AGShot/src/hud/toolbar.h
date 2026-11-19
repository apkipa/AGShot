#pragma once

#include "hud/annotations.h"
#include "hud/color_picker.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace agshot
{
    enum class ToolbarButton
    {
        None = -1,
        EditSelection = 0,
        SelectObject,
        Shape,
        Arrow,
        Brush,
        Text,
        StepBadge,
        Mosaic,
        Undo,
        Redo,
        TextRecognition,
        Cancel,
        Copy,
        Count,
    };

    enum class SubToolbarAction
    {
        None,
        ShapeRect,
        ShapeEllipse,
        ShapeDiamond,
        LineArrow,
        LineStraight,
        MosaicBrush,
        MosaicRect,
        BrushHighlighterToggle,
        StrokePicker,
        Color0,
        Color1,
        Color2,
        Color3,
        Color4,
        Color5,
        FillPicker,
        FillModeNone,
        FillModeSolid,
        FillPatternCrossHatch,
        FillPatternDiagHatch,
        StrokeWidthValue,
        Width0,
        Width1,
        Width2,
        Width3,
        FontSizeValue,
        Font0,
        Font1,
        Font2,
        Font3,
        BadgeShapeOutlineCircle,
        BadgeShapeFilledCircle,
        BadgeShapeOutlineSquare,
        BadgeShapeFilledSquare,
        BadgeNumArabic,
        BadgeNumRoman,
        BadgeNumLowerAlpha,
        BadgeNumUpperAlpha,
        BadgeNumChinese,
        BadgeNumberValue,
        BadgeSizeValue,
        BadgeSize0,
        BadgeSize1,
        BadgeSize2,
        BadgeSize3,
        CornerRadiusCycle,
        MosaicEffectDropdown,
        MosaicBrushValue,
        MosaicIntensitySlider,
        OrderSendToBack,
        OrderSendBackward,
        OrderBringForward,
        OrderBringToFront,
        AlignLeft,
        AlignCenterH,
        AlignRight,
        AlignTop,
        AlignCenterV,
        AlignBottom,
        DistributeH,
        DistributeV,
        DuplicateSelected,
        DeleteSelected,
    };

    enum class SubItemKind
    {
        IconToggle,
        StrokeWell,
        FillWell,
        ColorSwatch,
        StrokeWidth,
        BadgeShape,
        TextPill,
        ValuePill,
        DropdownPill,
        StaticIcon,
        Slider,
        Divider,
    };

    struct SubToolbarItem
    {
        SubToolbarAction action{ SubToolbarAction::None };
        SubItemKind kind{ SubItemKind::IconToggle };
        RECT rect{};
        bool active{};
        bool enabled{ true };
        bool destructive{};
        int variant{}; // color index or stroke width index
        float sliderValue{}; // normalized [0..1] for SubItemKind::Slider
        D2D1_COLOR_F swatchColor{};
        bool hasColor{ true };
        AnnotationStrokeStyle strokeStyle{ AnnotationStrokeStyle::Solid };
        wchar_t label[16]{};
    };

    enum class ToolbarHitTarget
    {
        None,
        Panel,
        Handle,
        Button,
        SubPanel,
        SubButton,
    };

    struct ToolbarHit
    {
        ToolbarHitTarget target{ ToolbarHitTarget::None };
        ToolbarButton button{ ToolbarButton::None };
        SubToolbarAction subAction{ SubToolbarAction::None };
    };

    struct ToolbarState
    {
        ToolbarButton activeMode{ ToolbarButton::EditSelection };
        bool canUndo{};
        bool canRedo{};
        bool canBringForward{};
        bool canSendBackward{};
        std::optional<AnnotationKind> selectedKind;
        std::size_t selectedCount{};
        AnnotationStyle activeStyle{};
        ColorPickerTarget activePicker{ ColorPickerTarget::None };
        bool mosaicEffectMenuOpen{};
        int activeStepNumber{ 1 };
    };

    // Main toolbar + contextual sub-bar geometry and hover/enable state.
    struct Toolbar
    {
        RECT panel{};
        RECT handle{};
        std::array<RECT, static_cast<std::size_t>(ToolbarButton::Count)> buttons{};
        std::array<bool, static_cast<std::size_t>(ToolbarButton::Count)> enabled{};
        std::array<RECT, 4> dividers{};

        RECT subPanel{};
        bool subVisible{};
        std::vector<SubToolbarItem> subItems;

        RECT subPanel2{};
        bool subVisible2{};
        std::vector<SubToolbarItem> subItems2;

        ToolbarButton hovered{ ToolbarButton::None };
        SubToolbarAction hoveredSub{ SubToolbarAction::None };

        const RECT& buttonRect(ToolbarButton button) const noexcept
        {
            return buttons[static_cast<std::size_t>(button)];
        }

        bool isEnabled(ToolbarButton button) const noexcept
        {
            return button != ToolbarButton::None && enabled[static_cast<std::size_t>(button)];
        }

        const SubToolbarItem* findSubItem(SubToolbarAction action) const noexcept
        {
            if (action == SubToolbarAction::None)
            {
                return nullptr;
            }
            if (subVisible)
            {
                for (const SubToolbarItem& item : subItems)
                {
                    if (item.action == action)
                    {
                        return &item;
                    }
                }
            }
            if (subVisible2)
            {
                for (const SubToolbarItem& item : subItems2)
                {
                    if (item.action == action)
                    {
                        return &item;
                    }
                }
            }
            return nullptr;
        }

        bool isSubEnabled(SubToolbarAction action) const noexcept
        {
            const SubToolbarItem* item = findSubItem(action);
            return item != nullptr && item->enabled;
        }
    };

    bool IsAnnotationToolButton(ToolbarButton button) noexcept;
    std::optional<AnnotationKind> ToolAnnotationKind(ToolbarButton button) noexcept;

    // Lays out the main toolbar near "selection" within "bounds", then positions
    // any contextual sub-bar according to "state".
    bool LayoutToolbar(Toolbar& toolbar, const ToolbarState& state,
                       const RECT& selection, const RECT& bounds, int dpi) noexcept;

    // Rebuilds and repositions only the contextual sub-bar anchored to the
    // existing main panel position.
    void LayoutSubToolbar(Toolbar& toolbar, const ToolbarState& state,
                          const RECT& bounds, int dpi) noexcept;

    ToolbarButton HitTestToolbar(const Toolbar& toolbar, POINT point) noexcept;
    ToolbarHit HitTestToolbarTarget(const Toolbar& toolbar, POINT point) noexcept;
    void DragToolbar(Toolbar& toolbar, const Toolbar& start, POINT origin, POINT current,
                     const RECT& bounds) noexcept;

    LPCWSTR ToolbarLabel(ToolbarButton button) noexcept;
    LPCWSTR SubToolbarLabel(SubToolbarAction action) noexcept;
    RECT MosaicSliderTrackRect(const RECT& itemRect, int dpi) noexcept;
}
