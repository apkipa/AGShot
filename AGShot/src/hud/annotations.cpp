#include "pch.h"

#include "core/clipboard.h"
#include "core/error.h"
#include "hud/annotations.h"

namespace agshot
{
    namespace
    {
        float DistancePointToSegment(D2D1_POINT_2F p, D2D1_POINT_2F a, D2D1_POINT_2F b) noexcept
        {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float lenSq = dx * dx + dy * dy;
            if (lenSq <= 1e-4f)
            {
                return std::hypot(p.x - a.x, p.y - a.y);
            }
            const float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq, 0.0f, 1.0f);
            const float projX = a.x + t * dx;
            const float projY = a.y + t * dy;
            return std::hypot(p.x - projX, p.y - projY);
        }

        bool PointInRectF(D2D1_POINT_2F pt, const D2D1_RECT_F& r, float inflate = 0.0f) noexcept
        {
            return pt.x >= (r.left - inflate) && pt.x <= (r.right + inflate)
                && pt.y >= (r.top - inflate) && pt.y <= (r.bottom + inflate);
        }

        bool RectsIntersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) noexcept
        {
            return a.left <= b.right && a.right >= b.left
                && a.top <= b.bottom && a.bottom >= b.top;
        }

        winrt::com_ptr<ID2D1Geometry> BuildShapeGeometry(
            ID2D1Factory1* factory, const AnnotationItem& item, const D2D1_RECT_F& r, int dpi)
        {
            if (factory == nullptr)
            {
                return {};
            }

            switch (item.style.shapeKind)
            {
            case AnnotationShapeKind::Rectangle:
            {
                const float w = r.right - r.left;
                const float h = r.bottom - r.top;
                if (item.style.cornerRadiusDip > 0.5f)
                {
                    const float radius = std::min({ ScaleByDpi(item.style.cornerRadiusDip, dpi), w * 0.5f, h * 0.5f });
                    winrt::com_ptr<ID2D1RoundedRectangleGeometry> geom;
                    AGSHOT_CHECK_HR(factory->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(r, radius, radius), geom.put()));
                    return geom;
                }
                winrt::com_ptr<ID2D1RectangleGeometry> geom;
                AGSHOT_CHECK_HR(factory->CreateRectangleGeometry(r, geom.put()));
                return geom;
            }
            case AnnotationShapeKind::Ellipse:
            {
                const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
                    D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f),
                    (r.right - r.left) * 0.5f,
                    (r.bottom - r.top) * 0.5f);
                winrt::com_ptr<ID2D1EllipseGeometry> geom;
                AGSHOT_CHECK_HR(factory->CreateEllipseGeometry(ellipse, geom.put()));
                return geom;
            }
            case AnnotationShapeKind::Diamond:
            {
                const float mx = (r.left + r.right) * 0.5f;
                const float my = (r.top + r.bottom) * 0.5f;
                const D2D1_POINT_2F v[4] = {
                    D2D1::Point2F(mx, r.top),
                    D2D1::Point2F(r.right, my),
                    D2D1::Point2F(mx, r.bottom),
                    D2D1::Point2F(r.left, my),
                };
                winrt::com_ptr<ID2D1PathGeometry> path;
                AGSHOT_CHECK_HR(factory->CreatePathGeometry(path.put()));
                winrt::com_ptr<ID2D1GeometrySink> sink;
                AGSHOT_CHECK_HR(path->Open(sink.put()));

                const float edgeLen = std::hypot((r.right - r.left) * 0.5f, (r.bottom - r.top) * 0.5f);
                if (item.style.cornerRadiusDip > 0.5f && edgeLen > 1.0f)
                {
                    const float cut = std::min(ScaleByDpi(item.style.cornerRadiusDip, dpi), edgeLen * 0.45f);
                    const float t = cut / edgeLen;
                    const auto lerpPt = [](const D2D1_POINT_2F& a, const D2D1_POINT_2F& b, float f) noexcept
                    {
                        return D2D1::Point2F(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f);
                    };
                    const D2D1_POINT_2F startPt = lerpPt(v[0], v[3], t);
                    sink->BeginFigure(startPt, D2D1_FIGURE_BEGIN_FILLED);
                    for (int i = 0; i < 4; ++i)
                    {
                        const D2D1_POINT_2F& cur = v[i];
                        const D2D1_POINT_2F& next = v[(i + 1) % 4];
                        const D2D1_POINT_2F outPt = lerpPt(cur, next, t);
                        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(cur, outPt));
                        if (i < 3)
                        {
                            const D2D1_POINT_2F nextIn = lerpPt(next, cur, t);
                            sink->AddLine(nextIn);
                        }
                    }
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                }
                else
                {
                    sink->BeginFigure(v[0], D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLines(&v[1], 3);
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                }
                AGSHOT_CHECK_HR(sink->Close());
                return path;
            }
            }
            return {};
        }

        void DrawHatchFill(ID2D1DeviceContext* context, ID2D1Geometry* clipGeom,
                           const D2D1_RECT_F& bounds, ID2D1Brush* brush,
                           AnnotationFillPattern pattern, int dpi)
        {
            if (context == nullptr || clipGeom == nullptr || brush == nullptr)
            {
                return;
            }
            const float step = std::max(3.0f, ScaleByDpi(7.5f, dpi));
            const float lineW = std::max(1.0f, ScaleByDpi(1.5f, dpi));
            const float h = bounds.bottom - bounds.top;

            context->PushLayer(D2D1::LayerParameters(bounds, clipGeom), nullptr);
            for (float x = bounds.left - h; x <= bounds.right + step; x += step)
            {
                context->DrawLine(
                    D2D1::Point2F(x, bounds.bottom),
                    D2D1::Point2F(x + h, bounds.top),
                    brush, lineW);
                if (pattern == AnnotationFillPattern::CrossHatch)
                {
                    context->DrawLine(
                        D2D1::Point2F(x, bounds.top),
                        D2D1::Point2F(x + h, bounds.bottom),
                        brush, lineW);
                }
            }
            context->PopLayer();
        }

        winrt::com_ptr<ID2D1PathGeometry> BuildPolylineGeometry(
            ID2D1Factory1* factory, const std::vector<D2D1_POINT_2F>& points)
        {
            if (factory == nullptr || points.size() < 2)
            {
                return {};
            }

            winrt::com_ptr<ID2D1PathGeometry> path;
            AGSHOT_CHECK_HR(factory->CreatePathGeometry(path.put()));

            winrt::com_ptr<ID2D1GeometrySink> sink;
            AGSHOT_CHECK_HR(path->Open(sink.put()));
            sink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLines(points.data() + 1, static_cast<UINT32>(points.size() - 1));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            AGSHOT_CHECK_HR(sink->Close());
            return path;
        }

        void DrawArrow(ID2D1Factory1* factory, ID2D1DeviceContext* context,
                       ID2D1StrokeStyle* strokeStyle, ID2D1Brush* brush,
                       D2D1_POINT_2F start, D2D1_POINT_2F end,
                       float strokePx, int dpi)
        {
            const float dx = end.x - start.x;
            const float dy = end.y - start.y;
            const float length = std::hypot(dx, dy);
            if (length < 0.5f)
            {
                return;
            }

            const float headLen = std::clamp(
                strokePx * 4.2f,
                ScaleByDpi(10.0f, dpi),
                std::max(ScaleByDpi(10.0f, dpi), length * 0.65f));
            const float headHalfW = headLen * 0.42f;
            const float ux = dx / length;
            const float uy = dy / length;
            const float px = -uy;
            const float py = ux;

            const D2D1_POINT_2F shaftEnd{
                end.x - ux * (headLen * 0.62f),
                end.y - uy * (headLen * 0.62f)
            };
            if (length > headLen * 0.62f)
            {
                context->DrawLine(start, shaftEnd, brush, strokePx, strokeStyle);
            }

            const D2D1_POINT_2F wing1{
                end.x - ux * headLen + px * headHalfW,
                end.y - uy * headLen + py * headHalfW
            };
            const D2D1_POINT_2F notch{
                end.x - ux * (headLen * 0.72f),
                end.y - uy * (headLen * 0.72f)
            };
            const D2D1_POINT_2F wing2{
                end.x - ux * headLen - px * headHalfW,
                end.y - uy * headLen - py * headHalfW
            };

            winrt::com_ptr<ID2D1PathGeometry> head;
            AGSHOT_CHECK_HR(factory->CreatePathGeometry(head.put()));
            winrt::com_ptr<ID2D1GeometrySink> sink;
            AGSHOT_CHECK_HR(head->Open(sink.put()));
            sink->BeginFigure(end, D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F poly[3] = { wing1, notch, wing2 };
            sink->AddLines(poly, 3);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            AGSHOT_CHECK_HR(sink->Close());
            context->FillGeometry(head.get(), brush);
        }

        void DrawSelectionChromeSingle(ID2D1DeviceContext* context, ID2D1SolidColorBrush* scratchBrush,
                                       const AnnotationItem& item, IDWriteFactory* dwrite, int dpi)
        {
            const D2D1_COLOR_F accentColor = D2D1::ColorF(0.18f, 0.49f, 0.94f, 1.0f);
            const D2D1_COLOR_F fillColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            const float stroke = ScaleByDpi(1.5f, dpi);
            const float handleSize = ScaleByDpi(8.0f, dpi);

            if (item.kind == AnnotationKind::Line)
            {
                scratchBrush->SetColor(accentColor);
                context->DrawLine(item.start, item.end, scratchBrush, stroke);
                const float r = handleSize * 0.5f;
                for (const D2D1_POINT_2F pt : { item.start, item.end })
                {
                    const D2D1_ELLIPSE dot = D2D1::Ellipse(pt, r, r);
                    scratchBrush->SetColor(fillColor);
                    context->FillEllipse(dot, scratchBrush);
                    scratchBrush->SetColor(accentColor);
                    context->DrawEllipse(dot, scratchBrush, stroke);
                }
                return;
            }

            const D2D1_RECT_F bounds = AnnotationBounds(item, dwrite, dpi);
            scratchBrush->SetColor(accentColor);
            context->DrawRectangle(bounds, scratchBrush, stroke);

            if (item.kind == AnnotationKind::Shape
                || (item.kind == AnnotationKind::Mosaic
                    && item.style.mosaicKind == AnnotationMosaicKind::Rectangle))
            {
                for (const D2D1_RECT_F& h : AnnotationBoxHandles(bounds, handleSize))
                {
                    scratchBrush->SetColor(fillColor);
                    context->FillRectangle(h, scratchBrush);
                    scratchBrush->SetColor(accentColor);
                    context->DrawRectangle(h, scratchBrush, ScaleByDpi(1.25f, dpi));
                }
            }
        }

        void DrawSelectionChromeMulti(ID2D1DeviceContext* context, ID2D1SolidColorBrush* scratchBrush,
                                      ID2D1StrokeStyle* dashStyle,
                                      const std::vector<const AnnotationItem*>& items,
                                      IDWriteFactory* dwrite, int dpi)
        {
            if (items.empty() || context == nullptr || scratchBrush == nullptr)
            {
                return;
            }
            scratchBrush->SetColor(D2D1::ColorF(0.18f, 0.49f, 0.94f, 0.95f));

            const float itemStroke = ScaleByDpi(1.25f, dpi);
            D2D1_RECT_F unionBounds = AnnotationBounds(*items[0], dwrite, dpi);
            for (const AnnotationItem* item : items)
            {
                const D2D1_RECT_F b = AnnotationBounds(*item, dwrite, dpi);
                context->DrawRectangle(b, scratchBrush, itemStroke);
                unionBounds.left = std::min(unionBounds.left, b.left);
                unionBounds.top = std::min(unionBounds.top, b.top);
                unionBounds.right = std::max(unionBounds.right, b.right);
                unionBounds.bottom = std::max(unionBounds.bottom, b.bottom);
            }

            const float pad = ScaleByDpi(4.0f, dpi);
            unionBounds.left -= pad;
            unionBounds.top -= pad;
            unionBounds.right += pad;
            unionBounds.bottom += pad;
            context->DrawRectangle(unionBounds, scratchBrush, ScaleByDpi(1.5f, dpi), dashStyle);
        }
    }

    D2D1_RECT_F NormalizeAnnotationRect(D2D1_RECT_F rect) noexcept
    {
        if (rect.left > rect.right)
        {
            std::swap(rect.left, rect.right);
        }
        if (rect.top > rect.bottom)
        {
            std::swap(rect.top, rect.bottom);
        }
        return rect;
    }

    D2D1_RECT_F ConstrainDraftRect(POINT origin, POINT current, bool lockSquare, bool fromCenter) noexcept
    {
        const float ox = static_cast<float>(origin.x);
        const float oy = static_cast<float>(origin.y);
        float dx = static_cast<float>(current.x - origin.x);
        float dy = static_cast<float>(current.y - origin.y);
        if (lockSquare)
        {
            const float side = std::max(std::fabs(dx), std::fabs(dy));
            dx = (dx < 0.0f) ? -side : side;
            dy = (dy < 0.0f) ? -side : side;
        }
        if (fromCenter)
        {
            return NormalizeAnnotationRect(D2D1::RectF(ox - dx, oy - dy, ox + dx, oy + dy));
        }
        return NormalizeAnnotationRect(D2D1::RectF(ox, oy, ox + dx, oy + dy));
    }

    D2D1_POINT_2F ConstrainLineEndpoint(D2D1_POINT_2F anchor, D2D1_POINT_2F freePt, bool snapAngle) noexcept
    {
        if (!snapAngle)
        {
            return freePt;
        }
        const float dx = freePt.x - anchor.x;
        const float dy = freePt.y - anchor.y;
        const float len = std::hypot(dx, dy);
        if (len < 1e-3f)
        {
            return freePt;
        }
        constexpr float kStep = 3.14159265f / 12.0f; // 15 deg
        const float angle = std::round(std::atan2(dy, dx) / kStep) * kStep;
        return D2D1::Point2F(anchor.x + len * std::cos(angle), anchor.y + len * std::sin(angle));
    }

    winrt::com_ptr<IDWriteTextLayout> CreateAnnotationTextLayout(
        IDWriteFactory* dwrite, const AnnotationItem& item, int dpi)
    {
        if (dwrite == nullptr)
        {
            return {};
        }

        const float fontSize = ScaleByDpi(item.style.fontSizeDip, dpi);
        winrt::com_ptr<IDWriteTextFormat> format;
        AGSHOT_CHECK_HR(dwrite->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"", format.put()));

        const float maxW = (item.textMaxWidth > 1.0f) ? item.textMaxWidth : 10000.0f;
        const std::wstring& content = item.text;
        winrt::com_ptr<IDWriteTextLayout> layout;
        AGSHOT_CHECK_HR(dwrite->CreateTextLayout(
            content.c_str(), static_cast<UINT32>(content.size()),
            format.get(), maxW, 10000.0f, layout.put()));
        if (item.textMaxWidth <= 1.0f)
        {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        return layout;
    }

    D2D1_RECT_F AnnotationBounds(const AnnotationItem& item, IDWriteFactory* dwrite, int dpi)
    {
        const float strokeHalf = ScaleByDpi(item.style.strokeWidthDip, dpi) * 0.5f;
        switch (item.kind)
        {
        case AnnotationKind::Shape:
            return NormalizeAnnotationRect(item.rect);
        case AnnotationKind::Mosaic:
            if (item.style.mosaicKind == AnnotationMosaicKind::Rectangle)
            {
                return NormalizeAnnotationRect(item.rect);
            }
            [[fallthrough]];
        case AnnotationKind::Brush:
        {
            if (item.points.empty())
            {
                return D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
            }
            float minX = item.points.front().x;
            float minY = item.points.front().y;
            float maxX = minX;
            float maxY = minY;
            for (const D2D1_POINT_2F& pt : item.points)
            {
                minX = std::min(minX, pt.x);
                minY = std::min(minY, pt.y);
                maxX = std::max(maxX, pt.x);
                maxY = std::max(maxY, pt.y);
            }
            const float pad = (item.kind == AnnotationKind::Mosaic)
                ? ScaleByDpi(item.style.mosaicBrushDip, dpi) * 0.5f
                : strokeHalf;
            return D2D1::RectF(minX - pad, minY - pad, maxX + pad, maxY + pad);
        }
        case AnnotationKind::Line:
            return D2D1::RectF(
                std::min(item.start.x, item.end.x) - strokeHalf,
                std::min(item.start.y, item.end.y) - strokeHalf,
                std::max(item.start.x, item.end.x) + strokeHalf,
                std::max(item.start.y, item.end.y) + strokeHalf);
        case AnnotationKind::StepBadge:
        {
            const float r = ScaleByDpi(item.style.badgeRadiusDip, dpi);
            return D2D1::RectF(item.origin.x - r, item.origin.y - r, item.origin.x + r, item.origin.y + r);
        }
        case AnnotationKind::Text:
        {
            const float pad = ScaleByDpi(6.0f, dpi);
            float w = ScaleByDpi(24.0f, dpi);
            float h = ScaleByDpi(item.style.fontSizeDip * 1.35f, dpi);
            if (auto layout = CreateAnnotationTextLayout(dwrite, item, dpi))
            {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics)))
                {
                    w = std::max(w, metrics.widthIncludingTrailingWhitespace);
                    h = std::max(h, metrics.height);
                }
            }
            return D2D1::RectF(
                item.origin.x - pad,
                item.origin.y - pad,
                item.origin.x + w + pad,
                item.origin.y + h + pad);
        }
        }
        return D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
    }

    std::array<D2D1_RECT_F, 8> AnnotationBoxHandles(const D2D1_RECT_F& bounds, float handleSize) noexcept
    {
        const float half = handleSize * 0.5f;
        const float midX = (bounds.left + bounds.right) * 0.5f;
        const float midY = (bounds.top + bounds.bottom) * 0.5f;
        const auto box = [half](float cx, float cy) noexcept
        {
            return D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
        };
        return
        {
            box(bounds.left, bounds.top),
            box(midX, bounds.top),
            box(bounds.right, bounds.top),
            box(bounds.right, midY),
            box(bounds.right, bounds.bottom),
            box(midX, bounds.bottom),
            box(bounds.left, bounds.bottom),
            box(bounds.left, midY),
        };
    }

    AnnotationHandle HitTestAnnotationHandle(
        const AnnotationItem& item, POINT point, IDWriteFactory* dwrite, int dpi, float handleSize)
    {
        const D2D1_POINT_2F pt{ static_cast<float>(point.x), static_cast<float>(point.y) };
        const float grabRadius = std::max(ScaleByDpi(6.0f, dpi), handleSize);

        if (item.kind == AnnotationKind::Line)
        {
            if (std::hypot(pt.x - item.start.x, pt.y - item.start.y) <= grabRadius)
            {
                return AnnotationHandle::LineStart;
            }
            if (std::hypot(pt.x - item.end.x, pt.y - item.end.y) <= grabRadius)
            {
                return AnnotationHandle::LineEnd;
            }
            return HitTestAnnotationBody(item, point, dwrite, dpi) ? AnnotationHandle::Body : AnnotationHandle::None;
        }

        if (item.kind == AnnotationKind::Shape
            || (item.kind == AnnotationKind::Mosaic
                && item.style.mosaicKind == AnnotationMosaicKind::Rectangle))
        {
            const D2D1_RECT_F bounds = AnnotationBounds(item, dwrite, dpi);
            const auto handles = AnnotationBoxHandles(bounds, handleSize);
            constexpr AnnotationHandle kHandleOrder[8] =
            {
                AnnotationHandle::TopLeft,
                AnnotationHandle::Top,
                AnnotationHandle::TopRight,
                AnnotationHandle::Right,
                AnnotationHandle::BottomRight,
                AnnotationHandle::Bottom,
                AnnotationHandle::BottomLeft,
                AnnotationHandle::Left,
            };
            const float pad = ScaleByDpi(4.0f, dpi);
            for (std::size_t i = 0; i < handles.size(); ++i)
            {
                if (PointInRectF(pt, handles[i], pad))
                {
                    return kHandleOrder[i];
                }
            }
        }

        return HitTestAnnotationBody(item, point, dwrite, dpi) ? AnnotationHandle::Body : AnnotationHandle::None;
    }

    bool HitTestAnnotationBody(const AnnotationItem& item, POINT point, IDWriteFactory* dwrite, int dpi)
    {
        const D2D1_POINT_2F pt{ static_cast<float>(point.x), static_cast<float>(point.y) };
        const float hitPad = std::max(ScaleByDpi(5.0f, dpi), ScaleByDpi(item.style.strokeWidthDip, dpi) * 0.5f);

        switch (item.kind)
        {
        case AnnotationKind::Shape:
        {
            const D2D1_RECT_F r = NormalizeAnnotationRect(item.rect);
            const bool filled = HasVisibleColor(item.style.fillColor);
            if (item.style.shapeKind == AnnotationShapeKind::Rectangle)
            {
                if (filled)
                {
                    return PointInRectF(pt, r, hitPad);
                }
                const bool inOuter = PointInRectF(pt, r, hitPad);
                const bool inInner = (r.right - r.left > hitPad * 2.0f && r.bottom - r.top > hitPad * 2.0f)
                    && PointInRectF(pt, r, -hitPad);
                return inOuter && !inInner;
            }

            const float rx = (r.right - r.left) * 0.5f;
            const float ry = (r.bottom - r.top) * 0.5f;
            if (rx < 1.0f || ry < 1.0f)
            {
                return false;
            }
            const float cx = (r.left + r.right) * 0.5f;
            const float cy = (r.top + r.bottom) * 0.5f;
            const float nx = std::fabs(pt.x - cx) / rx;
            const float ny = std::fabs(pt.y - cy) / ry;

            if (item.style.shapeKind == AnnotationShapeKind::Diamond)
            {
                const float tol = hitPad / std::min(rx, ry);
                const float d = nx + ny;
                if (filled)
                {
                    return d <= 1.0f + tol;
                }
                return d >= std::max(0.0f, 1.0f - tol) && d <= 1.0f + tol;
            }

            const float distSq = nx * nx + ny * ny;
            if (filled)
            {
                return distSq <= 1.15f;
            }
            const float tol = hitPad / std::min(rx, ry);
            return distSq >= std::max(0.0f, (1.0f - tol) * (1.0f - tol))
                && distSq <= (1.0f + tol) * (1.0f + tol);
        }
        case AnnotationKind::Line:
            return DistancePointToSegment(pt, item.start, item.end) <= std::max(hitPad, ScaleByDpi(6.0f, dpi));
        case AnnotationKind::Brush:
        case AnnotationKind::Mosaic:
        {
            if (item.kind == AnnotationKind::Mosaic && item.style.mosaicKind == AnnotationMosaicKind::Rectangle)
            {
                return PointInRectF(pt, NormalizeAnnotationRect(item.rect), ScaleByDpi(3.0f, dpi));
            }
            if (item.points.empty())
            {
                return false;
            }
            const float tol = (item.kind == AnnotationKind::Mosaic)
                ? std::max(ScaleByDpi(6.0f, dpi), ScaleByDpi(item.style.mosaicBrushDip, dpi) * 0.5f)
                : std::max(ScaleByDpi(6.0f, dpi), hitPad);
            if (item.points.size() == 1)
            {
                return std::hypot(pt.x - item.points.front().x, pt.y - item.points.front().y) <= tol;
            }
            for (std::size_t i = 1; i < item.points.size(); ++i)
            {
                if (DistancePointToSegment(pt, item.points[i - 1], item.points[i]) <= tol)
                {
                    return true;
                }
            }
            return false;
        }
        case AnnotationKind::StepBadge:
        {
            const float r = ScaleByDpi(item.style.badgeRadiusDip, dpi) + ScaleByDpi(3.0f, dpi);
            if (item.style.badgeShape == AnnotationBadgeShape::OutlineSquare
                || item.style.badgeShape == AnnotationBadgeShape::FilledSquare)
            {
                return std::fabs(pt.x - item.origin.x) <= r && std::fabs(pt.y - item.origin.y) <= r;
            }
            return std::hypot(pt.x - item.origin.x, pt.y - item.origin.y) <= r;
        }
        case AnnotationKind::Text:
            return PointInRectF(pt, AnnotationBounds(item, dwrite, dpi));
        }
        return false;
    }

    void MoveAnnotation(AnnotationItem& item, float dx, float dy) noexcept
    {
        item.rect.left += dx;
        item.rect.right += dx;
        item.rect.top += dy;
        item.rect.bottom += dy;
        item.start.x += dx;
        item.start.y += dy;
        item.end.x += dx;
        item.end.y += dy;
        item.origin.x += dx;
        item.origin.y += dy;
        for (D2D1_POINT_2F& pt : item.points)
        {
            pt.x += dx;
            pt.y += dy;
        }
    }

    AnnotationItem DragAnnotationHandle(
        const AnnotationItem& start, AnnotationHandle handle, POINT origin, POINT current,
        TransformModifiers mods) noexcept
    {
        AnnotationItem result = start;
        const float dx = static_cast<float>(current.x - origin.x);
        const float dy = static_cast<float>(current.y - origin.y);

        if (handle == AnnotationHandle::Body)
        {
            MoveAnnotation(result, dx, dy);
            return result;
        }

        if (result.kind == AnnotationKind::Line)
        {
            if (handle == AnnotationHandle::LineStart)
            {
                result.start.x += dx;
                result.start.y += dy;
                result.start = ConstrainLineEndpoint(result.end, result.start, mods.shift);
            }
            else if (handle == AnnotationHandle::LineEnd)
            {
                result.end.x += dx;
                result.end.y += dy;
                result.end = ConstrainLineEndpoint(result.start, result.end, mods.shift);
            }
            return result;
        }

        const D2D1_RECT_F orig = NormalizeAnnotationRect(start.rect);
        const float origW = std::max(1.0f, orig.right - orig.left);
        const float origH = std::max(1.0f, orig.bottom - orig.top);
        const float cx = (orig.left + orig.right) * 0.5f;
        const float cy = (orig.top + orig.bottom) * 0.5f;

        const bool hasLeft = (handle == AnnotationHandle::Left
            || handle == AnnotationHandle::TopLeft || handle == AnnotationHandle::BottomLeft);
        const bool hasRight = (handle == AnnotationHandle::Right
            || handle == AnnotationHandle::TopRight || handle == AnnotationHandle::BottomRight);
        const bool hasTop = (handle == AnnotationHandle::Top
            || handle == AnnotationHandle::TopLeft || handle == AnnotationHandle::TopRight);
        const bool hasBottom = (handle == AnnotationHandle::Bottom
            || handle == AnnotationHandle::BottomLeft || handle == AnnotationHandle::BottomRight);
        const bool isCorner = (hasLeft || hasRight) && (hasTop || hasBottom);

        if (mods.shift && isCorner)
        {
            const float dragX = hasLeft ? (orig.left + dx) : (orig.right + dx);
            const float dragY = hasTop ? (orig.top + dy) : (orig.bottom + dy);
            const float anchorX = mods.alt ? cx : (hasLeft ? orig.right : orig.left);
            const float anchorY = mods.alt ? cy : (hasTop ? orig.bottom : orig.top);

            const float vx = dragX - anchorX;
            const float vy = dragY - anchorY;
            const float sx = (vx == 0.0f) ? (hasLeft ? -1.0f : 1.0f) : (vx < 0.0f ? -1.0f : 1.0f);
            const float sy = (vy == 0.0f) ? (hasTop ? -1.0f : 1.0f) : (vy < 0.0f ? -1.0f : 1.0f);

            const float scale = mods.alt
                ? std::max(std::fabs(vx) / (origW * 0.5f), std::fabs(vy) / (origH * 0.5f))
                : std::max(std::fabs(vx) / origW, std::fabs(vy) / origH);

            if (mods.alt)
            {
                const float hw = scale * origW * 0.5f;
                const float hh = scale * origH * 0.5f;
                result.rect = NormalizeAnnotationRect(D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh));
            }
            else
            {
                result.rect = NormalizeAnnotationRect(D2D1::RectF(
                    anchorX, anchorY,
                    anchorX + sx * scale * origW,
                    anchorY + sy * scale * origH));
            }
            return result;
        }

        D2D1_RECT_F r = orig;
        if (hasLeft)
        {
            r.left += dx;
            if (mods.alt) r.right -= dx;
        }
        if (hasRight)
        {
            r.right += dx;
            if (mods.alt) r.left -= dx;
        }
        if (hasTop)
        {
            r.top += dy;
            if (mods.alt) r.bottom -= dy;
        }
        if (hasBottom)
        {
            r.bottom += dy;
            if (mods.alt) r.top -= dy;
        }
        result.rect = NormalizeAnnotationRect(r);
        return result;
    }

    std::wstring FormatStepBadgeLabel(int number, AnnotationBadgeNumberStyle style)
    {
        const int n = std::clamp(number, 1, 999);
        switch (style)
        {
        case AnnotationBadgeNumberStyle::Arabic:
            return std::to_wstring(n);

        case AnnotationBadgeNumberStyle::Roman:
        {
            struct RomanPair { int value; const wchar_t* sym; };
            constexpr RomanPair kTable[] =
            {
                { 900, L"CM" }, { 500, L"D" }, { 400, L"CD" }, { 100, L"C" },
                { 90, L"XC" },  { 50, L"L" },  { 40, L"XL" },  { 10, L"X" },
                { 9, L"IX" },   { 5, L"V" },   { 4, L"IV" },   { 1, L"I" },
            };
            int rem = n;
            std::wstring out;
            for (const RomanPair& p : kTable)
            {
                while (rem >= p.value)
                {
                    out += p.sym;
                    rem -= p.value;
                }
            }
            return out;
        }

        case AnnotationBadgeNumberStyle::LowerAlpha:
        case AnnotationBadgeNumberStyle::UpperAlpha:
        {
            const wchar_t base = (style == AnnotationBadgeNumberStyle::LowerAlpha) ? L'a' : L'A';
            int rem = n;
            std::wstring out;
            while (rem > 0)
            {
                --rem;
                out.push_back(static_cast<wchar_t>(base + (rem % 26)));
                rem /= 26;
            }
            std::reverse(out.begin(), out.end());
            return out;
        }

        case AnnotationBadgeNumberStyle::Chinese:
        {
            constexpr wchar_t kDigits[10] =
            {
                L'\u96F6', L'\u4E00', L'\u4E8C', L'\u4E09', L'\u56DB',
                L'\u4E94', L'\u516D', L'\u4E03', L'\u516B', L'\u4E5D',
            };
            constexpr wchar_t kTen = L'\u5341';
            constexpr wchar_t kHundred = L'\u767E';

            const int h = n / 100;
            const int t = (n % 100) / 10;
            const int o = n % 10;
            std::wstring out;
            if (h > 0)
            {
                out.push_back(kDigits[h]);
                out.push_back(kHundred);
                if (t == 0 && o > 0)
                {
                    out.push_back(kDigits[0]);
                }
            }
            if (t > 0)
            {
                if (h > 0 || t > 1)
                {
                    out.push_back(kDigits[t]);
                }
                out.push_back(kTen);
            }
            if (o > 0)
            {
                out.push_back(kDigits[o]);
            }
            return out;
        }
        }
        return std::to_wstring(n);
    }

    LPCWSTR CursorForAnnotationHandle(AnnotationHandle handle) noexcept
    {
        switch (handle)
        {
        case AnnotationHandle::Body:
        case AnnotationHandle::LineStart:
        case AnnotationHandle::LineEnd:
            return IDC_SIZEALL;
        case AnnotationHandle::Left:
        case AnnotationHandle::Right:
            return IDC_SIZEWE;
        case AnnotationHandle::Top:
        case AnnotationHandle::Bottom:
            return IDC_SIZENS;
        case AnnotationHandle::TopLeft:
        case AnnotationHandle::BottomRight:
            return IDC_SIZENWSE;
        case AnnotationHandle::TopRight:
        case AnnotationHandle::BottomLeft:
            return IDC_SIZENESW;
        case AnnotationHandle::None:
        default:
            return IDC_ARROW;
        }
    }

    void AnnotationDocument::Clear() noexcept
    {
        m_items.clear();
        m_undoStack.clear();
        m_redoStack.clear();
        m_nextId = 1;
        m_selectedIds.clear();
    }

    bool AnnotationDocument::IsSelected(std::uint64_t id) const noexcept
    {
        return id != 0
            && std::find(m_selectedIds.begin(), m_selectedIds.end(), id) != m_selectedIds.end();
    }

    void AnnotationDocument::SetSelectedId(std::uint64_t id) noexcept
    {
        m_selectedIds.clear();
        if (id != 0 && FindItem(id) != nullptr)
        {
            m_selectedIds.push_back(id);
        }
    }

    void AnnotationDocument::SetSelectedIds(std::vector<std::uint64_t> ids)
    {
        m_selectedIds.clear();
        for (std::uint64_t id : ids)
        {
            if (id != 0 && FindItem(id) != nullptr && !IsSelected(id))
            {
                m_selectedIds.push_back(id);
            }
        }
    }

    void AnnotationDocument::ToggleSelectedId(std::uint64_t id)
    {
        if (id == 0 || FindItem(id) == nullptr)
        {
            return;
        }
        const auto it = std::find(m_selectedIds.begin(), m_selectedIds.end(), id);
        if (it != m_selectedIds.end())
        {
            m_selectedIds.erase(it);
        }
        else
        {
            m_selectedIds.push_back(id);
        }
    }

    void AnnotationDocument::SelectAll()
    {
        m_selectedIds.clear();
        m_selectedIds.reserve(m_items.size());
        for (const AnnotationItem& item : m_items)
        {
            m_selectedIds.push_back(item.id);
        }
    }

    const AnnotationItem* AnnotationDocument::FindItem(std::uint64_t id) const noexcept
    {
        for (const AnnotationItem& item : m_items)
        {
            if (item.id == id)
            {
                return &item;
            }
        }
        return nullptr;
    }

    AnnotationItem* AnnotationDocument::FindItem(std::uint64_t id) noexcept
    {
        for (AnnotationItem& item : m_items)
        {
            if (item.id == id)
            {
                return &item;
            }
        }
        return nullptr;
    }

    const AnnotationItem* AnnotationDocument::SelectedItem() const noexcept
    {
        const std::uint64_t id = selectedId();
        return (id != 0) ? FindItem(id) : nullptr;
    }

    int AnnotationDocument::NextStepNumber() const noexcept
    {
        int maxStep = 0;
        for (const AnnotationItem& item : m_items)
        {
            if (item.kind == AnnotationKind::StepBadge)
            {
                maxStep = std::max(maxStep, item.stepNumber);
            }
        }
        return maxStep + 1;
    }

    std::uint64_t AnnotationDocument::AddItem(AnnotationItem item)
    {
        PushUndoSnapshot(m_items);
        item.id = m_nextId++;
        const std::uint64_t assigned = item.id;
        m_items.push_back(std::move(item));
        return assigned;
    }

    bool AnnotationDocument::DeleteItem(std::uint64_t id)
    {
        for (std::size_t i = 0; i < m_items.size(); ++i)
        {
            if (m_items[i].id == id)
            {
                PushUndoSnapshot(m_items);
                m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(i));
                ReconcileSelection();
                return true;
            }
        }
        return false;
    }

    bool AnnotationDocument::DeleteSelected()
    {
        if (m_selectedIds.empty())
        {
            return false;
        }
        bool any = false;
        for (const AnnotationItem& item : m_items)
        {
            if (IsSelected(item.id))
            {
                any = true;
                break;
            }
        }
        if (!any)
        {
            m_selectedIds.clear();
            return false;
        }

        PushUndoSnapshot(m_items);
        std::erase_if(m_items, [this](const AnnotationItem& item) {
            return IsSelected(item.id);
        });
        m_selectedIds.clear();
        return true;
    }

    bool AnnotationDocument::CanBringSelectedForward() const noexcept
    {
        if (m_selectedIds.empty() || m_items.size() < 2)
        {
            return false;
        }
        bool seenSelected = false;
        for (const AnnotationItem& item : m_items)
        {
            if (IsSelected(item.id))
            {
                seenSelected = true;
            }
            else if (seenSelected)
            {
                return true;
            }
        }
        return false;
    }

    bool AnnotationDocument::CanSendSelectedBackward() const noexcept
    {
        if (m_selectedIds.empty() || m_items.size() < 2)
        {
            return false;
        }
        bool seenUnselected = false;
        for (const AnnotationItem& item : m_items)
        {
            if (!IsSelected(item.id))
            {
                seenUnselected = true;
            }
            else if (seenUnselected)
            {
                return true;
            }
        }
        return false;
    }

    bool AnnotationDocument::ReorderSelected(AnnotationZOrderOp op)
    {
        if (m_selectedIds.empty() || m_items.size() < 2)
        {
            return false;
        }
        const bool wantUp = (op == AnnotationZOrderOp::BringForward || op == AnnotationZOrderOp::BringToFront);
        if (wantUp ? !CanBringSelectedForward() : !CanSendSelectedBackward())
        {
            return false;
        }

        PushUndoSnapshot(m_items);

        switch (op)
        {
        case AnnotationZOrderOp::SendToBack:
            std::stable_partition(m_items.begin(), m_items.end(), [this](const AnnotationItem& item) {
                return IsSelected(item.id);
            });
            break;

        case AnnotationZOrderOp::BringToFront:
            std::stable_partition(m_items.begin(), m_items.end(), [this](const AnnotationItem& item) {
                return !IsSelected(item.id);
            });
            break;

        case AnnotationZOrderOp::SendBackward:
            for (std::size_t i = 1; i < m_items.size(); ++i)
            {
                if (IsSelected(m_items[i].id) && !IsSelected(m_items[i - 1].id))
                {
                    std::swap(m_items[i], m_items[i - 1]);
                }
            }
            break;

        case AnnotationZOrderOp::BringForward:
            for (std::size_t i = m_items.size() - 1; i-- > 0;)
            {
                if (IsSelected(m_items[i].id) && !IsSelected(m_items[i + 1].id))
                {
                    std::swap(m_items[i], m_items[i + 1]);
                }
            }
            break;
        }
        return true;
    }

    bool AnnotationDocument::DuplicateSelected(float dx, float dy)
    {
        if (m_selectedIds.empty())
        {
            return false;
        }
        std::vector<AnnotationItem> clones;
        for (const AnnotationItem& item : m_items)
        {
            if (IsSelected(item.id))
            {
                clones.push_back(item);
            }
        }
        if (clones.empty())
        {
            return false;
        }

        PushUndoSnapshot(m_items);
        int nextStep = NextStepNumber();
        std::vector<std::uint64_t> newIds;
        newIds.reserve(clones.size());
        for (AnnotationItem& clone : clones)
        {
            clone.id = m_nextId++;
            MoveAnnotation(clone, dx, dy);
            if (clone.kind == AnnotationKind::StepBadge)
            {
                clone.stepNumber = std::clamp(nextStep++, 1, 999);
            }
            newIds.push_back(clone.id);
            m_items.push_back(std::move(clone));
        }
        m_selectedIds = std::move(newIds);
        return true;
    }

    void AnnotationDocument::CommitPreviousState(std::vector<AnnotationItem> previousItems)
    {
        PushUndoSnapshot(std::move(previousItems));
    }

    bool AnnotationDocument::Undo() noexcept
    {
        if (m_undoStack.empty())
        {
            return false;
        }
        if (m_redoStack.size() >= kMaxUndoDepth)
        {
            m_redoStack.erase(m_redoStack.begin());
        }
        m_redoStack.push_back(std::move(m_items));
        m_items = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        ReconcileSelection();
        return true;
    }

    bool AnnotationDocument::Redo() noexcept
    {
        if (m_redoStack.empty())
        {
            return false;
        }
        if (m_undoStack.size() >= kMaxUndoDepth)
        {
            m_undoStack.erase(m_undoStack.begin());
        }
        m_undoStack.push_back(std::move(m_items));
        m_items = std::move(m_redoStack.back());
        m_redoStack.pop_back();
        ReconcileSelection();
        return true;
    }

    std::uint64_t AnnotationDocument::HitTestTopmost(POINT point, IDWriteFactory* dwrite, int dpi) const
    {
        for (auto it = m_items.rbegin(); it != m_items.rend(); ++it)
        {
            if (HitTestAnnotationBody(*it, point, dwrite, dpi))
            {
                return it->id;
            }
        }
        return 0;
    }

    std::vector<std::uint64_t> AnnotationDocument::HitTestRect(
        const D2D1_RECT_F& marquee, IDWriteFactory* dwrite, int dpi) const
    {
        const D2D1_RECT_F norm = NormalizeAnnotationRect(marquee);
        std::vector<std::uint64_t> hit;
        if ((norm.right - norm.left) < 1.0f && (norm.bottom - norm.top) < 1.0f)
        {
            return hit;
        }
        for (const AnnotationItem& item : m_items)
        {
            if (RectsIntersect(norm, AnnotationBounds(item, dwrite, dpi)))
            {
                hit.push_back(item.id);
            }
        }
        return hit;
    }

    void AnnotationDocument::PushUndoSnapshot(std::vector<AnnotationItem> snapshot)
    {
        if (m_undoStack.size() >= kMaxUndoDepth)
        {
            m_undoStack.erase(m_undoStack.begin());
        }
        m_undoStack.push_back(std::move(snapshot));
        m_redoStack.clear();
    }

    void AnnotationDocument::ReconcileSelection() noexcept
    {
        std::erase_if(m_selectedIds, [this](std::uint64_t id) {
            return id == 0 || FindItem(id) == nullptr;
        });
    }

    void AnnotationRenderer::DiscardDeviceResources() noexcept
    {
        m_brushContext = nullptr;
        m_scratchBrush = nullptr;

        m_strokeFactory = nullptr;
        m_dashedStroke = nullptr;
        m_dottedStroke = nullptr;
        m_selectionDashStroke = nullptr;
        m_polylineCache.clear();

        m_badgeDwrite = nullptr;
        m_badgeFontSizePx = -1.0f;
        m_badgeFormat = nullptr;
        m_textLayoutCache.clear();
    }

    ID2D1SolidColorBrush* AnnotationRenderer::ScratchBrush(
        ID2D1DeviceContext* context, const D2D1_COLOR_F& color)
    {
        if (m_brushContext != context || !m_scratchBrush)
        {
            m_scratchBrush = nullptr;
            AGSHOT_CHECK_HR(context->CreateSolidColorBrush(color, m_scratchBrush.put()));
            m_brushContext = context;
        }
        else
        {
            m_scratchBrush->SetColor(color);
        }
        return m_scratchBrush.get();
    }

    ID2D1StrokeStyle* AnnotationRenderer::EnsureStrokeStyle(
        ID2D1Factory1* factory, ID2D1StrokeStyle* roundStroke, AnnotationStrokeStyle style)
    {
        if (style == AnnotationStrokeStyle::Solid || factory == nullptr)
        {
            return roundStroke;
        }
        if (m_strokeFactory != factory)
        {
            m_dashedStroke = nullptr;
            m_dottedStroke = nullptr;
            m_selectionDashStroke = nullptr;
            m_polylineCache.clear();
            m_strokeFactory = factory;
        }
        if (style == AnnotationStrokeStyle::Dashed)
        {
            if (!m_dashedStroke)
            {
                const float dashes[2] = { 3.0f, 2.2f };
                const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                    D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
                if (FAILED(factory->CreateStrokeStyle(props, dashes, 2, m_dashedStroke.put())))
                {
                    return roundStroke;
                }
            }
            return m_dashedStroke.get();
        }
        if (!m_dottedStroke)
        {
            const float dashes[2] = { 0.15f, 2.2f };
            const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
            if (FAILED(factory->CreateStrokeStyle(props, dashes, 2, m_dottedStroke.put())))
            {
                return roundStroke;
            }
        }
        return m_dottedStroke.get();
    }

    ID2D1StrokeStyle* AnnotationRenderer::EnsureSelectionDashStroke(ID2D1Factory1* factory)
    {
        if (factory == nullptr)
        {
            return nullptr;
        }
        if (m_strokeFactory != factory)
        {
            m_dashedStroke = nullptr;
            m_dottedStroke = nullptr;
            m_selectionDashStroke = nullptr;
            m_polylineCache.clear();
            m_strokeFactory = factory;
        }
        if (!m_selectionDashStroke)
        {
            const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
            factory->CreateStrokeStyle(props, nullptr, 0, m_selectionDashStroke.put());
        }
        return m_selectionDashStroke.get();
    }

    IDWriteTextFormat* AnnotationRenderer::EnsureBadgeFormat(IDWriteFactory* dwrite, float fontSizePx)
    {
        if (dwrite == nullptr)
        {
            return nullptr;
        }
        if (!m_badgeFormat || m_badgeDwrite != dwrite || std::fabs(m_badgeFontSizePx - fontSizePx) > 0.05f)
        {
            m_badgeFormat = nullptr;
            AGSHOT_CHECK_HR(dwrite->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                fontSizePx, L"en-US", m_badgeFormat.put()));
            m_badgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_badgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_badgeFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            m_badgeDwrite = dwrite;
            m_badgeFontSizePx = fontSizePx;
        }
        return m_badgeFormat.get();
    }

    winrt::com_ptr<ID2D1PathGeometry> AnnotationRenderer::EnsurePolylineGeometry(
        ID2D1Factory1* factory, const AnnotationItem& item)
    {
        if (factory == nullptr || item.points.size() < 2)
        {
            return {};
        }
        if (item.id == 0)
        {
            return BuildPolylineGeometry(factory, item.points);
        }
        if (m_strokeFactory != factory)
        {
            m_dashedStroke = nullptr;
            m_dottedStroke = nullptr;
            m_selectionDashStroke = nullptr;
            m_polylineCache.clear();
            m_strokeFactory = factory;
        }
        const D2D1_POINT_2F p0 = item.points.front();
        const D2D1_POINT_2F pN = item.points.back();
        const auto it = m_polylineCache.find(item.id);
        if (it != m_polylineCache.end()
            && it->second.pointCount == item.points.size()
            && it->second.first.x == p0.x && it->second.first.y == p0.y
            && it->second.last.x == pN.x && it->second.last.y == pN.y
            && it->second.geometry)
        {
            return it->second.geometry;
        }
        if (m_polylineCache.size() >= 256)
        {
            m_polylineCache.clear();
        }
        auto geom = BuildPolylineGeometry(factory, item.points);
        if (geom)
        {
            m_polylineCache[item.id] = CachedPolyline{ item.points.size(), p0, pN, geom };
        }
        return geom;
    }

    winrt::com_ptr<IDWriteTextLayout> AnnotationRenderer::EnsureTextLayout(
        IDWriteFactory* dwrite, const AnnotationItem& item, int dpi)
    {
        if (dwrite == nullptr || item.text.empty())
        {
            return {};
        }
        if (item.id == 0)
        {
            return CreateAnnotationTextLayout(dwrite, item, dpi);
        }
        const auto it = m_textLayoutCache.find(item.id);
        if (it != m_textLayoutCache.end()
            && it->second.factory == dwrite
            && it->second.dpi == dpi
            && it->second.fontSizeDip == item.style.fontSizeDip
            && it->second.maxWidth == item.textMaxWidth
            && it->second.text == item.text
            && it->second.layout)
        {
            return it->second.layout;
        }
        if (m_textLayoutCache.size() >= 128)
        {
            m_textLayoutCache.clear();
        }
        auto layout = CreateAnnotationTextLayout(dwrite, item, dpi);
        if (layout)
        {
            m_textLayoutCache[item.id] = CachedTextLayout{
                dwrite, dpi, item.style.fontSizeDip, item.textMaxWidth, item.text, layout
            };
        }
        return layout;
    }

    winrt::com_ptr<ID2D1Brush> AnnotationRenderer::CreateGpuMosaicBrush(
        ID2D1DeviceContext* context, ID2D1Bitmap* sourceBitmap,
        AnnotationMosaicEffect effect, float strengthPx)
    {
        if (context == nullptr || sourceBitmap == nullptr)
        {
            return {};
        }

        const D2D1_SIZE_F srcSize = sourceBitmap->GetSize();
        if (srcSize.width < 1.0f || srcSize.height < 1.0f)
        {
            return {};
        }
        const D2D1_RECT_F srcRect = D2D1::RectF(0.0f, 0.0f, srcSize.width, srcSize.height);

        if (effect == AnnotationMosaicEffect::GaussianBlur)
        {
            winrt::com_ptr<ID2D1Effect> blur;
            if (FAILED(context->CreateEffect(CLSID_D2D1GaussianBlur, blur.put())))
            {
                return {};
            }
            blur->SetInput(0, sourceBitmap);
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std::max(0.5f, strengthPx));
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);

            winrt::com_ptr<ID2D1Image> blurred;
            blur->GetOutput(blurred.put());
            const D2D1_IMAGE_BRUSH_PROPERTIES imgProps = D2D1::ImageBrushProperties(
                srcRect, D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                D2D1_INTERPOLATION_MODE_LINEAR);
            winrt::com_ptr<ID2D1ImageBrush> brush;
            if (FAILED(context->CreateImageBrush(blurred.get(), imgProps, brush.put())))
            {
                return {};
            }
            return brush;
        }

        const float block = std::max(2.0f, strengthPx);
        winrt::com_ptr<ID2D1Effect> scaleDown;
        winrt::com_ptr<ID2D1Effect> scaleUp;
        if (FAILED(context->CreateEffect(CLSID_D2D1Scale, scaleDown.put()))
            || FAILED(context->CreateEffect(CLSID_D2D1Scale, scaleUp.put())))
        {
            return {};
        }

        scaleDown->SetInput(0, sourceBitmap);
        scaleDown->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(1.0f / block, 1.0f / block));
        scaleDown->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR);
        scaleDown->SetValue(D2D1_SCALE_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);

        scaleUp->SetInputEffect(0, scaleDown.get());
        scaleUp->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(block, block));
        scaleUp->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        scaleUp->SetValue(D2D1_SCALE_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);

        winrt::com_ptr<ID2D1Image> pixelated;
        scaleUp->GetOutput(pixelated.put());
        const D2D1_IMAGE_BRUSH_PROPERTIES imgProps = D2D1::ImageBrushProperties(
            srcRect, D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        winrt::com_ptr<ID2D1ImageBrush> brush;
        if (FAILED(context->CreateImageBrush(pixelated.get(), imgProps, brush.put())))
        {
            return {};
        }
        return brush;
    }

    void AnnotationRenderer::DrawAnnotations(
        ID2D1Factory1* factory, ID2D1DeviceContext* context,
        ID2D1StrokeStyle* roundStroke, IDWriteFactory* dwrite,
        ID2D1Bitmap* sourceBitmap, int dpi,
        const std::vector<AnnotationItem>& items,
        const AnnotationItem* draftItem,
        std::span<const std::uint64_t> selectedIds,
        bool showSelectionChrome,
        std::uint64_t hiddenId)
    {
        if (context == nullptr)
        {
            return;
        }

        const auto drawItem = [&](const AnnotationItem& item)
        {
            const float strokePx = ScaleByDpi(item.style.strokeWidthDip, dpi);

            if (item.kind == AnnotationKind::Mosaic)
            {
                const float strengthPx = (item.style.mosaicEffect == AnnotationMosaicEffect::GaussianBlur)
                    ? std::max(0.5f, ScaleByDpi(item.style.mosaicBlurRadiusDip, dpi))
                    : std::max(2.0f, ScaleByDpi(static_cast<float>(item.style.mosaicBlockDip), dpi));
                winrt::com_ptr<ID2D1Brush> mosaicBrush =
                    CreateGpuMosaicBrush(context, sourceBitmap, item.style.mosaicEffect, strengthPx);
                if (!mosaicBrush)
                {
                    return;
                }
                if (item.style.mosaicKind == AnnotationMosaicKind::Rectangle)
                {
                    const D2D1_RECT_F r = NormalizeAnnotationRect(item.rect);
                    if (r.right - r.left >= 1.0f && r.bottom - r.top >= 1.0f)
                    {
                        context->FillRectangle(r, mosaicBrush.get());
                    }
                }
                else if (auto path = EnsurePolylineGeometry(factory, item))
                {
                    context->DrawGeometry(
                        path.get(), mosaicBrush.get(),
                        ScaleByDpi(item.style.mosaicBrushDip, dpi), roundStroke);
                }
                else if (item.points.size() == 1)
                {
                    const float radius = ScaleByDpi(item.style.mosaicBrushDip, dpi) * 0.5f;
                    context->FillEllipse(D2D1::Ellipse(item.points.front(), radius, radius), mosaicBrush.get());
                }
                return;
            }

            D2D1_COLOR_F primaryColor = item.style.color;
            if (item.kind == AnnotationKind::Brush && item.style.brushHighlighter)
            {
                primaryColor.a = std::clamp(primaryColor.a * 0.42f, 0.0f, 1.0f);
            }

            switch (item.kind)
            {
            case AnnotationKind::Shape:
            {
                const D2D1_RECT_F r = NormalizeAnnotationRect(item.rect);
                if (r.right - r.left < 1.0f && r.bottom - r.top < 1.0f)
                {
                    break;
                }
                const bool hasFill = HasVisibleColor(item.style.fillColor);
                const bool hasStroke = HasVisibleColor(item.style.color);
                winrt::com_ptr<ID2D1Geometry> geom = BuildShapeGeometry(factory, item, r, dpi);
                if (hasFill)
                {
                    ID2D1SolidColorBrush* fillBrush = ScratchBrush(context, item.style.fillColor);
                    if (geom)
                    {
                        if (item.style.fillPattern == AnnotationFillPattern::Solid)
                        {
                            context->FillGeometry(geom.get(), fillBrush);
                        }
                        else
                        {
                            DrawHatchFill(context, geom.get(), r, fillBrush,
                                          item.style.fillPattern, dpi);
                        }
                    }
                    else
                    {
                        context->FillRectangle(r, fillBrush);
                    }
                }
                if (hasStroke)
                {
                    ID2D1StrokeStyle* resolvedStroke =
                        EnsureStrokeStyle(factory, roundStroke, item.style.strokeStyle);
                    ID2D1SolidColorBrush* strokeBrush = ScratchBrush(context, primaryColor);
                    if (geom)
                    {
                        context->DrawGeometry(geom.get(), strokeBrush, strokePx, resolvedStroke);
                    }
                    else
                    {
                        context->DrawRectangle(r, strokeBrush, strokePx, resolvedStroke);
                    }
                }
                break;
            }
            case AnnotationKind::Line:
            {
                ID2D1StrokeStyle* resolvedStroke =
                    EnsureStrokeStyle(factory, roundStroke, item.style.strokeStyle);
                ID2D1SolidColorBrush* lineBrush = ScratchBrush(context, primaryColor);
                if (item.style.lineKind == AnnotationLineKind::Arrow)
                {
                    DrawArrow(factory, context, resolvedStroke, lineBrush,
                              item.start, item.end, strokePx, dpi);
                }
                else
                {
                    context->DrawLine(item.start, item.end, lineBrush, strokePx, resolvedStroke);
                }
                break;
            }
            case AnnotationKind::Brush:
                if (auto path = EnsurePolylineGeometry(factory, item))
                {
                    context->DrawGeometry(path.get(), ScratchBrush(context, primaryColor), strokePx, roundStroke);
                }
                else if (item.points.size() == 1)
                {
                    const float radius = strokePx * 0.5f;
                    context->FillEllipse(
                        D2D1::Ellipse(item.points.front(), radius, radius),
                        ScratchBrush(context, primaryColor));
                }
                break;
            case AnnotationKind::Text:
                if (!item.text.empty())
                {
                    if (auto layout = EnsureTextLayout(dwrite, item, dpi))
                    {
                        context->DrawTextLayout(
                            item.origin, layout.get(), ScratchBrush(context, primaryColor),
                            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                    }
                }
                break;
            case AnnotationKind::StepBadge:
            {
                const float r = ScaleByDpi(item.style.badgeRadiusDip, dpi);
                const D2D1_RECT_F box = D2D1::RectF(
                    item.origin.x - r, item.origin.y - r,
                    item.origin.x + r, item.origin.y + r);
                const bool isSquare =
                    item.style.badgeShape == AnnotationBadgeShape::OutlineSquare
                    || item.style.badgeShape == AnnotationBadgeShape::FilledSquare;
                const bool isFilled =
                    item.style.badgeShape == AnnotationBadgeShape::FilledCircle
                    || item.style.badgeShape == AnnotationBadgeShape::FilledSquare;

                const D2D1_COLOR_F contrastColor = IsLightColor(item.style.color, 0.65f)
                    ? D2D1::ColorF(0.08f, 0.09f, 0.11f, item.style.color.a)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, item.style.color.a);
                const D2D1_COLOR_F textColor = isFilled ? contrastColor : primaryColor;
                const float borderW = ScaleByDpi(2.0f, dpi);

                if (isSquare)
                {
                    const float cr = r * 0.30f;
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(box, cr, cr);
                    if (isFilled)
                    {
                        context->FillRoundedRectangle(rr, ScratchBrush(context, primaryColor));
                    }
                    else
                    {
                        context->DrawRoundedRectangle(rr, ScratchBrush(context, primaryColor), borderW);
                    }
                }
                else
                {
                    const D2D1_ELLIPSE circle = D2D1::Ellipse(item.origin, r, r);
                    if (isFilled)
                    {
                        context->FillEllipse(circle, ScratchBrush(context, primaryColor));
                    }
                    else
                    {
                        context->DrawEllipse(circle, ScratchBrush(context, primaryColor), borderW);
                    }
                }

                const std::wstring label = FormatStepBadgeLabel(item.stepNumber, item.style.badgeNumberStyle);
                float fontScale = 1.15f;
                if (label.size() == 2) fontScale = 0.88f;
                else if (label.size() == 3) fontScale = 0.70f;
                else if (label.size() >= 4) fontScale = 0.54f;

                if (IDWriteTextFormat* numFormat = EnsureBadgeFormat(dwrite, std::max(6.0f, r * fontScale)))
                {
                    context->DrawText(
                        label.c_str(), static_cast<UINT32>(label.size()),
                        numFormat, box, ScratchBrush(context, textColor));
                }
                break;
            }
            case AnnotationKind::Mosaic:
                break;
            }
        };

        for (const AnnotationItem& item : items)
        {
            if (hiddenId != 0 && item.id == hiddenId)
            {
                continue;
            }
            drawItem(item);
        }
        if (draftItem != nullptr)
        {
            drawItem(*draftItem);
        }
        if (showSelectionChrome && !selectedIds.empty())
        {
            DrawSelectionChrome(factory, context, dwrite, dpi, items, selectedIds, hiddenId);
        }
    }

    void AnnotationRenderer::DrawSelectionChrome(
        ID2D1Factory1* factory, ID2D1DeviceContext* context,
        IDWriteFactory* dwrite, int dpi,
        const std::vector<AnnotationItem>& items,
        std::span<const std::uint64_t> selectedIds,
        std::uint64_t hiddenId)
    {
        if (context == nullptr || selectedIds.empty())
        {
            return;
        }
        std::vector<const AnnotationItem*> selectedItems;
        selectedItems.reserve(selectedIds.size());
        for (const AnnotationItem& item : items)
        {
            if (item.id == 0 || (hiddenId != 0 && item.id == hiddenId))
            {
                continue;
            }
            if (std::find(selectedIds.begin(), selectedIds.end(), item.id) != selectedIds.end())
            {
                selectedItems.push_back(&item);
            }
        }
        if (selectedItems.empty())
        {
            return;
        }
        ID2D1SolidColorBrush* scratch = ScratchBrush(context, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        if (selectedItems.size() == 1)
        {
            DrawSelectionChromeSingle(context, scratch, *selectedItems[0], dwrite, dpi);
        }
        else
        {
            DrawSelectionChromeMulti(
                context, scratch, EnsureSelectionDashStroke(factory),
                selectedItems, dwrite, dpi);
        }
    }

    void AnnotationRenderer::ExportSelectionToClipboard(
        HWND owner, const RenderDevice& device, const ScreenImage& sourceImage,
        const RECT& bounds, const RECT& selection,
        const std::vector<AnnotationItem>& items) const
    {
        if (items.empty())
        {
            const RECT region{
                selection.left + bounds.left,
                selection.top + bounds.top,
                selection.right + bounds.left,
                selection.bottom + bounds.top
            };
            sourceImage.CopyToClipboard(owner, region);
            return;
        }

        const RECT clientBounds{
            0, 0,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top
        };
        RECT clipped{};
        if (sourceImage.empty()
            || !IntersectRect(&clipped, &selection, &clientBounds)
            || IsRectEmpty(&clipped)
            || device.d2dDevice() == nullptr
            || device.d2dFactory() == nullptr)
        {
            throw Error{ Failure{ L"AnnotationRenderer::ExportSelectionToClipboard",
                                  L"the selection or render device is unavailable" } };
        }

        const UINT width = static_cast<UINT>(clipped.right - clipped.left);
        const UINT height = static_cast<UINT>(clipped.bottom - clipped.top);
        const D2D1_SIZE_U size = D2D1::SizeU(width, height);

        winrt::com_ptr<ID2D1DeviceContext> target;
        AGSHOT_CHECK_HR(device.d2dDevice()->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, target.put()));
        target->SetDpi(96.0f, 96.0f);

        const D2D1_BITMAP_PROPERTIES1 targetProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        winrt::com_ptr<ID2D1Bitmap1> surface;
        AGSHOT_CHECK_HR(target->CreateBitmap(size, nullptr, 0, targetProperties, surface.put()));
        target->SetTarget(surface.get());

        const D2D1_BITMAP_PROPERTIES1 readProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        winrt::com_ptr<ID2D1Bitmap1> readable;
        AGSHOT_CHECK_HR(target->CreateBitmap(size, nullptr, 0, readProperties, readable.put()));

        const D2D1_BITMAP_PROPERTIES srcProps = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);
        winrt::com_ptr<ID2D1Bitmap> background;
        AGSHOT_CHECK_HR(target->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(sourceImage.width()), static_cast<UINT32>(sourceImage.height())),
            sourceImage.pixels(),
            static_cast<UINT32>(sourceImage.stride()),
            srcProps,
            background.put()));

        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        target->SetTransform(D2D1::Matrix3x2F::Translation(
            -static_cast<float>(clipped.left),
            -static_cast<float>(clipped.top)));
        target->DrawBitmap(
            background.get(),
            D2D1::RectF(0.0f, 0.0f,
                        static_cast<float>(sourceImage.width()),
                        static_cast<float>(sourceImage.height())),
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);

        AnnotationRenderer offscreenRenderer;
        offscreenRenderer.DrawAnnotations(
            device.d2dFactory(), target.get(), device.roundStroke(), device.dwriteFactory(),
            background.get(), device.dpi(), items, nullptr, {}, false, 0);

        target->SetTransform(D2D1::Matrix3x2F::Identity());
        AGSHOT_CHECK_HR(target->EndDraw());

        AGSHOT_CHECK_HR(readable->CopyFromBitmap(nullptr, surface.get(), nullptr));
        D2D1_MAPPED_RECT mapped{};
        AGSHOT_CHECK_HR(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        auto unmap = wil::scope_exit([&] { readable->Unmap(); });

        CopyBgraToClipboard(
            owner, mapped.bits, static_cast<int>(width), static_cast<int>(height), mapped.pitch);
    }
}
