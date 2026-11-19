#pragma once

#include "core/ocr_runtime.h"
#include "core/screen_capture.h"
#include "hud/hud_theme.h"
#include "hud/text_edit_utils.h"

#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace agshot
{
    inline constexpr UINT_PTR kOcrSelectionTimer = 0xA651;

    // Self-contained OCR text panel and spinner overlay shown when the HUD is
    // in text-recognition mode. Owned directly as a value member by Overlay so
    // its lifetime and resource cleanup remain deterministic on the UI thread;
    // only m_completion uses shared_ptr as the cross-thread envelope to
    // OcrRuntime.
    class OcrTextPanel
    {
    public:
        OcrTextPanel() = default;

        void ResetSession() noexcept;
        void CancelSession(HWND window) noexcept;
        void Destroy(HWND window) noexcept;

        void StopSelection(HWND window) noexcept;
        void CancelInteractions(HWND window) noexcept;

        void InvalidateLayout() noexcept;
        void InvalidateSpinner() noexcept;

        bool pending() const noexcept { return m_pending; }
        bool capturingInput() const noexcept { return m_scrollDragging || m_selecting; }
        const std::wstring& text() const noexcept { return m_text; }
        std::size_t caret() const noexcept { return m_caret; }
        std::size_t anchor() const noexcept { return m_anchor; }
        void SetRecognizedText(std::wstring text) noexcept;

        void Enter(HWND window, OcrRuntime* runtime, const ScreenImage& image,
                   const RECT& bounds, const RECT& selection);
        bool HandleResultMessage(LPARAM lparam);

        bool HasScrollbar(const RECT& box, int dpi, IDWriteFactory* dwrite,
                          IDWriteTextFormat* format) const;
        RECT ScrollTrack(const RECT& box, int dpi) const noexcept;

        bool OnLeftButtonDown(HWND window, const RECT& box, POINT point, int dpi,
                              IDWriteFactory* dwrite, IDWriteTextFormat* format,
                              bool& needsRender,
                              KeyModifiers mods = QueryCurrentKeyModifiers());
        bool OnMouseMove(const RECT& box, POINT point, int dpi,
                         IDWriteFactory* dwrite, IDWriteTextFormat* format);
        bool OnLeftButtonUp(HWND window, const RECT& box, POINT point, int dpi,
                            IDWriteFactory* dwrite, IDWriteTextFormat* format);
        bool OnSelectionTimer(const RECT& box, int dpi,
                              IDWriteFactory* dwrite, IDWriteTextFormat* format);
        void OnWheel(const RECT& box, int delta, int dpi, int textSizeDip,
                     IDWriteFactory* dwrite, IDWriteTextFormat* format);
        void ShowContextMenu(HWND window, const RECT& box, POINT point, int dpi,
                             IDWriteFactory* dwrite, IDWriteTextFormat* format);
        bool OnKeyDown(HWND window, const RECT& box, WPARAM key, int dpi,
                       IDWriteFactory* dwrite, IDWriteTextFormat* format,
                       bool& shouldClose, bool& needsRender,
                       KeyModifiers mods = QueryCurrentKeyModifiers());
        bool CopyText(HWND window) const;

        void Draw(ID2D1DeviceContext* context, IDWriteFactory* dwrite,
                  IDWriteTextFormat* format, const RECT& box, int dpi,
                  const HudBrushes& brushes);

#ifdef AGSHOT_TOOLBAR_PROOF
        void ConfigureProofState(const RECT& selection, float scrollProbe,
                                 bool spinnerProbe);
#endif

    private:
        RECT ScrollThumb(const RECT& box, int dpi, IDWriteFactory* dwrite,
                         IDWriteTextFormat* format) const;
        winrt::com_ptr<IDWriteTextLayout> TextLayout(const RECT& box, int dpi,
                                                     IDWriteFactory* dwrite,
                                                     IDWriteTextFormat* format) const;
        std::size_t PositionFromPoint(const RECT& box, POINT point, int dpi,
                                      IDWriteFactory* dwrite,
                                      IDWriteTextFormat* format) const;
        float ScrollMaximum(const RECT& box, int dpi, IDWriteFactory* dwrite,
                            IDWriteTextFormat* format) const;
        void EnsureCaretVisible(const RECT& box, int dpi, IDWriteFactory* dwrite,
                                IDWriteTextFormat* format);
        bool MoveCaret(const RECT& box, WPARAM key, int dpi,
                       IDWriteFactory* dwrite, IDWriteTextFormat* format,
                       KeyModifiers mods);
        bool UpdateSelection(const RECT& box, POINT point, int dpi,
                             IDWriteFactory* dwrite, IDWriteTextFormat* format);
        bool EnsureSpinnerBitmap(ID2D1DeviceContext* context, int dpi);
        void DrawSpinner(ID2D1DeviceContext* context, const RECT& box, int dpi);

        std::wstring m_text;
        std::shared_ptr<OcrCompletion> m_completion;
        bool m_pending{};
        LONGLONG m_pendingStart{};
        RECT m_cacheSelection{};
        bool m_cacheValid{};
        std::size_t m_caret{};
        std::size_t m_anchor{};
        TextGranularSelection m_granularSelect{};
        MultiClickTracker m_clickTracker{};
        bool m_selecting{};
        bool m_scrollDragging{};
        POINT m_scrollDragOrigin{};
        float m_scrollDragStart{};
        POINT m_selectionPoint{};
        float m_scrollY{};

        mutable winrt::com_ptr<IDWriteTextLayout> m_layout;
        mutable std::wstring m_layoutText;
        mutable float m_layoutWidth{ -1.0f };
        mutable std::size_t m_layoutHighlightFirst{};
        mutable std::size_t m_layoutHighlightLast{};
        mutable std::vector<DWRITE_HIT_TEST_METRICS> m_hitMetrics;

        winrt::com_ptr<ID2D1Bitmap1> m_spinnerBitmap;
        int m_spinnerDpi{};
    };
}
