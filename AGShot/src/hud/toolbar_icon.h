#pragma once

// The toolbar's text-recognition mark, traced from its reference art.
//
// The art is a 48-unit icon in which the mark's ink is not centred on the
// canvas and covers only the middle 30 units. Drawing it at its own scale left
// it visibly lighter than the edit-selection mark beside it, so the mark is
// scaled to a chosen ink width and re-centred in its button.
//
// The geometry lives apart from the drawing so that the same coordinates can be
// rendered and measured without a window. Every number below was corrected by
// comparing rendered pixels against the reference, not by reading them back.

#include <windows.h>

#include <d2d1_1.h>

#include <array>
#include <cmath>
#include <cstddef>

namespace agshot
{
    // The art's corners are cut back rather than left square: the two arms meet
    // through a fillet of this radius, in reference units.
    inline constexpr float kTextRecognitionCornerFillet = 1.3f;

    struct TextRecognitionIcon
    {
        // A bracket is an open polyline: the outer tip, a short fillet, and the
        // inner tip. The fillet is sampled into segments because it is barely a
        // pixel across at toolbar size.
        struct Bracket
        {
            static constexpr std::size_t kMaxPoints = 16;
            std::array<D2D1_POINT_2F, kMaxPoints> points{};
            std::size_t count{};
        };

        std::array<Bracket, 4> brackets{};
        // Three text bars, left-aligned, in drawing order.
        std::array<D2D1_POINT_2F, 3> barFrom{};
        std::array<D2D1_POINT_2F, 3> barTo{};
        // Stroke width in DIPs.
        float stroke{};
    };

    // Lays the mark out inside "box", sized so that its ink measures "inkDips"
    // across and drawn with a stroke "strokeDips" wide. The reference
    // coordinates below were measured from the art.
    //
    // The two are separate because the art is drawn with a lighter stroke for
    // its size than the toolbar's other icons use: traced as-is it reads as a
    // thinner mark than the button beside it, so the caller supplies the weight
    // the other icons are drawn at.
    inline TextRecognitionIcon LayoutTextRecognitionIcon(
        const RECT& box, float inkDips, float strokeDips,
        float cornerFillet = kTextRecognitionCornerFillet) noexcept
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kCentre = 25.0f;      // the art's ink centre, not the canvas centre
        constexpr float kArm = 11.315f;       // outer arm centrelines
        constexpr float kMirrorArm = 50.0f - kArm;
        constexpr float kTip = 19.0f;         // where an arm stops
        constexpr float kOppositeTip = 50.0f - kTip;
        // The scale is chosen so that the ink - the arms' centrelines plus half
        // a stroke on each side - still measures "inkDips", whatever stroke is
        // asked for. Without that, matching the neighbours' weight would also
        // make the mark bigger than them.
        constexpr float kCentreSpan = kMirrorArm - kArm;
        const float scale = (inkDips - strokeDips) / kCentreSpan;
        const float centreX = static_cast<float>(box.left + box.right) * 0.5f;
        const float centreY = static_cast<float>(box.top + box.bottom) * 0.5f;
        const auto toX = [centreX, scale](float value)
        {
            return centreX + (value - kCentre) * scale;
        };
        const auto toY = [centreY, scale](float value)
        {
            return centreY + (value - kCentre) * scale;
        };
        const auto point = [&toX, &toY](float x, float y)
        {
            return D2D1::Point2F(toX(x), toY(y));
        };

        TextRecognitionIcon icon{};
        icon.stroke = strokeDips;

        constexpr int kArcSteps = 6;
        for (int corner = 0; corner < 4; ++corner)
        {
            // Which way this corner faces: the second and fourth are on the
            // right, the third and fourth are along the bottom.
            const float sx = (corner == 1 || corner == 3) ? -1.0f : 1.0f;
            const float sy = (corner == 2 || corner == 3) ? -1.0f : 1.0f;
            const float arm = (sx > 0.0f) ? kArm : kMirrorArm;
            const float across = (sy > 0.0f) ? kArm : kMirrorArm;
            const float tip = (sx > 0.0f) ? kTip : kOppositeTip;
            const float acrossTip = (sy > 0.0f) ? kTip : kOppositeTip;

            // The horizontal arm stops where the fillet starts, and the vertical
            // arm resumes where it ends. Both are measured from the fillet's own
            // centre, so the arc is a plain quarter turn about it.
            const float arcX = arm + sx * cornerFillet;
            const float arcY = across + sy * cornerFillet;
            const float startAngle = (sy > 0.0f) ? -kPi * 0.5f : kPi * 0.5f;
            const float endAngle = (sx > 0.0f) ? ((sy > 0.0f) ? -kPi : kPi) : 0.0f;

            TextRecognitionIcon::Bracket& bracket = icon.brackets[corner];
            std::size_t n = 0;
            const auto push = [&bracket, &n](D2D1_POINT_2F value)
            {
                if (n < TextRecognitionIcon::Bracket::kMaxPoints)
                {
                    bracket.points[n++] = value;
                }
            };

            push(point(tip, across));
            for (int step = 0; step <= kArcSteps; ++step)
            {
                const float t = static_cast<float>(step) / kArcSteps;
                const float angle = startAngle + (endAngle - startAngle) * t;
                push(point(arcX + cornerFillet * std::cos(angle),
                           arcY + cornerFillet * std::sin(angle)));
            }
            push(point(arm, acrossTip));
            bracket.count = n;
        }

        // Measuring the art at a sub-pixel crossing shows the bars start on
        // 15.63 and that the outer two stop short of their earlier estimate.
        constexpr float kBars[3][3] =
        {
            { 19.62f, 15.63f, 34.40f },
            { 25.00f, 15.63f, 30.71f },
            { 30.38f, 15.63f, 32.46f },
        };
        for (int i = 0; i < 3; ++i)
        {
            icon.barFrom[i] = point(kBars[i][1], kBars[i][0]);
            icon.barTo[i] = point(kBars[i][2], kBars[i][0]);
        }
        return icon;
    }
}
