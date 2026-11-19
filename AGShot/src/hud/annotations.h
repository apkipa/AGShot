#pragma once

#include "core/screen_capture.h"
#include "hud/hud_theme.h"
#include "hud/render_device.h"
#include "hud/text_edit_utils.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <d2d1_1.h>
#include <dwrite.h>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <winrt/base.h>

namespace agshot
{
    enum class AnnotationKind
    {
        Shape,
        Line,
        Brush,
        Text,
        StepBadge,
        Mosaic,
    };

    enum class AnnotationShapeKind
    {
        Rectangle,
        Ellipse,
        Diamond,
    };

    enum class AnnotationStrokeStyle
    {
        Solid,
        Dashed,
        Dotted,
    };

    enum class AnnotationFillPattern
    {
        Solid,
        CrossHatch,
        DiagonalHatch,
    };

    enum class AnnotationLineKind
    {
        Arrow,
        Line,
    };

    enum class AnnotationMosaicKind
    {
        Brush,
        Rectangle,
    };

    enum class AnnotationMosaicEffect
    {
        Pixelate,
        GaussianBlur,
    };

    enum class AnnotationBadgeShape
    {
        OutlineCircle,
        FilledCircle,
        OutlineSquare,
        FilledSquare,
    };

    enum class AnnotationBadgeNumberStyle
    {
        Arabic,
        Roman,
        LowerAlpha,
        UpperAlpha,
        Chinese,
    };

    enum class AnnotationHandle
    {
        None,
        Body,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
        LineStart,
        LineEnd,
    };

    using TransformModifiers = KeyModifiers;

    inline constexpr float kVisibleAlphaThreshold = 0.005f;

    inline constexpr bool HasVisibleColor(const D2D1_COLOR_F& c) noexcept
    {
        return c.a >= kVisibleAlphaThreshold;
    }

    inline constexpr std::array<D2D1_COLOR_F, 6> kAnnotationPalette =
    {
        D2D1_COLOR_F{ 0.94f, 0.25f, 0.25f, 1.0f }, // Red
        D2D1_COLOR_F{ 0.13f, 0.77f, 0.37f, 1.0f }, // Green
        D2D1_COLOR_F{ 0.18f, 0.49f, 0.94f, 1.0f }, // Blue
        D2D1_COLOR_F{ 0.98f, 0.80f, 0.08f, 1.0f }, // Yellow
        D2D1_COLOR_F{ 0.98f, 0.98f, 0.99f, 1.0f }, // White
        D2D1_COLOR_F{ 0.08f, 0.09f, 0.11f, 1.0f }, // Black
    };

    inline constexpr std::array<D2D1_COLOR_F, 5> kStrokeQuickPalette =
    {
        D2D1_COLOR_F{ 0.94f, 0.25f, 0.25f, 1.0f }, // Red
        D2D1_COLOR_F{ 0.13f, 0.77f, 0.37f, 1.0f }, // Green
        D2D1_COLOR_F{ 0.18f, 0.49f, 0.94f, 1.0f }, // Blue
        D2D1_COLOR_F{ 0.98f, 0.80f, 0.08f, 1.0f }, // Yellow
        D2D1_COLOR_F{ 0.08f, 0.09f, 0.11f, 1.0f }, // Black
    };

    inline constexpr D2D1_COLOR_F kFillQuickWhite{ 0.98f, 0.98f, 0.99f, 1.0f };
    inline constexpr D2D1_COLOR_F kFillQuickHighlight{ 0.98f, 0.80f, 0.08f, 0.32f };
    inline constexpr D2D1_COLOR_F kFillHighlightRed{ 0.94f, 0.25f, 0.25f, 0.28f };
    inline constexpr D2D1_COLOR_F kFillHighlightYellow{ 0.98f, 0.80f, 0.08f, 0.32f };
    inline constexpr D2D1_COLOR_F kFillSolidRed{ 0.94f, 0.25f, 0.25f, 1.0f };

    inline constexpr std::array<float, 4> kAnnotationStrokeWidthsDip = { 2.0f, 3.0f, 5.0f, 8.0f };
    inline constexpr std::array<float, 4> kAnnotationFontSizesDip = { 14.0f, 20.0f, 28.0f, 38.0f };
    inline constexpr std::array<float, 4> kAnnotationBadgeRadiiDip = { 10.0f, 13.0f, 17.0f, 21.0f };
    inline constexpr int kMosaicBlockMinDip = 2;
    inline constexpr int kMosaicBlockMaxDip = 32;
    inline constexpr float kMosaicBlurMinDip = 0.5f;
    inline constexpr float kMosaicBlurMaxDip = 36.0f;
    inline constexpr std::array<float, 4> kAnnotationMosaicBrushDip = { 16.0f, 24.0f, 36.0f, 52.0f };

    // Pure RGBA style state: alpha < kVisibleAlphaThreshold means invisible/disabled,
    // while RGB channels remain preserved for editing and restoring alpha later.
    struct AnnotationStyle
    {
        D2D1_COLOR_F color{ 0.94f, 0.25f, 0.25f, 1.0f };
        D2D1_COLOR_F fillColor{ 0.94f, 0.25f, 0.25f, 0.0f };
        float lastStrokeAlpha{ 1.0f };
        float lastFillAlpha{ 1.0f };
        bool brushHighlighter{};
        float strokeWidthDip{ 3.0f };
        float cornerRadiusDip{ 0.0f };
        float fontSizeDip{ 20.0f };
        float badgeRadiusDip{ 13.0f };
        int mosaicBlockDip{ 10 };
        float mosaicBlurRadiusDip{ 12.0f };
        float mosaicBrushDip{ 24.0f };
        AnnotationShapeKind shapeKind{ AnnotationShapeKind::Rectangle };
        AnnotationStrokeStyle strokeStyle{ AnnotationStrokeStyle::Solid };
        AnnotationFillPattern fillPattern{ AnnotationFillPattern::Solid };
        AnnotationLineKind lineKind{ AnnotationLineKind::Arrow };
        AnnotationMosaicKind mosaicKind{ AnnotationMosaicKind::Brush };
        AnnotationMosaicEffect mosaicEffect{ AnnotationMosaicEffect::Pixelate };
        AnnotationBadgeShape badgeShape{ AnnotationBadgeShape::FilledCircle };
        AnnotationBadgeNumberStyle badgeNumberStyle{ AnnotationBadgeNumberStyle::Arabic };
    };

    // One vector or mosaic annotation stored in Overlay client physical pixels,
    // independent of the active selection rectangle.
    struct AnnotationItem
    {
        std::uint64_t id{};
        AnnotationKind kind{ AnnotationKind::Shape };
        AnnotationStyle style{};
        D2D1_RECT_F rect{};
        D2D1_POINT_2F start{};
        D2D1_POINT_2F end{};
        std::vector<D2D1_POINT_2F> points;
        D2D1_POINT_2F origin{};
        float textMaxWidth{};
        std::wstring text;
        int stepNumber{ 1 };
    };

    D2D1_RECT_F NormalizeAnnotationRect(D2D1_RECT_F rect) noexcept;
    D2D1_RECT_F ConstrainDraftRect(POINT origin, POINT current, bool lockSquare, bool fromCenter) noexcept;
    D2D1_POINT_2F ConstrainLineEndpoint(D2D1_POINT_2F anchor, D2D1_POINT_2F freePt, bool snapAngle) noexcept;
    winrt::com_ptr<IDWriteTextLayout> CreateAnnotationTextLayout(
        IDWriteFactory* dwrite, const AnnotationItem& item, int dpi);
    D2D1_RECT_F AnnotationBounds(const AnnotationItem& item, IDWriteFactory* dwrite, int dpi);
    std::array<D2D1_RECT_F, 8> AnnotationBoxHandles(const D2D1_RECT_F& bounds, float handleSize) noexcept;
    AnnotationHandle HitTestAnnotationHandle(
        const AnnotationItem& item, POINT point, IDWriteFactory* dwrite, int dpi, float handleSize);
    bool HitTestAnnotationBody(
        const AnnotationItem& item, POINT point, IDWriteFactory* dwrite, int dpi);
    void MoveAnnotation(AnnotationItem& item, float dx, float dy) noexcept;
    AnnotationItem DragAnnotationHandle(
        const AnnotationItem& start, AnnotationHandle handle, POINT origin, POINT current,
        TransformModifiers mods = {}) noexcept;
    LPCWSTR CursorForAnnotationHandle(AnnotationHandle handle) noexcept;
    std::wstring FormatStepBadgeLabel(int number, AnnotationBadgeNumberStyle style);

    enum class AnnotationZOrderOp
    {
        SendToBack,
        SendBackward,
        BringForward,
        BringToFront,
    };

    // Document holding committed annotations along with Undo/Redo history and multi-selection.
    class AnnotationDocument
    {
    public:
        static constexpr std::size_t kMaxUndoDepth = 128;

        void Clear() noexcept;
        bool empty() const noexcept { return m_items.empty(); }
        const std::vector<AnnotationItem>& items() const noexcept { return m_items; }

        bool canUndo() const noexcept { return !m_undoStack.empty(); }
        bool canRedo() const noexcept { return !m_redoStack.empty(); }

        std::uint64_t selectedId() const noexcept { return m_selectedIds.empty() ? 0 : m_selectedIds.back(); }
        const std::vector<std::uint64_t>& selectedIds() const noexcept { return m_selectedIds; }
        std::size_t selectionCount() const noexcept { return m_selectedIds.size(); }
        bool IsSelected(std::uint64_t id) const noexcept;

        void ClearSelection() noexcept { m_selectedIds.clear(); }
        void SetSelectedId(std::uint64_t id) noexcept;
        void SetSelectedIds(std::vector<std::uint64_t> ids);
        void ToggleSelectedId(std::uint64_t id);
        void SelectAll();

        const AnnotationItem* FindItem(std::uint64_t id) const noexcept;
        AnnotationItem* FindItem(std::uint64_t id) noexcept;
        const AnnotationItem* SelectedItem() const noexcept;

        int NextStepNumber() const noexcept;

        std::uint64_t AddItem(AnnotationItem item);
        bool DeleteItem(std::uint64_t id);
        bool DeleteSelected();
        bool CanBringSelectedForward() const noexcept;
        bool CanSendSelectedBackward() const noexcept;
        bool ReorderSelected(AnnotationZOrderOp op);
        bool DuplicateSelected(float dx, float dy);
        bool UpdateItem(std::uint64_t id, const AnnotationItem& updated);
        void CommitPreviousState(std::vector<AnnotationItem> previousItems);

        bool Undo() noexcept;
        bool Redo() noexcept;

        std::uint64_t HitTestTopmost(POINT point, IDWriteFactory* dwrite, int dpi) const;
        std::vector<std::uint64_t> HitTestRect(const D2D1_RECT_F& marquee, IDWriteFactory* dwrite, int dpi) const;

    private:
        void PushUndoSnapshot(std::vector<AnnotationItem> snapshot);
        void ReconcileSelection() noexcept;

        std::vector<AnnotationItem> m_items;
        std::vector<std::vector<AnnotationItem>> m_undoStack;
        std::vector<std::vector<AnnotationItem>> m_redoStack;
        std::uint64_t m_nextId{ 1 };
        std::vector<std::uint64_t> m_selectedIds;
    };

    // Shared Direct2D renderer for live HUD preview and offscreen clipboard export,
    // using GPU Direct2D effects (CLSID_D2D1Scale / CLSID_D2D1GaussianBlur) for
    // Mosaic and Gaussian Blur annotations.
    class AnnotationRenderer
    {
    public:
        void DiscardDeviceResources() noexcept;

        void DrawAnnotations(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                             ID2D1StrokeStyle* roundStroke, IDWriteFactory* dwrite,
                             ID2D1Bitmap* sourceBitmap, int dpi,
                             const std::vector<AnnotationItem>& items,
                             const AnnotationItem* draftItem,
                             std::span<const std::uint64_t> selectedIds,
                             bool showSelectionChrome,
                             std::uint64_t hiddenId = 0);

        void DrawSelectionChrome(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                                 IDWriteFactory* dwrite, int dpi,
                                 const std::vector<AnnotationItem>& items,
                                 std::span<const std::uint64_t> selectedIds,
                                 std::uint64_t hiddenId = 0);

        // Composites the cropped selection from "sourceImage" with "items"
        // and places the resulting CF_DIB on the clipboard.
        void ExportSelectionToClipboard(HWND owner, const RenderDevice& device,
                                        const ScreenImage& sourceImage,
                                        const RECT& bounds, const RECT& selection,
                                        const std::vector<AnnotationItem>& items) const;

    private:
        struct CachedPolyline
        {
            std::size_t pointCount{};
            D2D1_POINT_2F first{};
            D2D1_POINT_2F last{};
            winrt::com_ptr<ID2D1PathGeometry> geometry;
        };

        struct CachedTextLayout
        {
            IDWriteFactory* factory{};
            int dpi{};
            float fontSizeDip{};
            float maxWidth{};
            std::wstring text;
            winrt::com_ptr<IDWriteTextLayout> layout;
        };

        ID2D1SolidColorBrush* ScratchBrush(ID2D1DeviceContext* context, const D2D1_COLOR_F& color);
        ID2D1StrokeStyle* EnsureStrokeStyle(ID2D1Factory1* factory, ID2D1StrokeStyle* roundStroke,
                                            AnnotationStrokeStyle style);
        ID2D1StrokeStyle* EnsureSelectionDashStroke(ID2D1Factory1* factory);
        IDWriteTextFormat* EnsureBadgeFormat(IDWriteFactory* dwrite, float fontSizePx);
        winrt::com_ptr<ID2D1PathGeometry> EnsurePolylineGeometry(
            ID2D1Factory1* factory, const AnnotationItem& item);
        winrt::com_ptr<IDWriteTextLayout> EnsureTextLayout(
            IDWriteFactory* dwrite, const AnnotationItem& item, int dpi);

        winrt::com_ptr<ID2D1Brush> CreateGpuMosaicBrush(
            ID2D1DeviceContext* context, ID2D1Bitmap* sourceBitmap,
            AnnotationMosaicEffect effect, float strengthPx);

        ID2D1DeviceContext* m_brushContext{};
        winrt::com_ptr<ID2D1SolidColorBrush> m_scratchBrush;

        ID2D1Factory1* m_strokeFactory{};
        winrt::com_ptr<ID2D1StrokeStyle> m_dashedStroke;
        winrt::com_ptr<ID2D1StrokeStyle> m_dottedStroke;
        winrt::com_ptr<ID2D1StrokeStyle> m_selectionDashStroke;
        std::unordered_map<std::uint64_t, CachedPolyline> m_polylineCache;

        IDWriteFactory* m_badgeDwrite{};
        float m_badgeFontSizePx{ -1.0f };
        winrt::com_ptr<IDWriteTextFormat> m_badgeFormat;
        std::unordered_map<std::uint64_t, CachedTextLayout> m_textLayoutCache;
    };
}
