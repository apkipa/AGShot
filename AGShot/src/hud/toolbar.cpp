#include "pch.h"

#include "hud/color_model.h"
#include "hud/toolbar.h"
#include "hud_icon_ids.h"

namespace agshot
{
    namespace
    {
        // Everything here is in DIPs at 96 DPI and scaled on the way out.
        constexpr int kButtonSize = 32;
        constexpr int kSubButtonSize = 26;
        constexpr int kSubPillWidth = 28;
        constexpr int kSubValueWidth = 40;
        constexpr int kSubNumberWidth = 48;
        constexpr int kSubDropdownWidth = 74;
        constexpr int kSubStaticIconWidth = 20;
        constexpr int kSubSliderWidth = 84;
        constexpr int kHandleWidth = 18;
        constexpr int kPadding = 4;
        constexpr int kGap = 2;
        constexpr int kGroupGap = 6;
        constexpr int kDividerWidth = 1;
        constexpr int kMargin = 8;
        constexpr int kSubBarGap = 6;

        constexpr ToolbarButton kMainOrder[] =
        {
            ToolbarButton::EditSelection,
            ToolbarButton::SelectObject,
            // divider 0
            ToolbarButton::Shape,
            ToolbarButton::Arrow,
            ToolbarButton::Brush,
            ToolbarButton::Text,
            ToolbarButton::StepBadge,
            ToolbarButton::Mosaic,
            // divider 1
            ToolbarButton::Undo,
            ToolbarButton::Redo,
            // divider 2
            ToolbarButton::TextRecognition,
            // divider 3
            ToolbarButton::Cancel,
            ToolbarButton::Copy,
        };

        // Group boundary indices into kMainOrder (divider placed before each index).
        constexpr std::size_t kDividerBefore[4] = { 2, 8, 10, 11 };

        template <std::size_t N>
        int MatchingPaletteIndex(const std::array<D2D1_COLOR_F, N>& palette,
                                 const D2D1_COLOR_F& color) noexcept
        {
            for (std::size_t i = 0; i < N; ++i)
            {
                if (SameRgbaColor(palette[i], color, 0.02f))
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        SubToolbarItem MakeIconToggle(SubToolbarAction action, bool active, bool destructive = false) noexcept
        {
            SubToolbarItem item{};
            item.action = action;
            item.kind = SubItemKind::IconToggle;
            item.active = active;
            item.destructive = destructive;
            return item;
        }

        SubToolbarItem MakeTextPill(SubToolbarAction action, bool active, int variant, LPCWSTR label) noexcept
        {
            SubToolbarItem item{};
            item.action = action;
            item.kind = SubItemKind::TextPill;
            item.active = active;
            item.variant = variant;
            if (label != nullptr)
            {
                wcsncpy_s(item.label, label, _TRUNCATE);
            }
            return item;
        }

        SubToolbarItem MakePxValuePill(SubToolbarAction action, float valueDip, bool active = false) noexcept
        {
            SubToolbarItem item{};
            item.action = action;
            item.kind = SubItemKind::ValuePill;
            item.active = active;
            const float rounded = std::round(valueDip);
            if (std::fabs(valueDip - rounded) < 0.05f)
            {
                swprintf_s(item.label, L"%dpx", static_cast<int>(rounded));
            }
            else
            {
                swprintf_s(item.label, L"%.1fpx", valueDip);
            }
            return item;
        }

        SubToolbarItem MakeCornerRadiusPill(float radiusDip, bool enabled) noexcept
        {
            SubToolbarItem item{};
            item.action = SubToolbarAction::CornerRadiusCycle;
            item.kind = SubItemKind::ValuePill;
            item.enabled = enabled;
            item.active = enabled && (radiusDip >= 0.5f);
            swprintf_s(item.label, L"R%d", static_cast<int>(std::lround(radiusDip)));
            return item;
        }

        void AppendStrokeGroup(std::vector<SubToolbarItem>& items, const ToolbarState& state,
                               bool useStrokeWell, bool includeWhite)
        {
            const AnnotationStyle& s = state.activeStyle;
            const bool strokeVisible = HasVisibleColor(s.color);

            SubToolbarItem well{};
            well.action = SubToolbarAction::StrokePicker;
            well.kind = useStrokeWell ? SubItemKind::StrokeWell : SubItemKind::FillWell;
            well.active = (state.activePicker == ColorPickerTarget::Stroke);
            well.swatchColor = s.color;
            well.hasColor = strokeVisible;
            well.strokeStyle = s.strokeStyle;
            items.push_back(well);

            constexpr SubToolbarAction kActions[6] =
            {
                SubToolbarAction::Color0, SubToolbarAction::Color1, SubToolbarAction::Color2,
                SubToolbarAction::Color3, SubToolbarAction::Color4, SubToolbarAction::Color5,
            };

            if (includeWhite)
            {
                const int matchIdx = strokeVisible ? MatchingPaletteIndex(kAnnotationPalette, s.color) : -1;
                for (int i = 0; i < 6; ++i)
                {
                    SubToolbarItem swatch{};
                    swatch.action = kActions[i];
                    swatch.kind = SubItemKind::ColorSwatch;
                    swatch.active = strokeVisible && (i == matchIdx);
                    swatch.variant = i;
                    swatch.swatchColor = kAnnotationPalette[static_cast<std::size_t>(i)];
                    swatch.hasColor = true;
                    items.push_back(swatch);
                }
            }
            else
            {
                const int matchIdx = strokeVisible ? MatchingPaletteIndex(kStrokeQuickPalette, s.color) : -1;
                for (int i = 0; i < 5; ++i)
                {
                    SubToolbarItem swatch{};
                    swatch.action = kActions[i];
                    swatch.kind = SubItemKind::ColorSwatch;
                    swatch.active = strokeVisible && (i == matchIdx);
                    swatch.variant = i;
                    swatch.swatchColor = kStrokeQuickPalette[static_cast<std::size_t>(i)];
                    swatch.hasColor = true;
                    items.push_back(swatch);
                }
            }
        }

        void AppendFillGroup(std::vector<SubToolbarItem>& items, const ToolbarState& state)
        {
            const AnnotationStyle& s = state.activeStyle;
            const bool fillVisible = HasVisibleColor(s.fillColor);

            SubToolbarItem fillWell{};
            fillWell.action = SubToolbarAction::FillPicker;
            fillWell.kind = SubItemKind::FillWell;
            fillWell.active = (state.activePicker == ColorPickerTarget::Fill);
            fillWell.swatchColor = s.fillColor;
            fillWell.hasColor = fillVisible;
            items.push_back(fillWell);

            items.push_back(MakeIconToggle(
                SubToolbarAction::FillModeNone,
                !fillVisible));
            items.push_back(MakeIconToggle(
                SubToolbarAction::FillModeSolid,
                fillVisible && s.fillPattern == AnnotationFillPattern::Solid));
            items.push_back(MakeIconToggle(
                SubToolbarAction::FillPatternCrossHatch,
                fillVisible && s.fillPattern == AnnotationFillPattern::CrossHatch));
            items.push_back(MakeIconToggle(
                SubToolbarAction::FillPatternDiagHatch,
                fillVisible && s.fillPattern == AnnotationFillPattern::DiagonalHatch));
        }

        void AppendStrokeWidths(std::vector<SubToolbarItem>& items, float widthDip, bool includeValuePill)
        {
            if (includeValuePill)
            {
                items.push_back(MakePxValuePill(SubToolbarAction::StrokeWidthValue, widthDip));
            }
            const int activeIdx = static_cast<int>(ClosestValueIndex(kAnnotationStrokeWidthsDip, widthDip));
            constexpr SubToolbarAction kActions[4] =
            {
                SubToolbarAction::Width0, SubToolbarAction::Width1,
                SubToolbarAction::Width2, SubToolbarAction::Width3,
            };
            for (int i = 0; i < 4; ++i)
            {
                SubToolbarItem item{};
                item.action = kActions[i];
                item.kind = SubItemKind::StrokeWidth;
                item.active = (i == activeIdx);
                item.variant = i;
                items.push_back(item);
            }
        }

        void AppendDivider(std::vector<SubToolbarItem>& items)
        {
            SubToolbarItem div{};
            div.kind = SubItemKind::Divider;
            items.push_back(div);
        }

        void BuildArrangeSubItems(const ToolbarState& state, std::vector<SubToolbarItem>& out)
        {
            out.clear();
            const auto makeBtn = [](SubToolbarAction action, bool enabled, bool destructive = false) noexcept
            {
                SubToolbarItem item = MakeIconToggle(action, false, destructive);
                item.enabled = enabled;
                return item;
            };

            const bool hasSel = (state.selectedCount >= 1);
            const bool canAlign = (state.selectedCount >= 2);
            const bool canDistribute = (state.selectedCount >= 3);

            // Group 1: Z-Order (SendToBack, SendBackward, BringForward, BringToFront)
            out.push_back(makeBtn(SubToolbarAction::OrderSendToBack, state.canSendBackward));
            out.push_back(makeBtn(SubToolbarAction::OrderSendBackward, state.canSendBackward));
            out.push_back(makeBtn(SubToolbarAction::OrderBringForward, state.canBringForward));
            out.push_back(makeBtn(SubToolbarAction::OrderBringToFront, state.canBringForward));
            AppendDivider(out);

            // Group 2: Horizontal align + distribute (Left, CenterH, Right, DistributeH)
            out.push_back(makeBtn(SubToolbarAction::AlignLeft, canAlign));
            out.push_back(makeBtn(SubToolbarAction::AlignCenterH, canAlign));
            out.push_back(makeBtn(SubToolbarAction::AlignRight, canAlign));
            out.push_back(makeBtn(SubToolbarAction::DistributeH, canDistribute));
            AppendDivider(out);

            // Group 3: Vertical align + distribute (Top, CenterV, Bottom, DistributeV)
            out.push_back(makeBtn(SubToolbarAction::AlignTop, canAlign));
            out.push_back(makeBtn(SubToolbarAction::AlignCenterV, canAlign));
            out.push_back(makeBtn(SubToolbarAction::AlignBottom, canAlign));
            out.push_back(makeBtn(SubToolbarAction::DistributeV, canDistribute));
            AppendDivider(out);

            // Group 4: Duplicate & Delete
            out.push_back(makeBtn(SubToolbarAction::DuplicateSelected, hasSel));
            out.push_back(makeBtn(SubToolbarAction::DeleteSelected, hasSel, true));
        }

        void BuildKindSubItems(AnnotationKind kind, const ToolbarState& state,
                               bool includeDelete, std::vector<SubToolbarItem>& out)
        {
            out.clear();
            const AnnotationStyle& s = state.activeStyle;

            switch (kind)
            {
            case AnnotationKind::Shape:
                out.push_back(MakeIconToggle(
                    SubToolbarAction::ShapeRect, s.shapeKind == AnnotationShapeKind::Rectangle));
                out.push_back(MakeIconToggle(
                    SubToolbarAction::ShapeEllipse, s.shapeKind == AnnotationShapeKind::Ellipse));
                out.push_back(MakeIconToggle(
                    SubToolbarAction::ShapeDiamond, s.shapeKind == AnnotationShapeKind::Diamond));
                AppendDivider(out);
                AppendStrokeGroup(out, state, true, false);
                AppendDivider(out);
                AppendStrokeWidths(out, s.strokeWidthDip, true);
                AppendDivider(out);
                AppendFillGroup(out, state);
                AppendDivider(out);
                out.push_back(MakeCornerRadiusPill(
                    s.cornerRadiusDip, s.shapeKind != AnnotationShapeKind::Ellipse));
                break;

            case AnnotationKind::Line:
                out.push_back(MakeIconToggle(
                    SubToolbarAction::LineArrow, s.lineKind == AnnotationLineKind::Arrow));
                out.push_back(MakeIconToggle(
                    SubToolbarAction::LineStraight, s.lineKind == AnnotationLineKind::Line));
                AppendDivider(out);
                AppendStrokeGroup(out, state, true, false);
                AppendDivider(out);
                AppendStrokeWidths(out, s.strokeWidthDip, true);
                break;

            case AnnotationKind::Brush:
                AppendStrokeGroup(out, state, true, false);
                AppendDivider(out);
                AppendStrokeWidths(out, s.strokeWidthDip, true);
                AppendDivider(out);
                out.push_back(MakeIconToggle(
                    SubToolbarAction::BrushHighlighterToggle, s.brushHighlighter));
                break;

            case AnnotationKind::Text:
            {
                AppendStrokeGroup(out, state, false, true);
                AppendDivider(out);
                out.push_back(MakePxValuePill(SubToolbarAction::FontSizeValue, s.fontSizeDip));
                const int fontIdx = static_cast<int>(ClosestValueIndex(kAnnotationFontSizesDip, s.fontSizeDip));
                constexpr SubToolbarAction kFontActions[4] =
                {
                    SubToolbarAction::Font0, SubToolbarAction::Font1,
                    SubToolbarAction::Font2, SubToolbarAction::Font3,
                };
                constexpr LPCWSTR kFontLabels[4] = { L"S", L"M", L"L", L"XL" };
                for (int i = 0; i < 4; ++i)
                {
                    out.push_back(MakeTextPill(kFontActions[i], i == fontIdx, i, kFontLabels[i]));
                }
                break;
            }

            case AnnotationKind::StepBadge:
            {
                AppendStrokeGroup(out, state, false, false);
                AppendDivider(out);

                constexpr SubToolbarAction kShapeActions[4] =
                {
                    SubToolbarAction::BadgeShapeOutlineCircle,
                    SubToolbarAction::BadgeShapeFilledCircle,
                    SubToolbarAction::BadgeShapeOutlineSquare,
                    SubToolbarAction::BadgeShapeFilledSquare,
                };
                for (int i = 0; i < 4; ++i)
                {
                    SubToolbarItem item{};
                    item.action = kShapeActions[i];
                    item.kind = SubItemKind::BadgeShape;
                    item.variant = i;
                    item.active = (static_cast<int>(s.badgeShape) == i);
                    out.push_back(item);
                }

                AppendDivider(out);

                constexpr SubToolbarAction kNumActions[5] =
                {
                    SubToolbarAction::BadgeNumArabic,
                    SubToolbarAction::BadgeNumRoman,
                    SubToolbarAction::BadgeNumLowerAlpha,
                    SubToolbarAction::BadgeNumUpperAlpha,
                    SubToolbarAction::BadgeNumChinese,
                };
                constexpr LPCWSTR kNumLabels[5] = { L"1", L"IV", L"a", L"A", L"\u4E09" };
                for (int i = 0; i < 5; ++i)
                {
                    out.push_back(MakeTextPill(
                        kNumActions[i],
                        static_cast<int>(s.badgeNumberStyle) == i,
                        i,
                        kNumLabels[i]));
                }

                AppendDivider(out);

                {
                    SubToolbarItem numItem{};
                    numItem.action = SubToolbarAction::BadgeNumberValue;
                    numItem.kind = SubItemKind::ValuePill;
                    swprintf_s(numItem.label, L"# %d", std::clamp(state.activeStepNumber, 1, 999));
                    out.push_back(numItem);
                }

                AppendDivider(out);

                out.push_back(MakePxValuePill(SubToolbarAction::BadgeSizeValue, s.badgeRadiusDip * 2.0f));
                const int badgeIdx = static_cast<int>(ClosestValueIndex(kAnnotationBadgeRadiiDip, s.badgeRadiusDip));
                constexpr SubToolbarAction kBadgeActions[4] =
                {
                    SubToolbarAction::BadgeSize0, SubToolbarAction::BadgeSize1,
                    SubToolbarAction::BadgeSize2, SubToolbarAction::BadgeSize3,
                };
                constexpr LPCWSTR kBadgeLabels[4] = { L"S", L"M", L"L", L"XL" };
                for (int i = 0; i < 4; ++i)
                {
                    out.push_back(MakeTextPill(kBadgeActions[i], i == badgeIdx, i, kBadgeLabels[i]));
                }
                break;
            }

            case AnnotationKind::Mosaic:
            {
                if (state.activeMode != ToolbarButton::SelectObject)
                {
                    out.push_back(MakeIconToggle(
                        SubToolbarAction::MosaicBrush, s.mosaicKind == AnnotationMosaicKind::Brush));
                    out.push_back(MakeIconToggle(
                        SubToolbarAction::MosaicRect, s.mosaicKind == AnnotationMosaicKind::Rectangle));
                    AppendDivider(out);
                }

                // Effect dropdown pill: "Mosaic" or "Blur"
                {
                    SubToolbarItem effectItem{};
                    effectItem.action = SubToolbarAction::MosaicEffectDropdown;
                    effectItem.kind = SubItemKind::DropdownPill;
                    effectItem.active = state.mosaicEffectMenuOpen;
                    wcsncpy_s(
                        effectItem.label,
                        s.mosaicEffect == AnnotationMosaicEffect::GaussianBlur ? L"Blur" : L"Mosaic",
                        _TRUNCATE);
                    out.push_back(effectItem);
                }

                AppendDivider(out);

                // Brush size: px value pill + S / M / L / XL (dimmed in Rectangle mode to prevent layout jumps)
                {
                    const bool isBrush = (s.mosaicKind == AnnotationMosaicKind::Brush);
                    SubToolbarItem pxItem = MakePxValuePill(SubToolbarAction::MosaicBrushValue, s.mosaicBrushDip);
                    pxItem.enabled = isBrush;
                    out.push_back(pxItem);

                    const int brushIdx = static_cast<int>(ClosestValueIndex(kAnnotationMosaicBrushDip, s.mosaicBrushDip));
                    constexpr SubToolbarAction kBrushActions[4] =
                    {
                        SubToolbarAction::Width0, SubToolbarAction::Width1,
                        SubToolbarAction::Width2, SubToolbarAction::Width3,
                    };
                    constexpr LPCWSTR kBrushLabels[4] = { L"S", L"M", L"L", L"XL" };
                    for (int i = 0; i < 4; ++i)
                    {
                        SubToolbarItem pill = MakeTextPill(
                            kBrushActions[i], isBrush && (i == brushIdx), i, kBrushLabels[i]);
                        pill.enabled = isBrush;
                        out.push_back(pill);
                    }
                }

                AppendDivider(out);

                // Non-interactive intensity icon + continuous slider
                {
                    SubToolbarItem iconItem{};
                    iconItem.action = SubToolbarAction::None;
                    iconItem.kind = SubItemKind::StaticIcon;
                    iconItem.variant = static_cast<int>(ToolbarIconKind::SubMosaicIntensity);
                    out.push_back(iconItem);

                    SubToolbarItem sliderItem{};
                    sliderItem.action = SubToolbarAction::MosaicIntensitySlider;
                    sliderItem.kind = SubItemKind::Slider;
                    if (s.mosaicEffect == AnnotationMosaicEffect::GaussianBlur)
                    {
                        sliderItem.sliderValue = std::clamp(
                            (s.mosaicBlurRadiusDip - kMosaicBlurMinDip)
                                / (kMosaicBlurMaxDip - kMosaicBlurMinDip),
                            0.0f, 1.0f);
                    }
                    else
                    {
                        sliderItem.sliderValue = std::clamp(
                            static_cast<float>(s.mosaicBlockDip - kMosaicBlockMinDip)
                                / static_cast<float>(kMosaicBlockMaxDip - kMosaicBlockMinDip),
                            0.0f, 1.0f);
                    }
                    out.push_back(sliderItem);
                }
                break;
            }
            }

            if (includeDelete)
            {
                AppendDivider(out);
                SubToolbarItem del = MakeIconToggle(SubToolbarAction::DeleteSelected, false, true);
                del.enabled = (state.selectedKind.has_value() || state.selectedCount > 0);
                out.push_back(del);
            }
        }

        void BuildSubRows(const ToolbarState& state,
                          std::vector<SubToolbarItem>& row1,
                          std::vector<SubToolbarItem>& row2)
        {
            row1.clear();
            row2.clear();

            if (state.activeMode == ToolbarButton::SelectObject)
            {
                BuildArrangeSubItems(state, row1);
                if (state.selectedKind.has_value())
                {
                    BuildKindSubItems(*state.selectedKind, state, false, row2);
                }
                return;
            }

            if (const std::optional<AnnotationKind> kind = ToolAnnotationKind(state.activeMode))
            {
                BuildKindSubItems(*kind, state, true, row1);
            }
        }

        void UpdateEnabledState(Toolbar& toolbar, const ToolbarState& state) noexcept
        {
            toolbar.enabled.fill(true);
            toolbar.enabled[static_cast<std::size_t>(ToolbarButton::Undo)] = state.canUndo;
            toolbar.enabled[static_cast<std::size_t>(ToolbarButton::Redo)] = state.canRedo;
        }
    }

    bool IsAnnotationToolButton(ToolbarButton button) noexcept
    {
        return ToolAnnotationKind(button).has_value();
    }

    std::optional<AnnotationKind> ToolAnnotationKind(ToolbarButton button) noexcept
    {
        switch (button)
        {
        case ToolbarButton::Shape:
            return AnnotationKind::Shape;
        case ToolbarButton::Arrow:
            return AnnotationKind::Line;
        case ToolbarButton::Brush:
            return AnnotationKind::Brush;
        case ToolbarButton::Text:
            return AnnotationKind::Text;
        case ToolbarButton::StepBadge:
            return AnnotationKind::StepBadge;
        case ToolbarButton::Mosaic:
            return AnnotationKind::Mosaic;
        default:
            return std::nullopt;
        }
    }

    bool LayoutToolbar(Toolbar& toolbar, const ToolbarState& state,
                       const RECT& selection, const RECT& bounds, int dpi) noexcept
    {
        UpdateEnabledState(toolbar, state);

        const int button = MulDiv(kButtonSize, dpi, 96);
        const int padding = MulDiv(kPadding, dpi, 96);
        const int gap = MulDiv(kGap, dpi, 96);
        const int groupGap = MulDiv(kGroupGap, dpi, 96);
        const int dividerWidth = std::max(1, MulDiv(kDividerWidth, dpi, 96));
        const int margin = MulDiv(kMargin, dpi, 96);
        const int handle = MulDiv(kHandleWidth, dpi, 96);

        constexpr std::size_t kCount = std::size(kMainOrder);
        int contentWidth = handle + groupGap + static_cast<int>(kCount) * button;
        std::size_t nextDiv = 0;
        for (std::size_t i = 1; i < kCount; ++i)
        {
            if (nextDiv < 4 && i == kDividerBefore[nextDiv])
            {
                contentWidth += groupGap * 2 + dividerWidth;
                ++nextDiv;
            }
            else
            {
                contentWidth += gap;
            }
        }
        const int width = contentWidth + padding * 2;
        const int height = button + padding * 2;

        const int selectionLeft = static_cast<int>(selection.left);
        const int selectionTop = static_cast<int>(selection.top);
        const int selectionRight = static_cast<int>(selection.right);
        const int selectionBottom = static_cast<int>(selection.bottom);
        const int boundsLeft = static_cast<int>(bounds.left);
        const int boundsTop = static_cast<int>(bounds.top);
        const int boundsRight = static_cast<int>(bounds.right);
        const int boundsBottom = static_cast<int>(bounds.bottom);

        const int maxX = std::max(boundsLeft, boundsRight - width);
        const int maxY = std::max(boundsTop, boundsBottom - height);
        const int rightAlignedX = std::clamp(selectionRight - width, boundsLeft, maxX);
        const int centeredY = std::clamp(
            selectionTop + ((selectionBottom - selectionTop) - height) / 2, boundsTop, maxY);
        const RECT candidates[] =
        {
            { rightAlignedX, selectionBottom + margin, rightAlignedX + width, selectionBottom + margin + height },
            { rightAlignedX, selectionTop - margin - height, rightAlignedX + width, selectionTop - margin },
            { selectionRight + margin, centeredY, selectionRight + margin + width, centeredY + height },
            { selectionLeft - margin - width, centeredY, selectionLeft - margin, centeredY + height },
        };

        int x = 0;
        int y = 0;
        bool found = false;
        for (const RECT& candidate : candidates)
        {
            if (candidate.left < boundsLeft || candidate.top < boundsTop
                || candidate.right > boundsRight || candidate.bottom > boundsBottom)
            {
                continue;
            }

            RECT intersection{};
            if (IntersectRect(&intersection, &candidate, &selection))
            {
                continue;
            }

            x = candidate.left;
            y = candidate.top;
            found = true;
            break;
        }
        if (!found)
        {
            x = rightAlignedX;
            y = std::clamp(selectionBottom + margin, boundsTop, maxY);
            if (y + height > boundsBottom)
            {
                y = std::clamp(selectionTop - margin - height, boundsTop, maxY);
            }
        }

        toolbar.panel = RECT{ x, y, x + width, y + height };
        int cursorX = x + padding;
        const int buttonTop = y + padding;
        toolbar.handle = RECT{ cursorX, buttonTop, cursorX + handle, buttonTop + button };
        cursorX += handle + groupGap;

        const int divInset = MulDiv(7, dpi, 96);
        nextDiv = 0;
        for (std::size_t i = 0; i < kCount; ++i)
        {
            if (i > 0)
            {
                if (nextDiv < 4 && i == kDividerBefore[nextDiv])
                {
                    cursorX += groupGap;
                    toolbar.dividers[nextDiv] = RECT{
                        cursorX, buttonTop + divInset,
                        cursorX + dividerWidth, buttonTop + button - divInset };
                    cursorX += dividerWidth + groupGap;
                    ++nextDiv;
                }
                else
                {
                    cursorX += gap;
                }
            }

            const std::size_t idx = static_cast<std::size_t>(kMainOrder[i]);
            toolbar.buttons[idx] = RECT{ cursorX, buttonTop, cursorX + button, buttonTop + button };
            cursorX += button;
        }

        LayoutSubToolbar(toolbar, state, bounds, dpi);
        return true;
    }

    void LayoutSubToolbar(Toolbar& toolbar, const ToolbarState& state,
                          const RECT& bounds, int dpi) noexcept
    {
        UpdateEnabledState(toolbar, state);
        BuildSubRows(state, toolbar.subItems, toolbar.subItems2);
        if (toolbar.subItems.empty())
        {
            toolbar.subVisible = false;
            toolbar.subPanel = RECT{};
            toolbar.subVisible2 = false;
            toolbar.subPanel2 = RECT{};
            toolbar.hoveredSub = SubToolbarAction::None;
            return;
        }

        const int subSize = MulDiv(kSubButtonSize, dpi, 96);
        const int subPillW = MulDiv(kSubPillWidth, dpi, 96);
        const int subValueW = MulDiv(kSubValueWidth, dpi, 96);
        const int subNumberW = MulDiv(kSubNumberWidth, dpi, 96);
        const int subDropdownW = MulDiv(kSubDropdownWidth, dpi, 96);
        const int subStaticIconW = MulDiv(kSubStaticIconWidth, dpi, 96);
        const int subSliderW = MulDiv(kSubSliderWidth, dpi, 96);
        const int padding = MulDiv(kPadding, dpi, 96);
        const int gap = MulDiv(kGap, dpi, 96);
        const int groupGap = MulDiv(kGroupGap, dpi, 96);
        const int dividerWidth = std::max(1, MulDiv(kDividerWidth, dpi, 96));
        const int subGap = MulDiv(kSubBarGap, dpi, 96);
        const int divInset = MulDiv(5, dpi, 96);

        const auto widthForItem = [&](const SubToolbarItem& item) noexcept -> int
        {
            if (item.action == SubToolbarAction::BadgeNumberValue) return subNumberW;
            if (item.kind == SubItemKind::DropdownPill) return subDropdownW;
            if (item.kind == SubItemKind::StaticIcon) return subStaticIconW;
            if (item.kind == SubItemKind::Slider) return subSliderW;
            if (item.kind == SubItemKind::ValuePill) return subValueW;
            if (item.kind == SubItemKind::TextPill) return subPillW;
            return subSize;
        };

        const auto measureRowWidth = [&](const std::vector<SubToolbarItem>& items) noexcept -> int
        {
            int contentWidth = 0;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                const SubToolbarItem& item = items[i];
                if (item.kind == SubItemKind::Divider)
                {
                    contentWidth += groupGap * 2 + dividerWidth;
                    continue;
                }
                if (i > 0 && items[i - 1].kind != SubItemKind::Divider)
                {
                    contentWidth += gap;
                }
                contentWidth += widthForItem(item);
            }
            return contentWidth + padding * 2;
        };

        const int subHeight = subSize + padding * 2;
        const bool hasRow2 = !toolbar.subItems2.empty();
        const int totalSubH = hasRow2 ? (subHeight * 2 + subGap) : subHeight;
        const int boundsLeft = static_cast<int>(bounds.left);
        const int boundsTop = static_cast<int>(bounds.top);
        const int boundsRight = static_cast<int>(bounds.right);
        const int boundsBottom = static_cast<int>(bounds.bottom);

        int row1Y = static_cast<int>(toolbar.panel.bottom) + subGap;
        int row2Y = row1Y + subHeight + subGap;
        if (row1Y + totalSubH > boundsBottom)
        {
            const int aboveTop = static_cast<int>(toolbar.panel.top) - subGap - totalSubH;
            if (aboveTop >= boundsTop)
            {
                row1Y = static_cast<int>(toolbar.panel.top) - subGap - subHeight;
                row2Y = row1Y - subGap - subHeight;
            }
            else
            {
                const int clampedStart = std::clamp(row1Y, boundsTop, std::max(boundsTop, boundsBottom - totalSubH));
                row1Y = clampedStart;
                row2Y = clampedStart + subHeight + subGap;
            }
        }

        const auto layoutRow = [&](std::vector<SubToolbarItem>& items, int y, RECT& outPanel) noexcept
        {
            const int rowW = measureRowWidth(items);
            const int maxX = std::max(boundsLeft, boundsRight - rowW);
            const int rowX = std::clamp(static_cast<int>(toolbar.panel.right) - rowW, boundsLeft, maxX);
            outPanel = RECT{ rowX, y, rowX + rowW, y + subHeight };

            int cursorX = rowX + padding;
            const int itemTop = y + padding;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                SubToolbarItem& item = items[i];
                if (item.kind == SubItemKind::Divider)
                {
                    cursorX += groupGap;
                    item.rect = RECT{ cursorX, itemTop + divInset, cursorX + dividerWidth, itemTop + subSize - divInset };
                    cursorX += dividerWidth + groupGap;
                    continue;
                }
                if (i > 0 && items[i - 1].kind != SubItemKind::Divider)
                {
                    cursorX += gap;
                }
                const int itemW = widthForItem(item);
                item.rect = RECT{ cursorX, itemTop, cursorX + itemW, itemTop + subSize };
                cursorX += itemW;
            }
        };

        layoutRow(toolbar.subItems, row1Y, toolbar.subPanel);
        toolbar.subVisible = true;

        if (hasRow2)
        {
            layoutRow(toolbar.subItems2, row2Y, toolbar.subPanel2);
            toolbar.subVisible2 = true;
        }
        else
        {
            toolbar.subVisible2 = false;
            toolbar.subPanel2 = RECT{};
        }
    }

    ToolbarButton HitTestToolbar(const Toolbar& toolbar, POINT point) noexcept
    {
        for (std::size_t i = 0; i < toolbar.buttons.size(); ++i)
        {
            if (PtInRect(&toolbar.buttons[i], point))
            {
                return static_cast<ToolbarButton>(i);
            }
        }
        return ToolbarButton::None;
    }

    ToolbarHit HitTestToolbarTarget(const Toolbar& toolbar, POINT point) noexcept
    {
        const ToolbarButton button = HitTestToolbar(toolbar, point);
        if (button != ToolbarButton::None)
        {
            return { ToolbarHitTarget::Button, button, SubToolbarAction::None };
        }
        if (PtInRect(&toolbar.handle, point))
        {
            return { ToolbarHitTarget::Handle, ToolbarButton::None, SubToolbarAction::None };
        }
        if (PtInRect(&toolbar.panel, point))
        {
            return { ToolbarHitTarget::Panel, ToolbarButton::None, SubToolbarAction::None };
        }

        if (toolbar.subVisible)
        {
            for (const SubToolbarItem& item : toolbar.subItems)
            {
                if (item.kind != SubItemKind::Divider && item.kind != SubItemKind::StaticIcon
                    && PtInRect(&item.rect, point))
                {
                    return { ToolbarHitTarget::SubButton, ToolbarButton::None, item.action };
                }
            }
            if (PtInRect(&toolbar.subPanel, point))
            {
                return { ToolbarHitTarget::SubPanel, ToolbarButton::None, SubToolbarAction::None };
            }
        }

        if (toolbar.subVisible2)
        {
            for (const SubToolbarItem& item : toolbar.subItems2)
            {
                if (item.kind != SubItemKind::Divider && item.kind != SubItemKind::StaticIcon
                    && PtInRect(&item.rect, point))
                {
                    return { ToolbarHitTarget::SubButton, ToolbarButton::None, item.action };
                }
            }
            if (PtInRect(&toolbar.subPanel2, point))
            {
                return { ToolbarHitTarget::SubPanel, ToolbarButton::None, SubToolbarAction::None };
            }
        }

        return {};
    }

    void DragToolbar(Toolbar& toolbar, const Toolbar& start, POINT origin, POINT current,
                     const RECT& bounds) noexcept
    {
        const int startX = static_cast<int>(start.panel.left);
        const int startY = static_cast<int>(start.panel.top);
        const int width = static_cast<int>(start.panel.right - start.panel.left);
        const int height = static_cast<int>(start.panel.bottom - start.panel.top);
        const int boundsLeft = static_cast<int>(bounds.left);
        const int boundsTop = static_cast<int>(bounds.top);
        const int boundsRight = static_cast<int>(bounds.right);
        const int boundsBottom = static_cast<int>(bounds.bottom);
        const int maxX = std::max(boundsLeft, boundsRight - width);
        const int maxY = std::max(boundsTop, boundsBottom - height);
        const int dxFromPointer = static_cast<int>(current.x) - static_cast<int>(origin.x);
        const int dyFromPointer = static_cast<int>(current.y) - static_cast<int>(origin.y);
        const int x = std::clamp(startX + dxFromPointer, boundsLeft, maxX);
        const int y = std::clamp(startY + dyFromPointer, boundsTop, maxY);
        const int dx = x - startX;
        const int dy = y - startY;

        toolbar.panel = start.panel;
        toolbar.handle = start.handle;
        toolbar.buttons = start.buttons;
        toolbar.dividers = start.dividers;
        toolbar.subPanel = start.subPanel;
        toolbar.subItems = start.subItems;
        toolbar.subPanel2 = start.subPanel2;
        toolbar.subItems2 = start.subItems2;

        OffsetRect(&toolbar.panel, dx, dy);
        OffsetRect(&toolbar.handle, dx, dy);
        for (RECT& r : toolbar.buttons)
        {
            OffsetRect(&r, dx, dy);
        }
        for (RECT& r : toolbar.dividers)
        {
            OffsetRect(&r, dx, dy);
        }
        if (toolbar.subVisible)
        {
            OffsetRect(&toolbar.subPanel, dx, dy);
            for (SubToolbarItem& item : toolbar.subItems)
            {
                OffsetRect(&item.rect, dx, dy);
            }
        }
        if (toolbar.subVisible2)
        {
            OffsetRect(&toolbar.subPanel2, dx, dy);
            for (SubToolbarItem& item : toolbar.subItems2)
            {
                OffsetRect(&item.rect, dx, dy);
            }
        }
    }

    LPCWSTR ToolbarLabel(ToolbarButton button) noexcept
    {
        switch (button)
        {
        case ToolbarButton::EditSelection:
            return L"Edit selection";
        case ToolbarButton::SelectObject:
            return L"Select annotation";
        case ToolbarButton::Shape:
            return L"Shape";
        case ToolbarButton::Arrow:
            return L"Arrow / Line";
        case ToolbarButton::Brush:
            return L"Brush";
        case ToolbarButton::Text:
            return L"Text";
        case ToolbarButton::StepBadge:
            return L"Number";
        case ToolbarButton::Mosaic:
            return L"Mosaic / Blur";
        case ToolbarButton::Undo:
            return L"Undo (Ctrl+Z)";
        case ToolbarButton::Redo:
            return L"Redo (Ctrl+Y)";
        case ToolbarButton::TextRecognition:
            return L"Recognize text";
        case ToolbarButton::Cancel:
            return L"Cancel";
        case ToolbarButton::Copy:
            return L"Copy to clipboard (Ctrl+C)";
        default:
            return L"";
        }
    }

    LPCWSTR SubToolbarLabel(SubToolbarAction action) noexcept
    {
        switch (action)
        {
        case SubToolbarAction::ShapeRect:
            return L"Rectangle";
        case SubToolbarAction::ShapeEllipse:
            return L"Ellipse";
        case SubToolbarAction::ShapeDiamond:
            return L"Diamond";
        case SubToolbarAction::StrokePicker:
            return L"Border / Stroke";
        case SubToolbarAction::FillPicker:
            return L"Fill color";
        case SubToolbarAction::FillModeNone:
            return L"No fill (transparent)";
        case SubToolbarAction::FillModeSolid:
            return L"Solid fill";
        case SubToolbarAction::FillPatternCrossHatch:
            return L"Cross-hatch fill";
        case SubToolbarAction::FillPatternDiagHatch:
            return L"Diagonal hatch fill";
        case SubToolbarAction::StrokeWidthValue:
            return L"Stroke width (scroll)";
        case SubToolbarAction::FontSizeValue:
            return L"Font size (scroll)";
        case SubToolbarAction::CornerRadiusCycle:
            return L"Corner radius (click / scroll)";
        case SubToolbarAction::LineArrow:
            return L"Arrow";
        case SubToolbarAction::LineStraight:
            return L"Line";
        case SubToolbarAction::MosaicBrush:
            return L"Brush";
        case SubToolbarAction::MosaicRect:
            return L"Rectangle";
        case SubToolbarAction::MosaicEffectDropdown:
            return L"Effect (click / scroll)";
        case SubToolbarAction::MosaicBrushValue:
            return L"Brush size (scroll)";
        case SubToolbarAction::MosaicIntensitySlider:
            return L"Strength (drag / scroll)";
        case SubToolbarAction::BrushHighlighterToggle:
            return L"Translucent highlighter";
        case SubToolbarAction::BadgeShapeOutlineCircle:
            return L"Outlined circle";
        case SubToolbarAction::BadgeShapeFilledCircle:
            return L"Filled circle";
        case SubToolbarAction::BadgeShapeOutlineSquare:
            return L"Outlined square";
        case SubToolbarAction::BadgeShapeFilledSquare:
            return L"Filled square";
        case SubToolbarAction::BadgeNumArabic:
            return L"Numbers (1, 2, 3)";
        case SubToolbarAction::BadgeNumRoman:
            return L"Roman numerals (I, II, III)";
        case SubToolbarAction::BadgeNumLowerAlpha:
            return L"Lowercase letters (a, b, c)";
        case SubToolbarAction::BadgeNumUpperAlpha:
            return L"Uppercase letters (A, B, C)";
        case SubToolbarAction::BadgeNumChinese:
            return L"Chinese numerals";
        case SubToolbarAction::BadgeNumberValue:
            return L"Step number (click to edit / scroll)";
        case SubToolbarAction::BadgeSizeValue:
            return L"Badge size (scroll)";
        case SubToolbarAction::OrderSendToBack:
            return L"Send to back";
        case SubToolbarAction::OrderSendBackward:
            return L"Send backward";
        case SubToolbarAction::OrderBringForward:
            return L"Bring forward";
        case SubToolbarAction::OrderBringToFront:
            return L"Bring to front";
        case SubToolbarAction::AlignLeft:
            return L"Align left";
        case SubToolbarAction::AlignCenterH:
            return L"Align horizontal centers";
        case SubToolbarAction::AlignRight:
            return L"Align right";
        case SubToolbarAction::AlignTop:
            return L"Align top";
        case SubToolbarAction::AlignCenterV:
            return L"Align vertical centers";
        case SubToolbarAction::AlignBottom:
            return L"Align bottom";
        case SubToolbarAction::DistributeH:
            return L"Distribute horizontally";
        case SubToolbarAction::DistributeV:
            return L"Distribute vertically";
        case SubToolbarAction::DuplicateSelected:
            return L"Duplicate (Ctrl+D)";
        case SubToolbarAction::DeleteSelected:
            return L"Delete (Del)";
        default:
            return L"";
        }
    }

    RECT MosaicSliderTrackRect(const RECT& itemRect, int dpi) noexcept
    {
        const int useDpi = dpi > 0 ? dpi : 96;
        const int pad = MulDiv(6, useDpi, 96);
        const LONG left = itemRect.left + pad;
        const LONG right = std::max(left + 1, itemRect.right - pad);
        return RECT{ left, itemRect.top, right, itemRect.bottom };
    }
}
