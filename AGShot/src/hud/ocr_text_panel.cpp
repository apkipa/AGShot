#include "pch.h"

#include "hud/ocr_text_panel.h"

#include "core/error.h"
#include "hud/spinner_effect.h"

namespace agshot
{
    namespace
    {
        constexpr int kOcrScrollWidth = 6;
        constexpr int kOcrScrollInset = 5;
        constexpr int kOcrScrollGutter = 10;

        constexpr UINT kOcrSelectionTimerMs = 30;

        constexpr float kDegreesToRadians = 3.14159265358979f / 180.0f;
        constexpr float kOcrSpinnerSweepDeg = 270.0f;
        constexpr float kOcrSpinnerRadius = 9.0f;
        constexpr float kOcrSpinnerStroke = 2.0f;
        constexpr int kOcrSpinnerPeriodMs = 900;
    }

    void OcrTextPanel::ResetSession() noexcept
    {
        m_text.clear();
        m_completion.reset();
        m_pending = false;
        m_cacheValid = false;
        m_caret = 0;
        m_anchor = 0;
        m_granularSelect.Reset();
        m_clickTracker.Reset();
        m_selecting = false;
        m_scrollDragging = false;
        m_scrollY = 0.0f;
    }

    void OcrTextPanel::SetRecognizedText(std::wstring text) noexcept
    {
        InvalidateLayout();
        m_pending = false;
        m_text = std::move(text);
        m_caret = 0;
        m_anchor = 0;
        m_granularSelect.Reset();
        m_scrollY = 0.0f;
    }

    void OcrTextPanel::CancelSession(HWND window) noexcept
    {
        StopSelection(window);
        m_scrollDragging = false;
        m_completion.reset();
        m_pending = false;
        m_text.clear();
        if (m_text.capacity() > 4096)
        {
            m_text.shrink_to_fit();
        }
        InvalidateLayout();
        m_cacheValid = false;
    }

    void OcrTextPanel::Destroy(HWND window) noexcept
    {
        StopSelection(window);
        m_scrollDragging = false;
        m_completion.reset();
        m_pending = false;
        InvalidateLayout();
        m_text.clear();
        m_text.shrink_to_fit();
        InvalidateSpinner();
    }

    void OcrTextPanel::StopSelection(HWND window) noexcept
    {
        m_selecting = false;
        if (window != nullptr)
        {
            KillTimer(window, kOcrSelectionTimer);
        }
    }

    void OcrTextPanel::CancelInteractions(HWND window) noexcept
    {
        m_scrollDragging = false;
        StopSelection(window);
    }

    void OcrTextPanel::InvalidateLayout() noexcept
    {
        m_layout = nullptr;
        m_layoutText.clear();
        m_layoutWidth = -1.0f;
        m_layoutHighlightFirst = 0;
        m_layoutHighlightLast = 0;
        m_hitMetrics.clear();
    }

    void OcrTextPanel::InvalidateSpinner() noexcept
    {
        m_spinnerBitmap = nullptr;
        m_spinnerDpi = 0;
    }

    void OcrTextPanel::Enter(HWND window, OcrRuntime* runtime, const ScreenImage& image,
                             const RECT& bounds, const RECT& selection)
    {
        if (!m_cacheValid || !EqualRect(&m_cacheSelection, &selection))
        {
            m_cacheSelection = selection;
            m_cacheValid = true;
            m_caret = 0;
            m_anchor = 0;
            m_granularSelect.Reset();
            m_scrollY = 0.0f;

            const bool canRecognize = runtime != nullptr && runtime->available();
            m_pending = false;
            m_text = canRecognize
                ? std::wstring{}
                : L"OCR models are not installed.\r\nOpen Settings -> OCR to import a model pack.";
            if (canRecognize && !image.empty())
            {
                const RECT& imageRegion = image.region();
                const RECT crop{
                    selection.left + bounds.left - imageRegion.left,
                    selection.top + bounds.top - imageRegion.top,
                    selection.right + bounds.left - imageRegion.left,
                    selection.bottom + bounds.top - imageRegion.top };

                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                m_completion = runtime->SubmitRegion(
                    window, image.pixels(), image.width(), image.height(),
                    image.stride(), crop);
                m_pending = static_cast<bool>(m_completion);
                m_pendingStart = now.QuadPart;
            }
        }

        m_selecting = false;
        m_scrollDragging = false;
    }

    bool OcrTextPanel::HandleResultMessage(LPARAM lparam)
    {
        using CompletionMessage = std::shared_ptr<OcrCompletion>;
        std::unique_ptr<CompletionMessage> completionMessage{
            reinterpret_cast<CompletionMessage*>(lparam) };
        const std::shared_ptr<OcrCompletion> completion =
            completionMessage ? *completionMessage : nullptr;
        if (!completion || completion != m_completion)
        {
            return false;
        }

        bool ok{};
        std::wstring text;
        std::wstring error;
        {
            std::lock_guard lock(completion->mutex);
            ok = completion->ok;
            text = completion->text;
            error = completion->error;
        }

        m_pending = false;
        m_text = ok ? std::move(text) : L"OCR failed: " + std::move(error);
        if (m_text.empty() && ok)
        {
            m_text = L"No text found.";
        }
        m_caret = 0;
        m_anchor = 0;
        m_granularSelect.Reset();
        m_scrollY = 0.0f;
        return true;
    }

    RECT OcrTextPanel::ScrollTrack(const RECT& box, int dpi) const noexcept
    {
        const LONG width = box.right - box.left;
        const LONG height = box.bottom - box.top;
        const LONG verticalInset = std::min<LONG>(MulDiv(9, dpi, 96),
            std::max<LONG>(0, (height - 1) / 2));
        const LONG right = box.right - std::min<LONG>(MulDiv(kOcrScrollInset, dpi, 96),
            std::max<LONG>(0, width - 1));
        const LONG left = std::max(box.left, right - MulDiv(kOcrScrollWidth, dpi, 96));
        return RECT{ left, box.top + verticalInset, right, box.bottom - verticalInset };
    }

    bool OcrTextPanel::HasScrollbar(const RECT& box, int dpi, IDWriteFactory* dwrite,
                                    IDWriteTextFormat* format) const
    {
        return ScrollMaximum(box, dpi, dwrite, format) > 0.0f;
    }

    RECT OcrTextPanel::ScrollThumb(const RECT& box, int dpi, IDWriteFactory* dwrite,
                                   IDWriteTextFormat* format) const
    {
        const float maximum = ScrollMaximum(box, dpi, dwrite, format);
        if (maximum <= 0.0f)
        {
            return RECT{};
        }

        const RECT track = ScrollTrack(box, dpi);
        const float trackHeight = static_cast<float>(track.bottom - track.top);
        const float viewportHeight = std::max(1.0f, trackHeight);
        const float contentHeight = viewportHeight + maximum;
        const float thumbHeight = std::min(trackHeight, std::max(ScaleByDpi(18, dpi),
            trackHeight * viewportHeight / contentHeight));
        const float thumbTop = track.top + (trackHeight - thumbHeight)
            * m_scrollY / maximum;
        return RECT{ track.left, static_cast<LONG>(thumbTop), track.right,
                     static_cast<LONG>(thumbTop + thumbHeight) };
    }

    winrt::com_ptr<IDWriteTextLayout> OcrTextPanel::TextLayout(
        const RECT& box, int dpi, IDWriteFactory* dwrite, IDWriteTextFormat* format) const
    {
        if (dwrite == nullptr || format == nullptr)
        {
            return nullptr;
        }
        const float padding = ScaleByDpi(9, dpi);
        const float width = std::max(1.0f, static_cast<float>(box.right - box.left)
            - padding * 2.0f - ScaleByDpi(kOcrScrollGutter, dpi));

        if (!m_layout || m_layoutText != m_text || m_layoutWidth != width)
        {
            m_layout = nullptr;
            AGSHOT_CHECK_HR(dwrite->CreateTextLayout(
                m_text.c_str(), static_cast<UINT32>(m_text.size()),
                format, width, 100000.0f, m_layout.put()));
            AGSHOT_CHECK_HR(m_layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP));
            m_layoutText = m_text;
            m_layoutWidth = width;
            m_layoutHighlightFirst = 0;
            m_layoutHighlightLast = 0;
        }
        return m_layout;
    }

    std::size_t OcrTextPanel::PositionFromPoint(
        const RECT& box, POINT point, int dpi,
        IDWriteFactory* dwrite, IDWriteTextFormat* format) const
    {
        if (m_text.empty())
        {
            return 0;
        }
        auto layout = TextLayout(box, dpi, dwrite, format);
        if (!layout)
        {
            return m_text.size();
        }

        const float padding = ScaleByDpi(9, dpi);
        const LONG textLeft = box.left + static_cast<LONG>(padding);
        const LONG textRight = box.right - static_cast<LONG>(padding + ScaleByDpi(kOcrScrollGutter, dpi));
        const LONG textTop = box.top + static_cast<LONG>(padding);
        const LONG textBottom = box.bottom - static_cast<LONG>(padding);
        const float x = std::max(0.0f, static_cast<float>(
            std::clamp(point.x, textLeft, std::max(textLeft, textRight)) - box.left) - padding);
        const float y = std::max(0.0f, static_cast<float>(
            std::clamp(point.y, textTop, std::max(textTop, textBottom - 1)) - box.top)
            - padding + m_scrollY);
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        if (SUCCEEDED(layout->HitTestPoint(x, y, &trailing, &inside, &metrics)))
        {
            const std::size_t position = metrics.textPosition + (trailing ? metrics.length : 0);
            return std::min(position, m_text.size());
        }
        return m_text.size();
    }

    float OcrTextPanel::ScrollMaximum(const RECT& box, int dpi,
                                      IDWriteFactory* dwrite, IDWriteTextFormat* format) const
    {
        if (m_text.empty())
        {
            return 0.0f;
        }
        auto layout = TextLayout(box, dpi, dwrite, format);
        if (!layout)
        {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS metrics{};
        AGSHOT_CHECK_HR(layout->GetMetrics(&metrics));
        const float viewportHeight = std::max(1.0f,
            static_cast<float>(box.bottom - box.top) - ScaleByDpi(18, dpi));
        return std::max(0.0f, metrics.height - viewportHeight);
    }

    void OcrTextPanel::EnsureCaretVisible(const RECT& box, int dpi,
                                          IDWriteFactory* dwrite, IDWriteTextFormat* format)
    {
        auto layout = TextLayout(box, dpi, dwrite, format);
        if (!layout)
        {
            return;
        }
        FLOAT x = 0.0f;
        FLOAT y = 0.0f;
        DWRITE_HIT_TEST_METRICS caret{};
        if (FAILED(layout->HitTestTextPosition(static_cast<UINT32>(m_caret), FALSE,
                                                &x, &y, &caret)))
        {
            return;
        }

        const float viewportHeight = std::max(1.0f,
            static_cast<float>(box.bottom - box.top) - ScaleByDpi(18, dpi));
        if (y < m_scrollY)
        {
            m_scrollY = y;
        }
        else if (y + caret.height > m_scrollY + viewportHeight)
        {
            m_scrollY = y + caret.height - viewportHeight;
        }
        m_scrollY = std::clamp(m_scrollY, 0.0f, ScrollMaximum(box, dpi, dwrite, format));
    }

    bool OcrTextPanel::MoveCaret(const RECT& box, WPARAM key, int dpi,
                                 IDWriteFactory* dwrite, IDWriteTextFormat* format,
                                 KeyModifiers mods)
    {
        const std::size_t length = m_text.size();
        const bool extend = mods.shift;
        std::size_t next = m_caret;
        if (key == VK_LEFT || key == VK_RIGHT)
        {
            if (mods.ctrl)
            {
                next = (key == VK_LEFT)
                    ? PrevWordOffset(m_text, next)
                    : NextWordOffset(m_text, next);
            }
            else if (!extend && m_anchor != m_caret)
            {
                next = (key == VK_LEFT)
                    ? std::min(m_anchor, m_caret)
                    : std::max(m_anchor, m_caret);
            }
            else
            {
                next = (key == VK_LEFT)
                    ? PrevCodeUnitOffset(m_text, next)
                    : NextCodeUnitOffset(m_text, next);
            }
        }
        else if (key == VK_HOME || key == VK_END)
        {
            if (mods.ctrl)
            {
                next = (key == VK_HOME) ? 0 : length;
            }
            else
            {
                const auto [lineStart, lineEnd] = LineRangeAt(m_text, next);
                next = (key == VK_HOME) ? lineStart : lineEnd;
            }
        }
        else if (key == VK_UP || key == VK_DOWN)
        {
            if (auto layout = TextLayout(box, dpi, dwrite, format))
            {
                FLOAT x = 0.0f;
                FLOAT y = 0.0f;
                DWRITE_HIT_TEST_METRICS caret{};
                if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(m_caret), FALSE,
                                                           &x, &y, &caret)))
                {
                    const float targetY = y + (key == VK_UP ? -caret.height : caret.height);
                    if (targetY >= 0.0f)
                    {
                        BOOL trailing = FALSE;
                        BOOL inside = FALSE;
                        DWRITE_HIT_TEST_METRICS target{};
                        if (SUCCEEDED(layout->HitTestPoint(x, targetY, &trailing, &inside, &target)))
                        {
                            next = std::min<std::size_t>(
                                target.textPosition + (trailing ? target.length : 0), length);
                        }
                    }
                }
            }
        }

        m_caret = next;
        if (!extend)
        {
            m_anchor = next;
        }
        EnsureCaretVisible(box, dpi, dwrite, format);
        return true;
    }

    bool OcrTextPanel::UpdateSelection(const RECT& box, POINT point, int dpi,
                                       IDWriteFactory* dwrite, IDWriteTextFormat* format)
    {
        m_selectionPoint = point;
        const float oldScrollY = m_scrollY;
        const std::size_t oldCaret = m_caret;
        const std::size_t oldAnchor = m_anchor;
        const LONG inset = static_cast<LONG>(ScaleByDpi(9, dpi));
        const LONG top = box.top + inset;
        const LONG bottom = box.bottom - inset;
        const LONG left = box.left + inset;
        const LONG right = box.right - inset - static_cast<LONG>(ScaleByDpi(kOcrScrollGutter, dpi));
        const LONG clampedY = std::clamp(point.y, top, std::max(top, bottom - 1));
        const LONG clampedX = std::clamp(point.x, left, std::max(left, right));

        if (point.y < top || point.y >= bottom)
        {
            const float distance = static_cast<float>(point.y < top ? top - point.y : point.y - bottom + 1);
            const float step = std::max(ScaleByDpi(1, dpi), distance * 0.22f);
            m_scrollY = std::clamp(
                m_scrollY + (point.y < top ? -step : step),
                0.0f, ScrollMaximum(box, dpi, dwrite, format));
        }
        const std::size_t pos = PositionFromPoint(box, POINT{ clampedX, clampedY }, dpi, dwrite, format);
        m_granularSelect.ExtendTo(m_text, pos, m_anchor, m_caret);
        return m_caret != oldCaret || m_anchor != oldAnchor || m_scrollY != oldScrollY;
    }

    bool OcrTextPanel::OnLeftButtonDown(
        HWND window, const RECT& box, POINT point, int dpi,
        IDWriteFactory* dwrite, IDWriteTextFormat* format, bool& needsRender,
        KeyModifiers mods)
    {
        if (window != nullptr)
        {
            SetFocus(window);
        }
        const float scrollMaximum = ScrollMaximum(box, dpi, dwrite, format);
        const RECT scrollTrack = ScrollTrack(box, dpi);
        if (scrollMaximum > 0.0f && PtInRect(&scrollTrack, point))
        {
            const RECT scrollThumb = ScrollThumb(box, dpi, dwrite, format);
            if (PtInRect(&scrollThumb, point))
            {
                m_scrollDragging = true;
                m_scrollDragOrigin = point;
                m_scrollDragStart = m_scrollY;
            }
            else
            {
                const float page = std::max(1.0f,
                    static_cast<float>(box.bottom - box.top) - ScaleByDpi(27, dpi));
                m_scrollY = std::clamp(
                    m_scrollY + (point.y < scrollThumb.top ? -page : page),
                    0.0f, scrollMaximum);
                needsRender = true;
            }
            return true;
        }

        const int clicks = m_clickTracker.RegisterClick(point);
        const std::size_t pos = PositionFromPoint(box, point, dpi, dwrite, format);
        m_granularSelect.BeginClick(m_text, pos, clicks, mods.shift, m_anchor, m_caret);
        EnsureCaretVisible(box, dpi, dwrite, format);
        m_selecting = true;
        m_selectionPoint = point;
        if (window != nullptr)
        {
            SetTimer(window, kOcrSelectionTimer, kOcrSelectionTimerMs, nullptr);
        }
        needsRender = true;
        return true;
    }

    bool OcrTextPanel::OnMouseMove(const RECT& box, POINT point, int dpi,
                                   IDWriteFactory* dwrite, IDWriteTextFormat* format)
    {
        if (m_scrollDragging)
        {
            const RECT track = ScrollTrack(box, dpi);
            const RECT thumb = ScrollThumb(box, dpi, dwrite, format);
            const float travel = static_cast<float>(track.bottom - track.top)
                - static_cast<float>(thumb.bottom - thumb.top);
            if (travel > 0.0f)
            {
                const float maximum = ScrollMaximum(box, dpi, dwrite, format);
                const float pointerDelta = static_cast<float>(point.y - m_scrollDragOrigin.y);
                m_scrollY = std::clamp(
                    m_scrollDragStart + pointerDelta * maximum / travel, 0.0f, maximum);
            }
            return true;
        }
        if (m_selecting)
        {
            UpdateSelection(box, point, dpi, dwrite, format);
            return true;
        }
        return false;
    }

    bool OcrTextPanel::OnLeftButtonUp(HWND window, const RECT& box, POINT point, int dpi,
                                      IDWriteFactory* dwrite, IDWriteTextFormat* format)
    {
        if (m_scrollDragging)
        {
            m_scrollDragging = false;
            return true;
        }
        if (m_selecting)
        {
            UpdateSelection(box, point, dpi, dwrite, format);
            StopSelection(window);
            return true;
        }
        return false;
    }

    bool OcrTextPanel::OnSelectionTimer(const RECT& box, int dpi,
                                        IDWriteFactory* dwrite, IDWriteTextFormat* format)
    {
        if (!m_selecting)
        {
            return false;
        }
        return UpdateSelection(box, m_selectionPoint, dpi, dwrite, format);
    }

    void OcrTextPanel::OnWheel(const RECT& box, int delta, int dpi, int textSizeDip,
                               IDWriteFactory* dwrite, IDWriteTextFormat* format)
    {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const float page = std::max(1.0f, static_cast<float>(box.bottom - box.top)
            - ScaleByDpi(18, dpi));
        const float lineHeight = ScaleByDpi(static_cast<float>(textSizeDip + 3), dpi);
        const float amount = lines == WHEEL_PAGESCROLL
            ? page * static_cast<float>(delta) / WHEEL_DELTA
            : lineHeight * static_cast<float>(lines) * static_cast<float>(delta) / WHEEL_DELTA;
        m_scrollY = std::clamp(m_scrollY - amount, 0.0f, ScrollMaximum(box, dpi, dwrite, format));
    }

    void OcrTextPanel::ShowContextMenu(HWND window, const RECT& box, POINT point, int dpi,
                                       IDWriteFactory* dwrite, IDWriteTextFormat* format)
    {
        const TextContextCommand cmd = ShowTextContextMenu(
            window, point, /*editable=*/false, m_anchor != m_caret, !m_text.empty());
        if (cmd == TextContextCommand::Copy)
        {
            CopyText(window);
        }
        else if (cmd == TextContextCommand::SelectAll)
        {
            m_anchor = 0;
            m_caret = m_text.size();
            EnsureCaretVisible(box, dpi, dwrite, format);
        }
    }

    bool OcrTextPanel::OnKeyDown(HWND window, const RECT& box, WPARAM key, int dpi,
                                 IDWriteFactory* dwrite, IDWriteTextFormat* format,
                                 bool& shouldClose, bool& needsRender,
                                 KeyModifiers mods)
    {
        shouldClose = false;
        needsRender = false;
        if (key == 'C' && mods.ctrl)
        {
            // Only close once the text actually lands on the clipboard (or if
            // there is no text to copy), so a locked clipboard never discards
            // the recognized result.
            shouldClose = m_text.empty() ? true : CopyText(window);
            return true;
        }
        if (key == 'A' && mods.ctrl)
        {
            m_anchor = 0;
            m_caret = m_text.size();
            EnsureCaretVisible(box, dpi, dwrite, format);
            needsRender = true;
            return true;
        }
        if (key == VK_LEFT || key == VK_UP
            || key == VK_RIGHT || key == VK_DOWN
            || key == VK_HOME || key == VK_END)
        {
            MoveCaret(box, key, dpi, dwrite, format, mods);
            needsRender = true;
            return true;
        }
        if (key == VK_PRIOR || key == VK_NEXT)
        {
            const float page = std::max(1.0f,
                static_cast<float>(box.bottom - box.top) - ScaleByDpi(27, dpi));
            m_scrollY = std::clamp(
                m_scrollY + (key == VK_NEXT ? page : -page),
                0.0f, ScrollMaximum(box, dpi, dwrite, format));
            needsRender = true;
            return true;
        }
        if (key == VK_RETURN)
        {
            return true;
        }
        return false;
    }

    bool OcrTextPanel::CopyText(HWND window) const
    {
        const std::size_t first = std::min(m_anchor, m_caret);
        const std::size_t last = std::max(m_anchor, m_caret);
        const std::wstring_view slice = (first == last)
            ? std::wstring_view(m_text)
            : std::wstring_view(m_text).substr(first, last - first);
        return CopyTextToClipboard(window, slice);
    }

    bool OcrTextPanel::EnsureSpinnerBitmap(ID2D1DeviceContext* context, int dpi)
    {
        if (m_spinnerBitmap && m_spinnerDpi == dpi)
        {
            return true;
        }
        if (context == nullptr)
        {
            return false;
        }

        SpinnerParameters parameters{};
        parameters.radius = ScaleByDpi(kOcrSpinnerRadius, dpi);
        parameters.half_stroke = ScaleByDpi(kOcrSpinnerStroke, dpi) * 0.5f;
        parameters.sweep = kOcrSpinnerSweepDeg * kDegreesToRadians;
        parameters.tail_alpha = 0.15f;
        parameters.feather = 1.0f;

        if (FAILED(CreateSpinnerBitmap(
                context, parameters, D2D1::ColorF(0.18f, 0.49f, 0.94f, 1.0f),
                m_spinnerBitmap)))
        {
            m_spinnerBitmap = nullptr;
            m_spinnerDpi = 0;
            return false;
        }
        m_spinnerDpi = dpi;
        return true;
    }

    void OcrTextPanel::DrawSpinner(ID2D1DeviceContext* context, const RECT& box, int dpi)
    {
        if (!EnsureSpinnerBitmap(context, dpi))
        {
            return;
        }

        const float centerX = static_cast<float>(box.left + box.right) * 0.5f;
        const float centerY = static_cast<float>(box.top + box.bottom) * 0.5f;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsedMs = static_cast<double>(now.QuadPart - m_pendingStart)
            * 1000.0 / static_cast<double>(PerformanceCounterFrequency());
        const float turn = static_cast<float>(
            std::fmod(elapsedMs / static_cast<double>(kOcrSpinnerPeriodMs), 1.0)) * 360.0f;

        const D2D1_SIZE_F size = m_spinnerBitmap->GetSize();
        const float halfWidth = size.width * 0.5f;
        const float halfHeight = size.height * 0.5f;
        const D2D1_RECT_F destination{ -halfWidth, -halfHeight, halfWidth, halfHeight };

        D2D1_MATRIX_3X2_F saved{};
        context->GetTransform(&saved);
        context->SetTransform(
            D2D1::Matrix3x2F::Rotation(turn)
            * D2D1::Matrix3x2F::Translation(centerX, centerY)
            * saved);

        context->DrawBitmap(m_spinnerBitmap.get(), &destination, 1.0f,
                            D2D1_INTERPOLATION_MODE_LINEAR);

        context->SetTransform(saved);
    }

    void OcrTextPanel::Draw(ID2D1DeviceContext* context, IDWriteFactory* dwrite,
                            IDWriteTextFormat* format, const RECT& box, int dpi,
                            const HudBrushes& brushes)
    {
        context->FillRectangle(ToRectF(box), brushes.handleFill.get());
        context->DrawRectangle(ToRectF(box), brushes.handleEdge.get(), ScaleByDpi(1, dpi));

        if (m_pending)
        {
            DrawSpinner(context, box, dpi);
            return;
        }

        auto layout = TextLayout(box, dpi, dwrite, format);
        if (!layout)
        {
            return;
        }

        const float padding = ScaleByDpi(9, dpi);
        const D2D1_POINT_2F origin{
            std::round(box.left + padding),
            std::round(box.top + padding - m_scrollY) };
        const D2D1_RECT_F clip = D2D1::RectF(
            static_cast<float>(box.left), static_cast<float>(box.top),
            static_cast<float>(box.right), static_cast<float>(box.bottom));
        context->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        auto popClip = wil::scope_exit([&] { context->PopAxisAlignedClip(); });

        const std::size_t first = std::min(m_anchor, m_caret);
        const std::size_t last = std::max(m_anchor, m_caret);
        if (last > first)
        {
            UINT32 count = 0;
            layout->HitTestTextRange(
                static_cast<UINT32>(first), static_cast<UINT32>(last - first),
                origin.x, origin.y, nullptr, 0, &count);
            if (count > 0)
            {
                m_hitMetrics.resize(count);
                if (SUCCEEDED(layout->HitTestTextRange(
                        static_cast<UINT32>(first), static_cast<UINT32>(last - first),
                        origin.x, origin.y, m_hitMetrics.data(), count, &count)))
                {
                    for (UINT32 i = 0; i < count; ++i)
                    {
                        const auto& item = m_hitMetrics[i];
                        context->FillRectangle(
                            D2D1::RectF(item.left, item.top, item.left + item.width, item.top + item.height),
                            brushes.accent.get());
                    }
                }
            }
        }

        if (m_layoutHighlightFirst != first || m_layoutHighlightLast != last)
        {
            if (m_layoutHighlightLast > m_layoutHighlightFirst)
            {
                AGSHOT_CHECK_HR(layout->SetDrawingEffect(nullptr, DWRITE_TEXT_RANGE{
                    static_cast<UINT32>(m_layoutHighlightFirst),
                    static_cast<UINT32>(m_layoutHighlightLast - m_layoutHighlightFirst) }));
            }
            if (last > first)
            {
                AGSHOT_CHECK_HR(layout->SetDrawingEffect(brushes.handleFill.get(), DWRITE_TEXT_RANGE{
                    static_cast<UINT32>(first), static_cast<UINT32>(last - first) }));
            }
            m_layoutHighlightFirst = first;
            m_layoutHighlightLast = last;
        }

        context->DrawTextLayout(origin, layout.get(), brushes.handleEdge.get());

        FLOAT caretX = 0.0f;
        FLOAT caretY = 0.0f;
        DWRITE_HIT_TEST_METRICS caret{};
        if (SUCCEEDED(layout->HitTestTextPosition(
                static_cast<UINT32>(m_caret), FALSE, &caretX, &caretY, &caret)))
        {
            const float caretLeft = std::round(origin.x + caretX);
            const D2D1_POINT_2F top{ caretLeft, std::round(origin.y + caretY) };
            const D2D1_POINT_2F bottom{ caretLeft, top.y + std::max(1.0f, std::round(caret.height)) };
            context->DrawLine(top, bottom, brushes.accent.get(), ScaleByDpi(1, dpi));
        }
        popClip.reset();

        if (ScrollMaximum(box, dpi, dwrite, format) > 0.0f)
        {
            const RECT track = ScrollTrack(box, dpi);
            const RECT thumb = ScrollThumb(box, dpi, dwrite, format);
            const float radius = ScaleByDpi(kOcrScrollWidth, dpi) * 0.5f;
            context->FillRoundedRectangle(
                ToRoundedRect(track, radius), brushes.textScrollTrack.get());
            context->FillRoundedRectangle(
                ToRoundedRect(thumb, radius), brushes.textScrollThumb.get());
        }
    }

#ifdef AGSHOT_TOOLBAR_PROOF
    void OcrTextPanel::ConfigureProofState(const RECT& selection, float scrollProbe,
                                           bool spinnerProbe)
    {
        m_text = L"AGShot text recognition preview\r\n\r\nThis is mock OCR content for the selected area. Select part of this text with the mouse or keyboard, then copy it.\r\n\r\nThe recognized text stays cached while the selection remains unchanged.";
        m_cacheValid = true;
        m_cacheSelection = selection;
        m_scrollY = scrollProbe;
        m_pending = spinnerProbe;
        if (m_pending)
        {
            m_text.clear();
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            m_pendingStart = now.QuadPart
                - PerformanceCounterFrequency() * kOcrSpinnerPeriodMs / 4000;
        }
    }
#endif
}
