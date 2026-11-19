#pragma once

#include "hud/annotations.h"
#include "hud/color_picker.h"
#include "hud/inline_text_editor.h"
#include "hud/toolbar.h"
#include "hud/ui/ui_types.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace agshot
{
    struct ColorPickerOpenInfo
    {
        bool allowNone{};
        bool showStrokeStyles{};
        AnnotationStrokeStyle strokeStyle{ AnnotationStrokeStyle::Solid };
        D2D1_COLOR_F initialColor{};
        float restoreAlpha{ 1.0f };
    };

    // Owns annotation document, renderer, inline text editor, per-tool default
    // styles, multi-selection/marquee, and modifier-key transform constraints so
    // Overlay remains focused on top-level window routing.
    class AnnotationController
    {
    public:
        AnnotationController();

        void ResetSession(HWND window) noexcept;
        void ClearTransientDragState() noexcept;
        void DiscardDeviceResources() noexcept
        {
            m_renderer.DiscardDeviceResources();
            m_textEditor.DiscardDeviceResources();
        }

        const AnnotationDocument& document() const noexcept { return m_doc; }
        AnnotationDocument& document() noexcept { return m_doc; }
        InlineTextEditor& textEditor() noexcept { return m_textEditor; }
        const InlineTextEditor& textEditor() const noexcept { return m_textEditor; }

        bool empty() const noexcept { return m_doc.empty(); }
        bool hasActiveTextEditor() const noexcept { return m_textEditor.active(); }
        bool isInteracting() const noexcept { return m_interaction != InteractionKind::None; }

        AnnotationStyle& ToolStyle(AnnotationKind kind) noexcept;
        const AnnotationStyle& ToolStyle(AnnotationKind kind) const noexcept;

        std::optional<AnnotationKind> SelectedCommonKind() const noexcept;
        bool SelectionMatchesTool(ToolbarButton toolMode) const noexcept;
        void PopulateToolbarState(ToolbarState& state, ToolbarButton toolMode) const noexcept;

        LPCWSTR CursorForActiveEditor(POINT point, IDWriteFactory* dwrite, int dpi) const;
        LPCWSTR CursorForSelectedHandle(POINT point, ToolbarButton toolMode,
                                        IDWriteFactory* dwrite, int dpi) const;
        LPCWSTR CursorForCanvasPoint(POINT point, ToolbarButton toolMode, TransformModifiers mods,
                                     IDWriteFactory* dwrite, int dpi) const;

        bool InvokeSubAction(SubToolbarAction action, ToolbarButton toolMode,
                             IDWriteFactory* dwrite = nullptr, int dpi = 96);
        bool OnSubToolbarWheel(SubToolbarAction action, int delta, ToolbarButton toolMode);
        int ActiveStepNumber(ToolbarButton toolMode) const noexcept;
        void BeginLiveEditSnapshot();
        bool CommitLiveEdit();
        void ClearLiveEditSnapshot() noexcept { m_liveEditSnapshot.clear(); }
        bool SetActiveStepNumber(int number, ToolbarButton toolMode, bool commitUndo);
        bool StepActiveStepNumber(int deltaSteps, ToolbarButton toolMode);
        bool SetMosaicEffect(AnnotationMosaicEffect effect, ToolbarButton toolMode);
        bool SetMosaicIntensityNormalized(float normalized, ToolbarButton toolMode, bool commitUndo);
        bool ConsumeStepNumberEditRequest() noexcept
        {
            const bool req = m_requestStepNumberEdit;
            m_requestStepNumberEdit = false;
            return req;
        }
        ColorPickerOpenInfo ResolvePickerOpenInfo(ColorPickerTarget target, ToolbarButton toolMode) const noexcept;
        void ApplyColorFromPicker(const ColorPickerPopover& picker, ToolbarButton toolMode, bool commitUndo);

        void CommitTextEditor(HWND window);
        void BeginTextEditAt(HWND window, POINT point, IDWriteFactory* dwrite, int dpi);

        bool TryBeginSelectedHandleDrag(POINT point, ToolbarButton toolMode,
                                        IDWriteFactory* dwrite, int dpi,
                                        float handleSizeDip = 8.0f);
        bool HandleActiveTextEditorLeftDown(HWND window, POINT point, IDWriteFactory* dwrite, int dpi,
                                            bool& outNeedToolbarSync, bool& outNeedImeSync,
                                            KeyModifiers mods = QueryCurrentKeyModifiers());
        void OnCanvasLeftButtonDown(HWND window, POINT point, ToolbarButton toolMode,
                                    const RECT& selectionRect, TransformModifiers mods,
                                    bool isDoubleClick, IDWriteFactory* dwrite, int dpi,
                                    bool& outNeedToolbarSync, bool& outNeedImeSync);
        void OnMouseMove(HWND window, POINT point, TransformModifiers mods,
                         const RECT& canvasBounds, IDWriteFactory* dwrite, int dpi);
        void OnLeftButtonUp(POINT point, const RECT& canvasBounds, IDWriteFactory* dwrite, int dpi,
                            bool& outNeedToolbarSync);
        bool OnRightButtonUp(HWND window, POINT point, IDWriteFactory* dwrite, int dpi,
                             bool& outNeedToolbarSync, bool& outNeedImeSync);
        bool OnModifiersChanged(POINT point, TransformModifiers mods,
                                const RECT& canvasBounds, IDWriteFactory* dwrite, int dpi);

        bool OnKeyDown(HWND window, WPARAM key, ToolbarButton toolMode,
                       IDWriteFactory* dwrite, int dpi,
                       bool& outNeedToolbarSync, bool& outNeedImeSync,
                       KeyModifiers mods = QueryCurrentKeyModifiers());
        bool OnChar(HWND window, wchar_t ch, IDWriteFactory* dwrite, int dpi);
        bool OnImeStartComposition(HWND window, IDWriteFactory* dwrite, int dpi) const;
        bool OnImeComposition(HWND window, LPARAM lparam, IDWriteFactory* dwrite, int dpi);
        bool OnImeEndComposition(HWND window) noexcept;

        void DrawCanvasContent(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                               ID2D1StrokeStyle* roundStroke, IDWriteFactory* dwrite,
                               ID2D1Bitmap* sourceBitmap, int dpi);
        void DrawOverlayChrome(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                               IDWriteFactory* dwrite, int dpi,
                               const HudBrushes& brushes);
        void ExportToClipboard(HWND owner, const RenderDevice& device,
                               const ScreenImage& sourceImage,
                               const RECT& bounds, const RECT& selection) const;

    private:
        enum class InteractionKind
        {
            None,
            TextEditor,
            DrawDraft,
            Transform,
            MarqueeSelect,
        };

        void ApplyTransformDrag(POINT point, TransformModifiers mods,
                                const RECT& canvasBounds, IDWriteFactory* dwrite, int dpi);
        void UpdateMarqueeSelection(POINT point, TransformModifiers mods,
                                    IDWriteFactory* dwrite, int dpi);

        AnnotationDocument m_doc;
        AnnotationRenderer m_renderer;
        InlineTextEditor m_textEditor;
        std::array<AnnotationStyle, 6> m_toolStyles{};

        InteractionKind m_interaction{ InteractionKind::None };
        bool m_hasDraft{};
        AnnotationItem m_draft{};
        POINT m_dragOrigin{};
        POINT m_lastPointer{};
        D2D1_RECT_F m_marqueeRect{};
        std::vector<std::uint64_t> m_marqueeBaseIds;
        bool m_marqueeToggle{};

        AnnotationHandle m_dragHandle{ AnnotationHandle::None };
        std::uint64_t m_dragPrimaryId{};
        AnnotationItem m_dragPrimaryStart{};
        std::vector<std::pair<std::uint64_t, AnnotationItem>> m_dragMultiStart;
        std::vector<AnnotationItem> m_snapshotBeforeTransform;

        std::vector<AnnotationItem> m_liveEditSnapshot;
        std::optional<int> m_nextStepNumberOverride;
        bool m_requestStepNumberEdit{};
        ui::WheelDetentAccumulator m_subWheelAccum{};
        std::vector<float> m_snapGuidesX;
        std::vector<float> m_snapGuidesY;
        RECT m_snapCanvasBounds{};
    };
}
