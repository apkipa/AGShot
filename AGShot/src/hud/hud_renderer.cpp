#include "pch.h"

#include "hud/hud_renderer.h"

#include "core/embedded_resource.h"
#include "core/error.h"
#include "core/icon_pack.h"
#include "hud/ocr_text_panel.h"
#include "hud/ui/ui_slider.h"

namespace agshot
{
    namespace
    {
        constexpr int kBorderWidth = 2;
        constexpr int kCornerRadius = 6;
        constexpr int kLabelPadding = 7;
        constexpr int kTooltipPadding = 8;
        constexpr int kTooltipGap = 6;
        constexpr int kIconInset = 6;
        constexpr int kSubIconInset = 5;
        constexpr int kButtonRadius = 4;
        constexpr float kIconInk = 16.0f;

        RECT InsetRectBy(const RECT& rect, int by) noexcept
        {
            return RECT{ rect.left + by, rect.top + by, rect.right - by, rect.bottom - by };
        }

        void DrawRoundedColorTile(ID2D1DeviceContext* context,
                                  const D2D1_RECT_F& rect, float radius,
                                  const D2D1_COLOR_F& color, const HudBrushes& brushes, int dpi)
        {
            const D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(rect, radius, radius);
            if (color.a < 0.98f)
            {
                FillTiledCheckerboard(context, brushes, rect, ScaleByDpi(3.0f, dpi), radius);
            }
            if (HasVisibleColor(color))
            {
                context->FillRoundedRectangle(rrect, brushes.WithColor(color));
            }
        }

        void DrawFilledTriangle(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                                D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_POINT_2F p2,
                                ID2D1Brush* brush)
        {
            if (factory == nullptr || brush == nullptr)
            {
                return;
            }
            winrt::com_ptr<ID2D1PathGeometry> tri;
            AGSHOT_CHECK_HR(factory->CreatePathGeometry(tri.put()));
            winrt::com_ptr<ID2D1GeometrySink> sink;
            AGSHOT_CHECK_HR(tri->Open(sink.put()));
            sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(p1);
            sink->AddLine(p2);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            AGSHOT_CHECK_HR(sink->Close());
            context->FillGeometry(tri.get(), brush);
        }

        void DrawWellCornerCaret(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                                 const RECT& itemRect, bool active,
                                 const HudBrushes& brushes, int dpi)
        {
            const float rx = static_cast<float>(itemRect.right) - ScaleByDpi(3.2f, dpi);
            const float ry = static_cast<float>(itemRect.bottom) - ScaleByDpi(3.2f, dpi);
            const float triS = ScaleByDpi(4.5f, dpi);
            DrawFilledTriangle(
                factory, context,
                D2D1::Point2F(rx - triS, ry),
                D2D1::Point2F(rx, ry - triS),
                D2D1::Point2F(rx, ry),
                active ? brushes.accent.get() : brushes.handleFill.get());
        }

        winrt::com_ptr<ID2D1PathGeometry> CreateWellCutoutMask(
            ID2D1Factory1* factory, const RECT& itemRect, int dpi)
        {
            const float rx = static_cast<float>(itemRect.right) - ScaleByDpi(3.2f, dpi);
            const float ry = static_cast<float>(itemRect.bottom) - ScaleByDpi(3.2f, dpi);
            const float cutS = ScaleByDpi(7.6f, dpi);
            const float pad = ScaleByDpi(1.0f, dpi);

            winrt::com_ptr<ID2D1PathGeometry> corner;
            AGSHOT_CHECK_HR(factory->CreatePathGeometry(corner.put()));
            winrt::com_ptr<ID2D1GeometrySink> sink;
            AGSHOT_CHECK_HR(corner->Open(sink.put()));
            sink->BeginFigure(D2D1::Point2F(rx - cutS, ry + pad), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(rx + pad, ry - cutS));
            sink->AddLine(D2D1::Point2F(rx + pad, ry + pad));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            AGSHOT_CHECK_HR(sink->Close());
            return corner;
        }

        winrt::com_ptr<ID2D1PathGeometry> SubtractGeometry(
            ID2D1Factory1* factory, ID2D1Geometry* baseGeom, ID2D1Geometry* cutGeom)
        {
            winrt::com_ptr<ID2D1PathGeometry> out;
            AGSHOT_CHECK_HR(factory->CreatePathGeometry(out.put()));
            winrt::com_ptr<ID2D1GeometrySink> sink;
            AGSHOT_CHECK_HR(out->Open(sink.put()));
            AGSHOT_CHECK_HR(baseGeom->CombineWithGeometry(
                cutGeom, D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.get()));
            AGSHOT_CHECK_HR(sink->Close());
            return out;
        }

        void DrawStrokeWellOuterFrame(
            ID2D1Factory1* factory, ID2D1DeviceContext* context,
            const RECT& itemRect, AnnotationStrokeStyle strokeStyle,
            bool active, bool strokeVisible, int dpi)
        {
            const float cx = static_cast<float>(itemRect.left + itemRect.right) * 0.5f;
            const float cy = static_cast<float>(itemRect.top + itemRect.bottom) * 0.5f;
            const float halfF = ScaleByDpi(8.5f, dpi);
            const float L = cx - halfF;
            const float T = cy - halfF;
            const float R = cx + halfF;
            const float B = cy + halfF;
            const float sw = ScaleByDpi(1.4f, dpi);

            winrt::com_ptr<ID2D1SolidColorBrush> frameBrush;
            AGSHOT_CHECK_HR(context->CreateSolidColorBrush(
                D2D1::ColorF(0.96f, 0.97f, 0.99f, active ? 1.0f : (strokeVisible ? 0.88f : 0.55f)),
                frameBrush.put()));

            if (strokeStyle == AnnotationStrokeStyle::Dotted)
            {
                const float dotR = ScaleByDpi(0.95f, dpi);
                const float cIn = ScaleByDpi(0.35f, dpi);
                const float q = ScaleByDpi(4.25f, dpi);
                const float endOff = ScaleByDpi(3.8f, dpi);
                const D2D1_POINT_2F dots[15] = {
                    D2D1::Point2F(L + cIn, T + cIn),
                    D2D1::Point2F(cx - q, T),
                    D2D1::Point2F(cx, T),
                    D2D1::Point2F(cx + q, T),
                    D2D1::Point2F(R - cIn, T + cIn),
                    D2D1::Point2F(L, cy - q),
                    D2D1::Point2F(L, cy),
                    D2D1::Point2F(L, cy + q),
                    D2D1::Point2F(L + cIn, B - cIn),
                    D2D1::Point2F(R, cy - q),
                    D2D1::Point2F(R, cy),
                    D2D1::Point2F(R, cy + endOff),
                    D2D1::Point2F(cx - q, B),
                    D2D1::Point2F(cx, B),
                    D2D1::Point2F(cx + endOff, B),
                };
                for (const D2D1_POINT_2F& pt : dots)
                {
                    context->FillEllipse(D2D1::Ellipse(pt, dotR, dotR), frameBrush.get());
                }
                return;
            }

            if (factory != nullptr)
            {
                winrt::com_ptr<ID2D1StrokeStyle> capStyle;
                AGSHOT_CHECK_HR(factory->CreateStrokeStyle(
                    D2D1::StrokeStyleProperties(
                        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                        D2D1_LINE_JOIN_ROUND, 2.0f, D2D1_DASH_STYLE_SOLID, 0.0f),
                    nullptr, 0, capStyle.put()));

                winrt::com_ptr<ID2D1PathGeometry> path;
                AGSHOT_CHECK_HR(factory->CreatePathGeometry(path.put()));
                winrt::com_ptr<ID2D1GeometrySink> sink;
                AGSHOT_CHECK_HR(path->Open(sink.put()));

                if (strokeStyle == AnnotationStrokeStyle::Dashed)
                {
                    const float leg = ScaleByDpi(3.5f, dpi);
                    const float cr = ScaleByDpi(2.5f, dpi);
                    const float dHalf = ScaleByDpi(2.0f, dpi);
                    const float dEnd = ScaleByDpi(2.2f, dpi);

                    // Top-left corner
                    sink->BeginFigure(D2D1::Point2F(L, T + leg), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(L, T + cr));
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(L + cr, T), D2D1::SizeF(cr, cr), 0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddLine(D2D1::Point2F(L + leg, T));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);

                    // Top mid-dash
                    sink->BeginFigure(D2D1::Point2F(cx - dHalf, T), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(cx + dHalf, T));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);

                    // Top-right corner
                    sink->BeginFigure(D2D1::Point2F(R - leg, T), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(R - cr, T));
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(R, T + cr), D2D1::SizeF(cr, cr), 0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddLine(D2D1::Point2F(R, T + leg));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);

                    // Right mid-dash (stops before corner caret)
                    sink->BeginFigure(D2D1::Point2F(R, cy - dHalf), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(R, cy + dEnd));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);

                    // Left mid-dash
                    sink->BeginFigure(D2D1::Point2F(L, cy - dHalf), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(L, cy + dHalf));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);

                    // Bottom-left corner
                    sink->BeginFigure(D2D1::Point2F(L, B - leg), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(L, B - cr));
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(L + cr, B), D2D1::SizeF(cr, cr), 0.0f,
                        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddLine(D2D1::Point2F(L + leg, B));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);

                    // Bottom mid-dash (stops before corner caret)
                    sink->BeginFigure(D2D1::Point2F(cx - dHalf, B), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(cx + dEnd, B));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                }
                else
                {
                    const float cr = ScaleByDpi(2.7f, dpi);
                    const float endOff = ScaleByDpi(3.6f, dpi);
                    sink->BeginFigure(D2D1::Point2F(cx + endOff, B), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(L + cr, B));
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(L, B - cr), D2D1::SizeF(cr, cr), 0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddLine(D2D1::Point2F(L, T + cr));
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(L + cr, T), D2D1::SizeF(cr, cr), 0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddLine(D2D1::Point2F(R - cr, T));
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(R, T + cr), D2D1::SizeF(cr, cr), 0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->AddLine(D2D1::Point2F(R, cy + endOff));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                }

                AGSHOT_CHECK_HR(sink->Close());
                context->DrawGeometry(path.get(), frameBrush.get(), sw, capStyle.get());
                return;
            }

            context->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(L, T, R, B), ScaleByDpi(2.7f, dpi), ScaleByDpi(2.7f, dpi)),
                frameBrush.get(), sw);
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
        m_subTextLayouts.clear();

        m_sizeLabelLayout = nullptr;
        m_sizeLabelSelection = RECT{};
        m_sizeLabelValid = false;

        m_tooltipLayout = nullptr;
        m_tooltipLayoutButton = ToolbarButton::None;
        m_tooltipLayoutSub = SubToolbarAction::None;
    }

    IDWriteTextLayout* HudRenderer::GetOrCreateSubTextLayout(
        IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
        std::wstring_view text, float widthPx, float heightPx, int dpi, int styleKind)
    {
        if (dwrite == nullptr || textFormat == nullptr || text.empty())
        {
            return nullptr;
        }
        const int wKey = static_cast<int>(std::lround(widthPx));
        const int hKey = static_cast<int>(std::lround(heightPx));
        for (const CachedSubTextLayout& entry : m_subTextLayouts)
        {
            if (entry.factory == dwrite
                && entry.format == textFormat
                && entry.widthPx == wKey
                && entry.heightPx == hKey
                && entry.dpi == dpi
                && entry.styleKind == styleKind
                && entry.text == text
                && entry.layout)
            {
                return entry.layout.get();
            }
        }

        if (m_subTextLayouts.size() >= 64)
        {
            m_subTextLayouts.clear();
        }

        winrt::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(dwrite->CreateTextLayout(
                text.data(), static_cast<UINT32>(text.size()),
                textFormat, std::max(1.0f, widthPx), std::max(1.0f, heightPx),
                layout.put())))
        {
            return nullptr;
        }
        if (styleKind == 2)
        {
            const DWRITE_TEXT_RANGE full{ 0, static_cast<UINT32>(text.size()) };
            layout->SetFontSize(ScaleByDpi(9.8f, dpi), full);
            layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, full);
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        else if (styleKind == 1)
        {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        else
        {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        IDWriteTextLayout* result = layout.get();
        m_subTextLayouts.push_back(CachedSubTextLayout{
            dwrite, textFormat, std::wstring(text), wKey, hKey, dpi, styleKind, std::move(layout)
        });
        return result;
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

        const IconPack& pack = SharedHudIconPack();
        for (std::size_t i = 0; i < m_iconSvgs.size(); ++i)
        {
            const std::string_view svg = pack.SvgByIndex(i);
            if (svg.empty())
            {
                continue;
            }
            winrt::com_ptr<IStream> stream;
            AGSHOT_CHECK_HR(CreateMemoryStream(svg.data(), static_cast<DWORD>(svg.size()), stream));
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
                                    const RECT& selection, bool smart, int handleSize,
                                    bool showHandles)
    {
        const int dpi = device.dpi();
        ID2D1DeviceContext* context = device.d2dContext();
        const HudBrushes& brushes = device.brushes();
        const float stroke = ScaleByDpi(kBorderWidth, dpi);

        RECT border = selection;
        const int half = static_cast<int>(stroke / 2.0f);
        InflateRect(&border, -half, -half);

        context->DrawRectangle(ToRectF(border), brushes.accent.get(), stroke);

        if (!smart && showHandles)
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
        const float luminance = RelativeLuminance(
            red / 255.0f, green / 255.0f, blue / 255.0f);

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
        }
        auto popLayer = wil::scope_exit([&]
        {
            if (translucent)
            {
                context->PopLayer();
            }
        });

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
                                  ToolbarButton activeMode)
    {
        DrawToolbar(device.d2dFactory(), device.d2dContext(),
                    device.dwriteFactory(), device.textFormat(),
                    device.brushes(), device.dpi(), toolbar, activeMode);
    }

    void HudRenderer::DrawToolbar(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                                  IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
                                  const HudBrushes& brushes, int dpi,
                                  const Toolbar& toolbar, ToolbarButton activeMode)
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

        const float btnRadius = ScaleByDpi(kButtonRadius, dpi);
        const int inset = MulDiv(kIconInset, dpi, 96);
        for (std::size_t i = 0; i < toolbar.buttons.size(); ++i)
        {
            const auto button = static_cast<ToolbarButton>(i);
            const RECT& rect = toolbar.buttons[i];
            const bool enabled = toolbar.isEnabled(button);
            const bool active = (activeMode == button)
                && button != ToolbarButton::Undo
                && button != ToolbarButton::Redo
                && button != ToolbarButton::Cancel
                && button != ToolbarButton::Copy;
            const bool hovered = enabled && (toolbar.hovered == button);

            if (active)
            {
                context->FillRoundedRectangle(ToRoundedRect(rect, btnRadius), brushes.accent.get());
            }
            else if (hovered)
            {
                context->FillRoundedRectangle(ToRoundedRect(rect, btnRadius), brushes.hover.get());
            }

            ID2D1SolidColorBrush* iconBrush = brushes.icon.get();
            if (!enabled)
            {
                iconBrush = brushes.disabledIcon.get();
            }
            else if (button == ToolbarButton::Cancel)
            {
                iconBrush = brushes.cancel.get();
            }
            else if (active)
            {
                iconBrush = brushes.handleFill.get();
            }

            DrawToolbarSvg(context, static_cast<ToolbarIconKind>(i),
                           InsetRectBy(rect, inset), dpi, iconBrush);
        }

        for (const RECT& div : toolbar.dividers)
        {
            context->FillRectangle(ToRectF(div), brushes.panelEdge.get());
        }

        if (toolbar.subVisible)
        {
            DrawSubToolbar(factory, context, dwrite, textFormat, brushes, dpi, toolbar);
        }
    }

    void HudRenderer::DrawSubToolbar(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                                     IDWriteFactory* dwrite, IDWriteTextFormat* textFormat,
                                     const HudBrushes& brushes, int dpi, const Toolbar& toolbar)
    {
        const float radius = ScaleByDpi(kCornerRadius, dpi);
        const float btnRadius = ScaleByDpi(kButtonRadius, dpi);
        const int iconInset = MulDiv(kSubIconInset, dpi, 96);

        const auto iconForSub = [](SubToolbarAction action) noexcept -> std::optional<ToolbarIconKind>
        {
            switch (action)
            {
            case SubToolbarAction::ShapeRect:
                return ToolbarIconKind::SubShapeRect;
            case SubToolbarAction::ShapeEllipse:
                return ToolbarIconKind::SubShapeEllipse;
            case SubToolbarAction::ShapeDiamond:
                return ToolbarIconKind::SubShapeDiamond;
            case SubToolbarAction::LineArrow:
                return ToolbarIconKind::SubLineArrow;
            case SubToolbarAction::LineStraight:
                return ToolbarIconKind::SubLineStraight;
            case SubToolbarAction::MosaicBrush:
                return ToolbarIconKind::SubMosaicBrush;
            case SubToolbarAction::MosaicRect:
                return ToolbarIconKind::SubMosaicRect;
            case SubToolbarAction::BrushHighlighterToggle:
                return ToolbarIconKind::SubBrushHighlighter;
            case SubToolbarAction::FillModeNone:
                return ToolbarIconKind::SubFillNone;
            case SubToolbarAction::FillModeSolid:
                return ToolbarIconKind::SubFillSolid;
            case SubToolbarAction::FillPatternCrossHatch:
                return ToolbarIconKind::SubFillCrossHatch;
            case SubToolbarAction::FillPatternDiagHatch:
                return ToolbarIconKind::SubFillDiagHatch;
            case SubToolbarAction::OrderSendToBack:
                return ToolbarIconKind::SubOrderSendToBack;
            case SubToolbarAction::OrderSendBackward:
                return ToolbarIconKind::SubOrderSendBackward;
            case SubToolbarAction::OrderBringForward:
                return ToolbarIconKind::SubOrderBringForward;
            case SubToolbarAction::OrderBringToFront:
                return ToolbarIconKind::SubOrderBringToFront;
            case SubToolbarAction::AlignLeft:
                return ToolbarIconKind::SubAlignLeft;
            case SubToolbarAction::AlignCenterH:
                return ToolbarIconKind::SubAlignCenterH;
            case SubToolbarAction::AlignRight:
                return ToolbarIconKind::SubAlignRight;
            case SubToolbarAction::DistributeH:
                return ToolbarIconKind::SubDistributeH;
            case SubToolbarAction::AlignTop:
                return ToolbarIconKind::SubAlignTop;
            case SubToolbarAction::AlignCenterV:
                return ToolbarIconKind::SubAlignCenterV;
            case SubToolbarAction::AlignBottom:
                return ToolbarIconKind::SubAlignBottom;
            case SubToolbarAction::DistributeV:
                return ToolbarIconKind::SubDistributeV;
            case SubToolbarAction::DuplicateSelected:
                return ToolbarIconKind::SubDuplicate;
            case SubToolbarAction::DeleteSelected:
                return ToolbarIconKind::SubDelete;
            default:
                return std::nullopt;
            }
        };

        const auto drawRow = [&](const RECT& panelRect, const std::vector<SubToolbarItem>& items)
        {
            context->FillRoundedRectangle(ToRoundedRect(panelRect, radius), brushes.panel.get());
            context->DrawRoundedRectangle(ToRoundedRect(panelRect, radius), brushes.panelEdge.get(), 1.0f);

            for (const SubToolbarItem& item : items)
        {
            if (item.kind == SubItemKind::Divider)
            {
                context->FillRectangle(ToRectF(item.rect), brushes.panelEdge.get());
                continue;
            }

            const bool hovered = item.enabled && (toolbar.hoveredSub == item.action);
            if (item.action == SubToolbarAction::BadgeNumberValue)
            {
                continue;
            }
            const bool wellLike = (item.kind == SubItemKind::ColorSwatch
                || item.kind == SubItemKind::StrokeWell
                || item.kind == SubItemKind::FillWell
                || item.kind == SubItemKind::DropdownPill);
            if (item.kind != SubItemKind::Slider && item.kind != SubItemKind::StaticIcon)
            {
                if (item.enabled && item.active && !wellLike)
                {
                    context->FillRoundedRectangle(ToRoundedRect(item.rect, btnRadius), brushes.accent.get());
                }
                else if (hovered || (item.enabled && item.active && wellLike && item.kind != SubItemKind::ColorSwatch))
                {
                    context->FillRoundedRectangle(ToRoundedRect(item.rect, btnRadius), brushes.hover.get());
                }
            }

            switch (item.kind)
            {
            case SubItemKind::IconToggle:
                if (const auto iconKind = iconForSub(item.action))
                {
                    ID2D1SolidColorBrush* iconBrush = !item.enabled
                        ? brushes.disabledIcon.get()
                        : (item.destructive
                            ? brushes.cancel.get()
                            : (item.active ? brushes.handleFill.get() : brushes.icon.get()));
                    DrawToolbarSvg(context, *iconKind, InsetRectBy(item.rect, iconInset), dpi, iconBrush);
                }
                break;

            case SubItemKind::StrokeWell:
            {
                const float cx = static_cast<float>(item.rect.left + item.rect.right) * 0.5f;
                const float cy = static_cast<float>(item.rect.top + item.rect.bottom) * 0.5f;

                DrawStrokeWellOuterFrame(
                    factory, context, item.rect, item.strokeStyle,
                    item.active, HasVisibleColor(item.swatchColor), dpi);

                const float halfCore = ScaleByDpi(6.0f, dpi);
                const D2D1_RECT_F coreRect{ cx - halfCore, cy - halfCore, cx + halfCore, cy + halfCore };
                const D2D1_ROUNDED_RECT coreRR = D2D1::RoundedRect(
                    coreRect, ScaleByDpi(1.6f, dpi), ScaleByDpi(1.6f, dpi));

                if (factory != nullptr)
                {
                    winrt::com_ptr<ID2D1RoundedRectangleGeometry> coreGeom;
                    AGSHOT_CHECK_HR(factory->CreateRoundedRectangleGeometry(coreRR, coreGeom.put()));
                    winrt::com_ptr<ID2D1PathGeometry> cutMask = CreateWellCutoutMask(
                        factory, item.rect, dpi);
                    winrt::com_ptr<ID2D1PathGeometry> finalCore = SubtractGeometry(
                        factory, coreGeom.get(), cutMask.get());

                    if (item.swatchColor.a < 0.98f)
                    {
                        FillTiledCheckerboardGeometry(
                            context, brushes, finalCore.get(), coreRect, ScaleByDpi(3.0f, dpi));
                    }
                    if (HasVisibleColor(item.swatchColor))
                    {
                        context->FillGeometry(finalCore.get(), brushes.WithColor(item.swatchColor));
                    }
                    context->DrawGeometry(finalCore.get(), brushes.panelEdge.get(), 1.0f);
                    DrawWellCornerCaret(factory, context, item.rect, item.active, brushes, dpi);
                }
                else
                {
                    DrawRoundedColorTile(
                        context, coreRect, ScaleByDpi(1.6f, dpi),
                        item.swatchColor, brushes, dpi);
                    context->DrawRoundedRectangle(coreRR, brushes.panelEdge.get(), 1.0f);
                }
                if (item.active)
                {
                    context->DrawRoundedRectangle(
                        ToRoundedRect(item.rect, btnRadius),
                        brushes.accent.get(), ScaleByDpi(1.5f, dpi));
                }
                break;
            }

            case SubItemKind::FillWell:
            {
                const float inset = ScaleByDpi(4.0f, dpi);
                const D2D1_RECT_F swatch{
                    static_cast<float>(item.rect.left) + inset,
                    static_cast<float>(item.rect.top) + inset,
                    static_cast<float>(item.rect.right) - inset,
                    static_cast<float>(item.rect.bottom) - inset,
                };
                const float swatchRadius = ScaleByDpi(3.4f, dpi);
                const D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(swatch, swatchRadius, swatchRadius);

                if (factory != nullptr)
                {
                    winrt::com_ptr<ID2D1RoundedRectangleGeometry> boxGeom;
                    AGSHOT_CHECK_HR(factory->CreateRoundedRectangleGeometry(rrect, boxGeom.put()));
                    winrt::com_ptr<ID2D1PathGeometry> cutMask = CreateWellCutoutMask(
                        factory, item.rect, dpi);
                    winrt::com_ptr<ID2D1PathGeometry> finalFill = SubtractGeometry(
                        factory, boxGeom.get(), cutMask.get());

                    if (item.swatchColor.a < 0.98f)
                    {
                        FillTiledCheckerboardGeometry(
                            context, brushes, finalFill.get(), swatch, ScaleByDpi(3.0f, dpi));
                    }
                    if (HasVisibleColor(item.swatchColor))
                    {
                        context->FillGeometry(finalFill.get(), brushes.WithColor(item.swatchColor));
                    }
                    context->DrawGeometry(finalFill.get(), brushes.panelEdge.get(), 1.0f);
                    DrawWellCornerCaret(factory, context, item.rect, item.active, brushes, dpi);
                }
                else
                {
                    DrawRoundedColorTile(context, swatch, swatchRadius, item.swatchColor, brushes, dpi);
                    context->DrawRoundedRectangle(rrect, brushes.panelEdge.get(), 1.0f);
                }
                if (item.active)
                {
                    context->DrawRoundedRectangle(
                        ToRoundedRect(item.rect, btnRadius),
                        brushes.accent.get(), ScaleByDpi(1.5f, dpi));
                }
                break;
            }

            case SubItemKind::ColorSwatch:
            {
                const float inset = ScaleByDpi(5.0f, dpi);
                const D2D1_RECT_F swatch{
                    static_cast<float>(item.rect.left) + inset,
                    static_cast<float>(item.rect.top) + inset,
                    static_cast<float>(item.rect.right) - inset,
                    static_cast<float>(item.rect.bottom) - inset,
                };
                const float swatchRadius = ScaleByDpi(2.8f, dpi);
                const D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(swatch, swatchRadius, swatchRadius);
                DrawRoundedColorTile(context, swatch, swatchRadius, item.swatchColor, brushes, dpi);

                const bool ring = item.active;
                if (ring)
                {
                    context->DrawRoundedRectangle(rrect, brushes.handleEdge.get(), ScaleByDpi(1.0f, dpi));
                }
                context->DrawRoundedRectangle(
                    rrect,
                    ring ? brushes.handleFill.get() : brushes.panelEdge.get(),
                    ring ? ScaleByDpi(1.8f, dpi) : 1.0f);
                break;
            }

            case SubItemKind::StrokeWidth:
            {
                constexpr float kBarHeightsDip[4] = { 2.0f, 3.5f, 5.5f, 8.0f };
                const int idx = std::clamp(item.variant, 0, 3);
                const float barH = ScaleByDpi(kBarHeightsDip[idx], dpi);
                const float hPad = ScaleByDpi(5.0f, dpi);
                const float cy = static_cast<float>(item.rect.top + item.rect.bottom) * 0.5f;
                const D2D1_RECT_F bar{
                    static_cast<float>(item.rect.left) + hPad,
                    cy - barH * 0.5f,
                    static_cast<float>(item.rect.right) - hPad,
                    cy + barH * 0.5f,
                };
                context->FillRoundedRectangle(
                    D2D1::RoundedRect(bar, barH * 0.5f, barH * 0.5f),
                    !item.enabled
                        ? brushes.disabledIcon.get()
                        : (item.active ? brushes.handleFill.get() : brushes.icon.get()));
                break;
            }

            case SubItemKind::BadgeShape:
            {
                const int variant = std::clamp(item.variant, 0, 3);
                const bool isSquare = (variant == 2 || variant == 3);
                const bool isFilled = (variant == 1 || variant == 3);
                const float cx = static_cast<float>(item.rect.left + item.rect.right) * 0.5f;
                const float cy = static_cast<float>(item.rect.top + item.rect.bottom) * 0.5f;
                const float r = ScaleByDpi(7.0f, dpi);
                const D2D1_RECT_F box{ cx - r, cy - r, cx + r, cy + r };
                ID2D1SolidColorBrush* fg = item.active ? brushes.handleFill.get() : brushes.icon.get();
                ID2D1SolidColorBrush* invFg = item.active
                    ? brushes.accent.get()
                    : (hovered ? brushes.hover.get() : brushes.panel.get());
                const float strokeW = ScaleByDpi(1.65f, dpi);

                if (isSquare)
                {
                    const float cr = ScaleByDpi(2.4f, dpi);
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(box, cr, cr);
                    if (isFilled)
                    {
                        context->FillRoundedRectangle(rr, fg);
                    }
                    else
                    {
                        context->DrawRoundedRectangle(rr, fg, strokeW);
                    }
                }
                else
                {
                    const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
                    if (isFilled)
                    {
                        context->FillEllipse(circle, fg);
                    }
                    else
                    {
                        context->DrawEllipse(circle, fg, strokeW);
                    }
                }

                if (IDWriteTextLayout* layout = GetOrCreateSubTextLayout(
                        dwrite, textFormat, L"1", r * 2.0f, r * 2.0f, dpi, 2))
                {
                    context->DrawTextLayout(
                        D2D1::Point2F(box.left, box.top),
                        layout,
                        isFilled ? invFg : fg);
                }
                break;
            }

            case SubItemKind::TextPill:
            case SubItemKind::ValuePill:
                if (item.kind == SubItemKind::ValuePill && !item.active)
                {
                    context->DrawRoundedRectangle(
                        ToRoundedRect(item.rect, btnRadius), brushes.panelEdge.get(), 1.0f);
                }
                if (item.label[0] != L'\0')
                {
                    const float w = static_cast<float>(item.rect.right - item.rect.left);
                    const float h = static_cast<float>(item.rect.bottom - item.rect.top);
                    if (IDWriteTextLayout* layout = GetOrCreateSubTextLayout(
                            dwrite, textFormat, item.label, w, h, dpi, 0))
                    {
                        context->DrawTextLayout(
                            D2D1::Point2F(static_cast<float>(item.rect.left), static_cast<float>(item.rect.top)),
                            layout,
                            !item.enabled
                                ? brushes.disabledIcon.get()
                                : (item.active ? brushes.handleFill.get() : brushes.icon.get()));
                    }
                }
                break;

            case SubItemKind::DropdownPill:
            {
                context->DrawRoundedRectangle(
                    ToRoundedRect(item.rect, btnRadius),
                    item.active ? brushes.accent.get() : brushes.panelEdge.get(),
                    item.active ? ScaleByDpi(1.4f, dpi) : 1.0f);
                const float padL = ScaleByDpi(8.0f, dpi);
                const float chevronSpace = ScaleByDpi(18.0f, dpi);
                if (item.label[0] != L'\0')
                {
                    const float w = std::max(1.0f, static_cast<float>(item.rect.right - item.rect.left) - padL - chevronSpace);
                    const float h = static_cast<float>(item.rect.bottom - item.rect.top);
                    if (IDWriteTextLayout* layout = GetOrCreateSubTextLayout(
                            dwrite, textFormat, item.label, w, h, dpi, 1))
                    {
                        context->DrawTextLayout(
                            D2D1::Point2F(static_cast<float>(item.rect.left) + padL, static_cast<float>(item.rect.top)),
                            layout,
                            item.enabled ? brushes.icon.get() : brushes.disabledIcon.get());
                    }
                }
                const float cx = static_cast<float>(item.rect.right) - ScaleByDpi(9.5f, dpi);
                const float cy = static_cast<float>(item.rect.top + item.rect.bottom) * 0.5f;
                const float dx = ScaleByDpi(3.2f, dpi);
                const float dy = ScaleByDpi(1.7f, dpi);
                const float sw = ScaleByDpi(1.35f, dpi);
                ID2D1SolidColorBrush* chBrush = item.enabled ? brushes.icon.get() : brushes.disabledIcon.get();
                context->DrawLine(D2D1::Point2F(cx - dx, cy - dy), D2D1::Point2F(cx, cy + dy), chBrush, sw);
                context->DrawLine(D2D1::Point2F(cx, cy + dy), D2D1::Point2F(cx + dx, cy - dy), chBrush, sw);
                break;
            }

            case SubItemKind::StaticIcon:
            {
                if (item.variant >= 0 && item.variant < static_cast<int>(ToolbarIconKind::Count))
                {
                    const int iconSize = MulDiv(16, dpi, 96);
                    const int iconLeft = item.rect.left + ((item.rect.right - item.rect.left) - iconSize) / 2;
                    const int iconTop = item.rect.top + ((item.rect.bottom - item.rect.top) - iconSize) / 2;
                    const RECT iconBox{ iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize };
                    DrawToolbarSvg(
                        context, static_cast<ToolbarIconKind>(item.variant), iconBox, dpi,
                        item.enabled ? brushes.icon.get() : brushes.disabledIcon.get());
                }
                break;
            }

            case SubItemKind::Slider:
            {
                const RECT trackHit = MosaicSliderTrackRect(item.rect, dpi);
                const float trackH = ScaleByDpi(4.0f, dpi);
                const float trackR = trackH * 0.5f;
                const float cy = static_cast<float>(trackHit.top + trackHit.bottom) * 0.5f;
                const float tLeft = static_cast<float>(trackHit.left);
                const float tRight = static_cast<float>(trackHit.right);
                const float norm = std::clamp(item.sliderValue, 0.0f, 1.0f);
                const float fillX = tLeft + norm * (tRight - tLeft);

                const D2D1_RECT_F fullBar{ tLeft, cy - trackR, tRight, cy + trackR };
                context->FillRoundedRectangle(
                    D2D1::RoundedRect(fullBar, trackR, trackR),
                    brushes.panelEdge.get());
                if (fillX > tLeft + 0.5f)
                {
                    const D2D1_RECT_F activeBar{ tLeft, cy - trackR, fillX, cy + trackR };
                    context->FillRoundedRectangle(
                        D2D1::RoundedRect(activeBar, trackR, trackR),
                        brushes.accent.get());
                }

                ui::Slider1D slider;
                slider.Arrange(trackHit);
                slider.DrawThumb(context, brushes, norm, brushes.accent->GetColor(), dpi);
                break;
            }

            case SubItemKind::Divider:
                break;
            }
            }
        };

        if (toolbar.subVisible)
        {
            drawRow(toolbar.subPanel, toolbar.subItems);
        }
        if (toolbar.subVisible2)
        {
            drawRow(toolbar.subPanel2, toolbar.subItems2);
        }
    }

    ID2D1SvgDocument* HudRenderer::EyedropperSvg(ID2D1DeviceContext* context)
    {
        if (context == nullptr)
        {
            return nullptr;
        }
        EnsureToolbarSvgs(context);
        return m_iconSvgs[static_cast<std::size_t>(ToolbarIconKind::Eyedropper)].get();
    }

    void HudRenderer::DrawToolbarSvg(ID2D1DeviceContext* context, ToolbarIconKind kind,
                                     const RECT& box, int dpi, ID2D1SolidColorBrush* brush)
    {
        (void)dpi;
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
            AGSHOT_CHECK_HR(root->SetAttributeValue(L"fill", brush->GetColor()));
        }

        const float boxW = static_cast<float>(box.right - box.left);
        const float boxH = static_cast<float>(box.bottom - box.top);
        const float inkSpan = std::max(1.0f, std::min(boxW, boxH));
        const float scale = inkSpan / kIconInk;
        const float left = static_cast<float>(box.left) + (boxW - inkSpan) * 0.5f;
        const float top = static_cast<float>(box.top) + (boxH - inkSpan) * 0.5f;

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
        LPCWSTR text = nullptr;
        RECT anchor{};
        if (toolbar.hovered != ToolbarButton::None)
        {
            text = ToolbarLabel(toolbar.hovered);
            anchor = toolbar.buttonRect(toolbar.hovered);
        }
        else if (toolbar.hoveredSub != SubToolbarAction::None)
        {
            text = SubToolbarLabel(toolbar.hoveredSub);
            if (const SubToolbarItem* sub = toolbar.findSubItem(toolbar.hoveredSub))
            {
                anchor = sub->rect;
            }
        }

        if (text == nullptr || text[0] == L'\0' || IsRectEmpty(&anchor))
        {
            return;
        }

        if (!m_tooltipLayout
            || m_tooltipLayoutButton != toolbar.hovered
            || m_tooltipLayoutSub != toolbar.hoveredSub)
        {
            m_tooltipLayout = nullptr;
            AGSHOT_CHECK_HR(device.dwriteFactory()->CreateTextLayout(
                text, static_cast<UINT32>(wcslen(text)), device.textFormat(),
                static_cast<float>(surfaceWidth), static_cast<float>(surfaceHeight),
                m_tooltipLayout.put()));
            m_tooltipLayoutButton = toolbar.hovered;
            m_tooltipLayoutSub = toolbar.hoveredSub;
        }
        const auto& layout = m_tooltipLayout;

        DWRITE_TEXT_METRICS metrics{};
        AGSHOT_CHECK_HR(layout->GetMetrics(&metrics));

        const int dpi = device.dpi();
        const float padding = ScaleByDpi(kTooltipPadding, dpi);
        const float width = metrics.width + padding * 2.0f;
        const float height = metrics.height + padding;
        const float gap = ScaleByDpi(kTooltipGap, dpi);

        float x = static_cast<float>(anchor.left + anchor.right) / 2.0f - width / 2.0f;
        float y = static_cast<float>(anchor.bottom) + gap;
        if (y + height > static_cast<float>(surfaceHeight)
            || (toolbar.subVisible && toolbar.hovered != ToolbarButton::None
                && toolbar.subPanel.top > toolbar.panel.bottom
                && y + height > static_cast<float>(toolbar.subPanel.top)))
        {
            y = static_cast<float>(anchor.top) - gap - height;
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
        ToolbarState proofState{};
        proofState.activeMode = textRecognitionActive
            ? ToolbarButton::TextRecognition
            : ToolbarButton::EditSelection;
        Toolbar toolbar{};
        if (!LayoutToolbar(toolbar, proofState, selection, bounds, dpi))
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
                    device.dwriteFactory(), device.textFormat(),
                    proofBrushes, dpi, toolbar, proofState.activeMode);
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
