#include "pch.h"

#include "hud/hud_renderer.h"

#include "core/error.h"
#include "hud/ocr_text_panel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string_view>

namespace agshot
{
    namespace
    {
        constexpr int kBorderWidth = 2;
        constexpr int kCornerRadius = 6;
        constexpr int kLabelPadding = 7;
        constexpr int kTooltipPadding = 8;
        constexpr int kTooltipGap = 6;
        constexpr int kIconInset = 7;
        constexpr int kButtonRadius = 4;
        constexpr float kIconInk = 16.0f;

        constexpr std::array<std::string_view, 4> kToolbarIconSvgs{
            // EditSelection
            R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" fill="none" stroke="#FFFFFF" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M0.9 5.9V1.57A0.67 0.67 0 0 1 1.57 0.9H5.9M10.1 0.9H14.43A0.67 0.67 0 0 1 15.1 1.57V5.9M0.9 10.1V14.43A0.67 0.67 0 0 0 1.57 15.1H5.9M15.1 10.1V14.43A0.67 0.67 0 0 1 14.43 15.1H10.1"/></svg>)",
            // TextRecognition
            R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" fill="none" stroke="#FFFFFF" stroke-width="1.8" stroke-linecap="butt" stroke-linejoin="miter"><path d="M4.89 0.9H1.57A0.67 0.67 0 0 0 0.9 1.57V4.89M11.11 0.9H14.43A0.67 0.67 0 0 1 15.1 1.57V4.89M0.9 11.11V14.43A0.67 0.67 0 0 0 1.57 15.1H4.89M15.1 11.11V14.43A0.67 0.67 0 0 1 14.43 15.1H11.11M3.14 5.21H12.88M3.14 8H10.96M3.14 10.79H11.87"/></svg>)",
            // Copy
            R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" fill="none" stroke="#FFFFFF" stroke-width="1.8" stroke-linecap="butt" stroke-linejoin="round"><path d="M5.16 11.12H2.48A1.58 1.58 0 0 1 0.9 9.54V2.48A1.58 1.58 0 0 1 2.48 0.9H8.12A1.58 1.58 0 0 1 9.70 2.48V4.88M6.74 4.88H13.52A1.58 1.58 0 0 1 15.1 6.46V13.52A1.58 1.58 0 0 1 13.52 15.1H6.74A1.58 1.58 0 0 1 5.16 13.52V6.46A1.58 1.58 0 0 1 6.74 4.88Z"/></svg>)",
            // Cancel
            R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" fill="none" stroke="#FFFFFF" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M0.9 0.9L15.1 15.1M15.1 0.9L0.9 15.1"/></svg>)",
        };

        winrt::com_ptr<IStream> CreateUtf8MemoryStream(std::string_view text)
        {
            const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size());
            AGSHOT_CHECK_WIN32(memory != nullptr);
            void* locked = GlobalLock(memory);
            if (locked == nullptr)
            {
                GlobalFree(memory);
                AGSHOT_CHECK_WIN32(false);
            }
            std::memcpy(locked, text.data(), text.size());
            GlobalUnlock(memory);

            winrt::com_ptr<IStream> stream;
            const HRESULT hr = CreateStreamOnHGlobal(memory, TRUE, stream.put());
            if (FAILED(hr))
            {
                GlobalFree(memory);
                AGSHOT_CHECK_HR(hr);
            }
            return stream;
        }

        RECT InsetRectBy(const RECT& rect, int by) noexcept
        {
            return RECT{ rect.left + by, rect.top + by, rect.right - by, rect.bottom - by };
        }
    }

    void DebugStatsTracker::SetEnabled(bool enabled) noexcept
    {
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        m_sampleCount = 0;
        m_sampleHead = 0;
        m_frameMs = 0.0f;
        m_renderMs = 0.0f;
        m_lastFrame = 0;
        m_lastSample = 0;
    }

    void DebugStatsTracker::RecordFrameDuration(LONGLONG renderStartQpc) noexcept
    {
        if (!m_enabled)
        {
            return;
        }
        LARGE_INTEGER renderEnd{};
        QueryPerformanceCounter(&renderEnd);
        const float renderMs = static_cast<float>(
            static_cast<double>(renderEnd.QuadPart - renderStartQpc) * 1000.0
            / static_cast<double>(PerformanceCounterFrequency()));
        m_renderMs += (renderMs - m_renderMs) * 0.1f;
    }

    void DebugStatsTracker::RecordPresent() noexcept
    {
        if (!m_enabled)
        {
            return;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const LONGLONG frequency = PerformanceCounterFrequency();
        if (m_lastFrame != 0)
        {
            const float deltaMs = static_cast<float>(
                static_cast<double>(now.QuadPart - m_lastFrame) * 1000.0
                / static_cast<double>(frequency));
            if (deltaMs > 0.0f && deltaMs < 1000.0f)
            {
                m_frameMs = m_frameMs > 0.0f
                    ? m_frameMs + (deltaMs - m_frameMs) * 0.1f
                    : deltaMs;
            }
        }
        m_lastFrame = now.QuadPart;

        const double sinceSampleMs = m_lastSample == 0
            ? static_cast<double>(kWindowMs)
            : static_cast<double>(now.QuadPart - m_lastSample) * 1000.0
                / static_cast<double>(frequency);
        if (sinceSampleMs >= kWindowMs && m_frameMs > 0.0f)
        {
            m_samples[m_sampleHead] = 1000.0f / m_frameMs;
            m_sampleTime[m_sampleHead] = now.QuadPart;
            m_sampleHead = (m_sampleHead + 1) % kSamples;
            m_sampleCount = std::min(m_sampleCount + 1, kSamples);
            m_lastSample = now.QuadPart;
        }
    }

    void DebugStatsTracker::Draw(const RenderDevice& device, UINT surfaceWidth)
    {
        if (!m_enabled)
        {
            return;
        }

        const int dpi = device.dpi();
        ID2D1DeviceContext* context = device.d2dContext();
        const HudBrushes& brushes = device.brushes();

        const float margin = ScaleByDpi(10, dpi);
        const float width = ScaleByDpi(190, dpi);
        const float height = ScaleByDpi(78, dpi);
        const float left = static_cast<float>(surfaceWidth) - margin - width;
        const D2D1_RECT_F panel = D2D1::RectF(left, margin, left + width, margin + height);

        context->FillRoundedRectangle(
            D2D1::RoundedRect(panel, ScaleByDpi(4, dpi), ScaleByDpi(4, dpi)), brushes.panel.get());
        context->DrawRoundedRectangle(
            D2D1::RoundedRect(panel, ScaleByDpi(4, dpi), ScaleByDpi(4, dpi)),
            brushes.panelEdge.get(), ScaleByDpi(1, dpi));

        const float fps = m_frameMs > 0.0f ? 1000.0f / m_frameMs : 0.0f;
        wchar_t caption[96]{};
        swprintf_s(caption, L"%5.1f fps   %6.2f ms", fps, m_renderMs);
        context->DrawTextW(
            caption,
            static_cast<UINT32>(wcslen(caption)),
            device.statsTextFormat(),
            D2D1::RectF(panel.left + ScaleByDpi(8, dpi), panel.top + ScaleByDpi(6, dpi),
                        panel.right - ScaleByDpi(8, dpi), panel.top + ScaleByDpi(26, dpi)),
            brushes.icon.get());

        const D2D1_RECT_F plot = D2D1::RectF(
            panel.left + ScaleByDpi(8, dpi), panel.top + ScaleByDpi(30, dpi),
            panel.right - ScaleByDpi(8, dpi), panel.bottom - ScaleByDpi(8, dpi));
        const float plotWidth = plot.right - plot.left;
        const float plotHeight = plot.bottom - plot.top;
        if (m_sampleCount < 2 || plotWidth <= 0.0f || plotHeight <= 0.0f)
        {
            return;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double span = kGraphSeconds * static_cast<double>(PerformanceCounterFrequency());

        int slots[kSamples]{};
        float peak = 0.0f;
        int visible = 0;
        for (int i = 0; i < m_sampleCount; ++i)
        {
            const int slot = (m_sampleHead - m_sampleCount + i + kSamples * 2) % kSamples;
            const double age = static_cast<double>(now.QuadPart - m_sampleTime[slot]);
            if (age < 0.0 || age > span)
            {
                continue;
            }
            slots[visible++] = slot;
            peak = std::max(peak, m_samples[slot]);
        }

        if (visible < 2)
        {
            return;
        }

        const float scale = std::max(60.0f, peak * 1.1f);
        D2D1_POINT_2F previous{};
        for (int i = 0; i < visible; ++i)
        {
            const int slot = slots[i];
            const float age = static_cast<float>(
                static_cast<double>(now.QuadPart - m_sampleTime[slot]) / span);
            const D2D1_POINT_2F current{
                plot.right - plotWidth * age,
                plot.bottom - plotHeight * std::min(1.0f, m_samples[slot] / scale) };
            if (i > 0)
            {
                context->DrawLine(previous, current, brushes.accent.get(), ScaleByDpi(1, dpi));
            }
            previous = current;
        }
    }

    void HudRenderer::InvalidateLayouts() noexcept
    {
        m_sizeLabelLayout = nullptr;
        m_sizeLabelSelection = RECT{};
        m_sizeLabelValid = false;

        m_tooltipLayout = nullptr;
        m_tooltipLayoutButton = ToolbarButton::None;
    }

    void HudRenderer::DiscardIndependentResources() noexcept
    {
        InvalidateLayouts();
        m_svgContext = nullptr;
        for (auto& svg : m_iconSvgs)
        {
            svg = nullptr;
        }
    }

    void HudRenderer::EnsureToolbarSvgs(ID2D1DeviceContext* context)
    {
        if (m_svgContext == context && m_iconSvgs[0])
        {
            return;
        }

        m_svgContext = nullptr;
        for (auto& svg : m_iconSvgs)
        {
            svg = nullptr;
        }

        winrt::com_ptr<ID2D1DeviceContext5> context5;
        AGSHOT_CHECK_HR(context->QueryInterface(__uuidof(ID2D1DeviceContext5), context5.put_void()));

        for (std::size_t i = 0; i < m_iconSvgs.size(); ++i)
        {
            const winrt::com_ptr<IStream> stream = CreateUtf8MemoryStream(kToolbarIconSvgs[i]);
            AGSHOT_CHECK_HR(context5->CreateSvgDocument(
                stream.get(), D2D1::SizeF(kIconInk, kIconInk), m_iconSvgs[i].put()));
        }
        m_svgContext = context;
    }

    void HudRenderer::DrawDim(ID2D1DeviceContext* context, UINT width, UINT height,
                              bool hasSelection, const RECT& selection,
                              const HudBrushes& brushes)
    {
        const D2D1_RECT_F full = D2D1::RectF(0.0f, 0.0f,
                                             static_cast<float>(width),
                                             static_cast<float>(height));
        if (!hasSelection)
        {
            context->FillRectangle(full, brushes.dim.get());
            return;
        }

        const float left = static_cast<float>(selection.left);
        const float top = static_cast<float>(selection.top);
        const float right = static_cast<float>(selection.right);
        const float bottom = static_cast<float>(selection.bottom);

        context->FillRectangle(D2D1::RectF(full.left, full.top, full.right, top), brushes.dim.get());
        context->FillRectangle(D2D1::RectF(full.left, bottom, full.right, full.bottom), brushes.dim.get());
        context->FillRectangle(D2D1::RectF(full.left, top, left, bottom), brushes.dim.get());
        context->FillRectangle(D2D1::RectF(right, top, full.right, bottom), brushes.dim.get());
    }

    void HudRenderer::DrawSelection(const RenderDevice& device, UINT width, UINT height,
                                    const RECT& selection, bool smart, int handleSize)
    {
        const int dpi = device.dpi();
        ID2D1DeviceContext* context = device.d2dContext();
        const HudBrushes& brushes = device.brushes();
        const float stroke = ScaleByDpi(kBorderWidth, dpi);

        RECT border = selection;
        const int half = static_cast<int>(stroke / 2.0f);
        InflateRect(&border, -half, -half);

        context->DrawRectangle(ToRectF(border), brushes.accent.get(), stroke);

        if (!smart)
        {
            for (const RECT& handle : HandleRects(selection, handleSize))
            {
                const D2D1_RECT_F box = ToRectF(handle);
                context->FillRectangle(box, brushes.handleFill.get());
                context->DrawRectangle(box, brushes.handleEdge.get(), 1.0f);
            }
        }

        DrawSizeLabel(device, width, height, selection);
    }

    void HudRenderer::DrawSizeLabel(const RenderDevice& device, UINT surfaceWidth, UINT surfaceHeight,
                                    const RECT& selection)
    {
        const int dpi = device.dpi();
        wchar_t text[64]{};
        const LONG selectionWidth = selection.right - selection.left;
        const LONG selectionHeight = selection.bottom - selection.top;
        swprintf_s(text, L"%d x %d", selectionWidth, selectionHeight);

        if (!m_sizeLabelValid || !EqualRect(&m_sizeLabelSelection, &selection))
        {
            m_sizeLabelLayout = nullptr;
            AGSHOT_CHECK_HR(device.dwriteFactory()->CreateTextLayout(
                text, static_cast<UINT32>(wcslen(text)), device.textFormat(),
                static_cast<float>(surfaceWidth), static_cast<float>(surfaceHeight),
                m_sizeLabelLayout.put()));
            m_sizeLabelSelection = selection;
            m_sizeLabelValid = true;
        }
        const auto& layout = m_sizeLabelLayout;

        DWRITE_TEXT_METRICS metrics{};
        AGSHOT_CHECK_HR(layout->GetMetrics(&metrics));

        const float padding = ScaleByDpi(kLabelPadding, dpi);
        const float width = metrics.width + padding * 2.0f;
        const float height = metrics.height + padding;

        const float gap = ScaleByDpi(3, dpi);
        const float centeredX = std::clamp(
            static_cast<float>(selection.left), 0.0f,
            std::max(0.0f, static_cast<float>(surfaceWidth) - width));
        const float centeredY = std::clamp(
            (static_cast<float>(selection.top + selection.bottom) - height) * 0.5f, 0.0f,
            std::max(0.0f, static_cast<float>(surfaceHeight) - height));
        const D2D1_RECT_F candidates[] =
        {
            D2D1::RectF(centeredX, static_cast<float>(selection.top) - gap - height,
                        centeredX + width, static_cast<float>(selection.top) - gap),
            D2D1::RectF(centeredX, static_cast<float>(selection.bottom) + gap,
                        centeredX + width, static_cast<float>(selection.bottom) + gap + height),
            D2D1::RectF(static_cast<float>(selection.right) + gap, centeredY,
                        static_cast<float>(selection.right) + gap + width, centeredY + height),
            D2D1::RectF(static_cast<float>(selection.left) - gap - width, centeredY,
                        static_cast<float>(selection.left) - gap, centeredY + height),
        };

        D2D1_RECT_F box{};
        bool found = false;
        for (const D2D1_RECT_F& candidate : candidates)
        {
            if (candidate.left < 0.0f || candidate.top < 0.0f
                || candidate.right > static_cast<float>(surfaceWidth)
                || candidate.bottom > static_cast<float>(surfaceHeight))
            {
                continue;
            }
            if (candidate.right <= selection.left || candidate.left >= selection.right
                || candidate.bottom <= selection.top || candidate.top >= selection.bottom)
            {
                box = candidate;
                found = true;
                break;
            }
        }
        if (!found)
        {
            const float maxX = std::max(0.0f, static_cast<float>(surfaceWidth) - width);
            const float maxY = std::max(0.0f, static_cast<float>(surfaceHeight) - height);
            box = D2D1::RectF(
                std::clamp(static_cast<float>(selection.left), 0.0f, maxX),
                std::clamp(static_cast<float>(selection.top) - gap - height, 0.0f, maxY),
                0.0f, 0.0f);
            box.right = box.left + width;
            box.bottom = box.top + height;
        }

        const HudBrushes& brushes = device.brushes();
        ID2D1DeviceContext* context = device.d2dContext();
        context->FillRoundedRectangle(
            D2D1::RoundedRect(box, ScaleByDpi(4, dpi), ScaleByDpi(4, dpi)), brushes.panel.get());
        context->DrawTextLayout(
            D2D1::Point2F(box.left + padding, box.top + padding * 0.5f),
            layout.get(), brushes.icon.get());
    }

    void HudRenderer::DrawPixelMagnifier(
        const RenderDevice& device, UINT width, UINT height,
        const ScreenImage& image, const RECT& bounds,
        const RECT& selection, POINT pointer,
        bool draggingSelection, SelectionPart dragPart,
        bool translucent)
    {
        if (!device.hasBackground() || image.empty())
        {
            return;
        }

        const int dpi = device.dpi();
        const auto measured = [dpi](float px) { return MeasuredPixelsForDpi(px, dpi); };

        constexpr int sampleCells = 11;
        const float cell = measured(18.0f);
        const float previewSize = sampleCells * cell;
        const float cardWidth = measured(210.0f);
        const float cardHeight = measured(291.0f);
        const float gap = measured(16.0f);
        const float margin = measured(6.0f);

        POINT samplePoint = pointer;
        if (draggingSelection)
        {
            switch (dragPart)
            {
            case SelectionPart::Left:
            case SelectionPart::TopLeft:
            case SelectionPart::BottomLeft:
                samplePoint.x = selection.left;
                break;
            case SelectionPart::Right:
            case SelectionPart::TopRight:
            case SelectionPart::BottomRight:
                samplePoint.x = selection.right - 1;
                break;
            default:
                break;
            }
            switch (dragPart)
            {
            case SelectionPart::Top:
            case SelectionPart::TopLeft:
            case SelectionPart::TopRight:
                samplePoint.y = selection.top;
                break;
            case SelectionPart::Bottom:
            case SelectionPart::BottomLeft:
            case SelectionPart::BottomRight:
                samplePoint.y = selection.bottom - 1;
                break;
            default:
                break;
            }
        }
        const int sampleX = std::clamp(static_cast<int>(samplePoint.x), 0, image.width() - 1);
        const int sampleY = std::clamp(static_cast<int>(samplePoint.y), 0, image.height() - 1);
        const int sourceLeft = std::clamp(sampleX - sampleCells / 2, 0,
            std::max(0, image.width() - sampleCells));
        const int sourceTop = std::clamp(sampleY - sampleCells / 2, 0,
            std::max(0, image.height() - sampleCells));
        const int selectedX = std::clamp(sampleX - sourceLeft, 0, sampleCells - 1);
        const int selectedY = std::clamp(sampleY - sourceTop, 0, sampleCells - 1);

        const auto* pixels = static_cast<const BYTE*>(image.pixels());
        const BYTE* color = pixels + sampleY * image.stride() + sampleX * 4;
        const BYTE blue = color[0];
        const BYTE green = color[1];
        const BYTE red = color[2];
        const float luminance = (red * 0.299f + green * 0.587f + blue * 0.114f) / 255.0f;

        float x = static_cast<float>(pointer.x) + gap;
        float y = static_cast<float>(pointer.y) + gap;
        if (x + cardWidth > static_cast<float>(width))
        {
            x = static_cast<float>(pointer.x) - gap - cardWidth;
        }
        if (y + cardHeight > static_cast<float>(height))
        {
            y = static_cast<float>(pointer.y) - gap - cardHeight;
        }
        x = std::clamp(x, 0.0f, std::max(0.0f, static_cast<float>(width) - cardWidth));
        y = std::clamp(y, 0.0f, std::max(0.0f, static_cast<float>(height) - cardHeight));

        const RECT card{ static_cast<LONG>(x), static_cast<LONG>(y),
                         static_cast<LONG>(x + cardWidth), static_cast<LONG>(y + cardHeight) };

        ID2D1DeviceContext* context = device.d2dContext();
        const HudBrushes& brushes = device.brushes();

        struct LayerPop
        {
            ID2D1DeviceContext* ctx{};
            ~LayerPop()
            {
                if (ctx != nullptr)
                {
                    ctx->PopLayer();
                }
            }
        } layerPop;
        if (translucent)
        {
            const float layerExtent = measured(8.0f);
            const D2D1_RECT_F layerBounds = D2D1::RectF(
                x - layerExtent, y - layerExtent,
                x + cardWidth + layerExtent, y + cardHeight + layerExtent);
            const auto layer = D2D1::LayerParameters(
                layerBounds, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(), 0.55f);
            context->PushLayer(layer, device.magnifierLayer());
            layerPop.ctx = context;
        }

        for (int spread = 6; spread >= 1; --spread)
        {
            const float extent = measured(static_cast<float>(spread));
            const D2D1_RECT_F shadowBounds = D2D1::RectF(
                x - extent, y - extent + measured(2.0f),
                x + cardWidth + extent, y + cardHeight + extent + measured(2.0f));
            const float opacity = 0.015f * static_cast<float>(7 - spread);
            brushes.shadow->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, opacity));
            context->FillRoundedRectangle(
                D2D1::RoundedRect(shadowBounds, measured(7.0f) + extent, measured(7.0f) + extent),
                brushes.shadow.get());
        }
        brushes.shadow->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.1f));
        context->FillRoundedRectangle(ToRoundedRect(card, measured(7.0f)), brushes.handleFill.get());
        context->DrawRoundedRectangle(
            ToRoundedRect(card, measured(7.0f)), brushes.panelEdge.get(), measured(1.0f));

        const float previewLeft = x + (cardWidth - previewSize) * 0.5f;
        const float previewTop = y + margin;
        const D2D1_RECT_F destination = D2D1::RectF(
            previewLeft, previewTop, previewLeft + previewSize, previewTop + previewSize);
        const D2D1_RECT_F source = D2D1::RectF(
            static_cast<float>(sourceLeft), static_cast<float>(sourceTop),
            static_cast<float>(sourceLeft + sampleCells), static_cast<float>(sourceTop + sampleCells));
        context->DrawBitmap(device.background(), &destination, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &source);

        const float selectedLeft = previewLeft + selectedX * cell;
        const float selectedTop = previewTop + selectedY * cell;
        const RECT selectedCell{ static_cast<LONG>(selectedLeft), static_cast<LONG>(selectedTop),
                                 static_cast<LONG>(selectedLeft + cell), static_cast<LONG>(selectedTop + cell) };
        context->DrawRectangle(
            ToRectF(selectedCell), brushes.handleFill.get(), measured(2.0f));
        RECT inner = selectedCell;
        InflateRect(&inner, -1, -1);
        context->DrawRectangle(
            ToRectF(inner), brushes.handleEdge.get(), measured(1.0f));

        wchar_t coordinates[64]{};
        swprintf_s(coordinates, L"X: %d Y: %d",
                   samplePoint.x + bounds.left, samplePoint.y + bounds.top);
        wchar_t hex[16]{};
        swprintf_s(hex, L"%02X%02X%02X", red, green, blue);

        const auto drawCenteredText = [&](const wchar_t* text, const D2D1_RECT_F& rect,
                                          ID2D1SolidColorBrush* brush)
        {
            winrt::com_ptr<IDWriteTextLayout> layout;
            AGSHOT_CHECK_HR(device.dwriteFactory()->CreateTextLayout(
                text, static_cast<UINT32>(wcslen(text)), device.magnifierTextFormat(),
                rect.right - rect.left, rect.bottom - rect.top, layout.put()));
            AGSHOT_CHECK_HR(layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
            AGSHOT_CHECK_HR(layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
            context->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.get(), brush);
        };

        const float coordinatesTop = previewTop + previewSize + measured(3.0f);
        drawCenteredText(coordinates, D2D1::RectF(
            x + margin, coordinatesTop, x + cardWidth - margin,
            coordinatesTop + measured(34.0f)),
            brushes.handleEdge.get());

        const float swatchInset = measured(5.0f);
        const float swatchTop = coordinatesTop + measured(42.0f);
        const RECT swatch{ static_cast<LONG>(x + swatchInset), static_cast<LONG>(swatchTop),
                           static_cast<LONG>(x + cardWidth - swatchInset),
                           static_cast<LONG>(y + cardHeight - measured(6.0f)) };
        brushes.accent->SetColor(D2D1::ColorF(red / 255.0f, green / 255.0f, blue / 255.0f));
        context->FillRoundedRectangle(
            ToRoundedRect(swatch, measured(5.0f)), brushes.accent.get());
        drawCenteredText(hex, ToRectF(swatch),
                         luminance > 0.58f ? brushes.handleEdge.get() : brushes.icon.get());
        brushes.accent->SetColor(D2D1::ColorF(0.18f, 0.49f, 0.94f, 1.0f));
    }

    void HudRenderer::DrawToolbar(const RenderDevice& device, const Toolbar& toolbar,
                                  bool textRecognitionMode)
    {
        DrawToolbar(device.d2dFactory(), device.d2dContext(),
                    device.roundStroke(), device.flatStroke(),
                    device.brushes(), device.dpi(), toolbar, textRecognitionMode);
    }

    void HudRenderer::DrawToolbar(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                                  ID2D1StrokeStyle* roundStroke, ID2D1StrokeStyle* flatStroke,
                                  const HudBrushes& brushes, int dpi,
                                  const Toolbar& toolbar, bool textRecognitionMode)
    {
        const float radius = ScaleByDpi(kCornerRadius, dpi);

        context->FillRoundedRectangle(ToRoundedRect(toolbar.panel, radius), brushes.panel.get());
        context->DrawRoundedRectangle(ToRoundedRect(toolbar.panel, radius), brushes.panelEdge.get(), 1.0f);

        const float gripX = static_cast<float>(toolbar.handle.left + toolbar.handle.right) * 0.5f;
        const float gripY = static_cast<float>(toolbar.handle.top + toolbar.handle.bottom) * 0.5f;
        const float dotRadius = ScaleByDpi(1.25f, dpi);
        for (int row = -1; row <= 1; ++row)
        {
            for (int column = -1; column <= 1; column += 2)
            {
                const D2D1_POINT_2F center{
                    gripX + ScaleByDpi(static_cast<float>(column * 3), dpi),
                    gripY + ScaleByDpi(static_cast<float>(row * 4), dpi) };
                context->FillEllipse(
                    D2D1::Ellipse(center, dotRadius, dotRadius), brushes.icon.get());
            }
        }

        const bool editActive = !textRecognitionMode;
        const bool textActive = textRecognitionMode;
        const float btnRadius = ScaleByDpi(kButtonRadius, dpi);
        const auto modeButton = [&](const RECT& rect, bool active, bool hovered)
        {
            if (active)
            {
                context->FillRoundedRectangle(ToRoundedRect(rect, btnRadius), brushes.accent.get());
            }
            else if (hovered)
            {
                context->FillRoundedRectangle(ToRoundedRect(rect, btnRadius), brushes.hover.get());
            }
        };
        modeButton(toolbar.editSelection, editActive,
                   toolbar.hovered == ToolbarButton::EditSelection);
        modeButton(toolbar.textRecognition, textActive,
                   toolbar.hovered == ToolbarButton::TextRecognition);
        context->FillRectangle(ToRectF(toolbar.divider), brushes.panelEdge.get());

        if (toolbar.hovered == ToolbarButton::Copy)
        {
            context->FillRoundedRectangle(ToRoundedRect(toolbar.copy, btnRadius), brushes.hover.get());
        }
        if (toolbar.hovered == ToolbarButton::Cancel)
        {
            context->FillRoundedRectangle(ToRoundedRect(toolbar.cancel, btnRadius), brushes.hover.get());
        }

        (void)factory;
        (void)roundStroke;
        (void)flatStroke;
        const int inset = MulDiv(kIconInset, dpi, 96);
        DrawToolbarSvg(context, ToolbarIconKind::EditSelection,
                       InsetRectBy(toolbar.editSelection, inset), dpi,
                       editActive ? brushes.handleFill.get() : brushes.icon.get());
        DrawToolbarSvg(context, ToolbarIconKind::TextRecognition,
                       InsetRectBy(toolbar.textRecognition, inset), dpi,
                       textActive ? brushes.handleFill.get() : brushes.icon.get());
        DrawToolbarSvg(context, ToolbarIconKind::Copy,
                       InsetRectBy(toolbar.copy, inset), dpi, brushes.icon.get());
        DrawToolbarSvg(context, ToolbarIconKind::Cancel,
                       InsetRectBy(toolbar.cancel, inset), dpi, brushes.cancel.get());
    }

    void HudRenderer::DrawToolbarSvg(ID2D1DeviceContext* context, ToolbarIconKind kind,
                                     const RECT& box, int dpi, ID2D1SolidColorBrush* brush)
    {
        EnsureToolbarSvgs(context);
        ID2D1SvgDocument* svg = m_iconSvgs[static_cast<std::size_t>(kind)].get();
        if (svg == nullptr)
        {
            return;
        }

        winrt::com_ptr<ID2D1DeviceContext5> context5;
        AGSHOT_CHECK_HR(context->QueryInterface(__uuidof(ID2D1DeviceContext5), context5.put_void()));

        winrt::com_ptr<ID2D1SvgElement> root;
        svg->GetRoot(root.put());
        if (root && brush != nullptr)
        {
            AGSHOT_CHECK_HR(root->SetAttributeValue(L"stroke", brush->GetColor()));
        }

        const float scale = ScaleByDpi(1.0f, dpi);
        const float inkSpan = kIconInk * scale;
        const float left = static_cast<float>(box.left)
            + (static_cast<float>(box.right - box.left) - inkSpan) * 0.5f;
        const float top = static_cast<float>(box.top)
            + (static_cast<float>(box.bottom - box.top) - inkSpan) * 0.5f;

        D2D1_MATRIX_3X2_F original{};
        context->GetTransform(&original);
        const D2D1_MATRIX_3X2_F iconTransform =
            D2D1::Matrix3x2F::Scale(scale, scale)
            * D2D1::Matrix3x2F::Translation(left, top)
            * original;
        context->SetTransform(iconTransform);
        context5->DrawSvgDocument(svg);
        context->SetTransform(original);
    }

    void HudRenderer::DrawTooltip(const RenderDevice& device, UINT surfaceWidth, UINT surfaceHeight,
                                  const Toolbar& toolbar)
    {
        if (toolbar.hovered == ToolbarButton::None)
        {
            return;
        }

        const LPCWSTR text = ToolbarLabel(toolbar.hovered);
        if (text == nullptr || text[0] == L'\0')
        {
            return;
        }

        if (!m_tooltipLayout || m_tooltipLayoutButton != toolbar.hovered)
        {
            m_tooltipLayout = nullptr;
            AGSHOT_CHECK_HR(device.dwriteFactory()->CreateTextLayout(
                text, static_cast<UINT32>(wcslen(text)), device.textFormat(),
                static_cast<float>(surfaceWidth), static_cast<float>(surfaceHeight),
                m_tooltipLayout.put()));
            m_tooltipLayoutButton = toolbar.hovered;
        }
        const auto& layout = m_tooltipLayout;

        DWRITE_TEXT_METRICS metrics{};
        AGSHOT_CHECK_HR(layout->GetMetrics(&metrics));

        const int dpi = device.dpi();
        const float padding = ScaleByDpi(kTooltipPadding, dpi);
        const float width = metrics.width + padding * 2.0f;
        const float height = metrics.height + padding;
        const float gap = ScaleByDpi(kTooltipGap, dpi);

        const RECT& button = toolbar.hovered == ToolbarButton::Copy
            ? toolbar.copy
            : toolbar.hovered == ToolbarButton::Cancel
                ? toolbar.cancel
                : toolbar.hovered == ToolbarButton::EditSelection
                    ? toolbar.editSelection
                    : toolbar.textRecognition;

        float x = static_cast<float>(button.left + button.right) / 2.0f - width / 2.0f;
        float y = static_cast<float>(button.bottom) + gap;

        if (y + height > static_cast<float>(surfaceHeight))
        {
            y = static_cast<float>(button.top) - gap - height;
        }
        x = std::clamp(x, 0.0f, std::max(0.0f, static_cast<float>(surfaceWidth) - width));
        y = std::clamp(y, 0.0f, std::max(0.0f, static_cast<float>(surfaceHeight) - height));

        const RECT box{ static_cast<LONG>(x), static_cast<LONG>(y),
                        static_cast<LONG>(x + width), static_cast<LONG>(y + height) };

        ID2D1DeviceContext* context = device.d2dContext();
        const HudBrushes& brushes = device.brushes();
        context->FillRoundedRectangle(ToRoundedRect(box, ScaleByDpi(4, dpi)), brushes.tooltip.get());
        context->DrawTextLayout(
            D2D1::Point2F(x + padding, y + padding * 0.5f), layout.get(), brushes.icon.get());
    }

#ifdef AGSHOT_TOOLBAR_PROOF
    bool HudRenderer::ExportToolbarProof(RenderDevice& device, OcrTextPanel& ocrPanel,
                                           const wchar_t* path, int dpi,
                                           bool textRecognitionActive,
                                           float ocrScrollProbe, bool ocrSpinnerProbe)
    {
        device.OnDpiChanged(dpi);
        device.EnsureProofContext();

        const RECT selection = textRecognitionActive ? RECT{ 0, 0, 360, 240 }
                                                     : RECT{ 0, 0, 600, 400 };
        const RECT bounds{ 0, 0, 1920, 1080 };
        Toolbar toolbar{};
        if (!LayoutToolbar(toolbar, selection, bounds, dpi))
        {
            return false;
        }
        if (textRecognitionActive)
        {
            ocrPanel.ConfigureProofState(selection, ocrScrollProbe, ocrSpinnerProbe);
        }

        const RECT area = textRecognitionActive
            ? RECT{ selection.left - 16, selection.top - 16,
                    selection.right + 16, selection.bottom + 16 }
            : RECT{ toolbar.panel.left - 16, toolbar.panel.top - 16,
                    toolbar.panel.right + 16, toolbar.panel.bottom + 16 };
        const UINT width = static_cast<UINT>(area.right - area.left);
        const UINT height = static_cast<UINT>(area.bottom - area.top);
        const D2D1_SIZE_U size = D2D1::SizeU(width, height);
        const UINT stride = width * 4;
        const UINT bytes = stride * height;

        winrt::com_ptr<ID2D1DeviceContext> target;
        AGSHOT_CHECK_HR(device.d2dDevice()->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, target.put()));

        const D2D1_BITMAP_PROPERTIES1 targetProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi), static_cast<float>(dpi));
        winrt::com_ptr<ID2D1Bitmap1> surface;
        AGSHOT_CHECK_HR(target->CreateBitmap(size, nullptr, 0, targetProperties, surface.put()));
        target->SetTarget(surface.get());

        const D2D1_BITMAP_PROPERTIES1 readProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi), static_cast<float>(dpi));
        winrt::com_ptr<ID2D1Bitmap1> readable;
        AGSHOT_CHECK_HR(target->CreateBitmap(size, nullptr, 0, readProperties, readable.put()));

        HudBrushes proofBrushes;
        CreateHudBrushes(target.get(), proofBrushes);

        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        target->SetTransform(D2D1::Matrix3x2F::Translation(
            -static_cast<float>(area.left), -static_cast<float>(area.top)));

        if (textRecognitionActive)
        {
            ocrPanel.Draw(target.get(), device.dwriteFactory(), device.textFormat(),
                          selection, dpi, proofBrushes);
        }
        DrawToolbar(device.d2dFactory(), target.get(),
                    device.roundStroke(), device.flatStroke(),
                    proofBrushes, dpi, toolbar, textRecognitionActive);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        AGSHOT_CHECK_HR(target->EndDraw());

        AGSHOT_CHECK_HR(readable->CopyFromBitmap(nullptr, surface.get(), nullptr));
        D2D1_MAPPED_RECT mapped{};
        AGSHOT_CHECK_HR(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped));

        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + bytes;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        bool ok = false;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"wb") == 0 && file != nullptr)
        {
            std::fwrite(&header, sizeof(header), 1, file);
            std::fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, file);
            for (UINT row = 0; row < height; ++row)
            {
                std::fwrite(mapped.bits + static_cast<size_t>(row) * mapped.pitch, 1, stride, file);
            }
            std::fclose(file);
            ok = true;
        }
        readable->Unmap();
        return ok;
    }
#endif
}
