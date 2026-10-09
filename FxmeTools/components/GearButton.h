/*
  ------------------------------------------------------------------------------
    GearButton.h

    The top bar's "global settings" button: a toothed wheel with a hole,
    filled in an accent colour, brighter on hover, darker while pressed.
    Dede and MechanOdd each carry a local copy of this drawing; new plugins
    use this one.

        fxme::GearButton gear;
        gear.setAccent (theme::accent);
        gear.onClick = [this] { showGlobalPanel(); };

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

class GearButton : public juce::Button
{
public:
    GearButton() : juce::Button ("global") { setMouseClickGrabsKeyboardFocus (false); }

    void setAccent (juce::Colour c) { accent = c; repaint(); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        const float d = juce::jmin (b.getWidth(), b.getHeight());
        const auto c = b.getCentre();

        const float outer = d * 0.5f;          // tooth tips
        const float root  = outer * 0.74f;     // between the teeth
        const float hole  = outer * 0.34f;
        constexpr int teeth = 8;

        juce::Path gear;
        const float pitch = juce::MathConstants<float>::twoPi / (float) teeth;
        for (int i = 0; i < teeth; ++i)
        {
            const float a = (float) i * pitch;
            const float angles[] { a - 0.30f * pitch, a - 0.18f * pitch, a + 0.18f * pitch, a + 0.30f * pitch };
            const float radii[]  { root, outer, outer, root };

            for (int j = 0; j < 4; ++j)
            {
                const auto pt = c.getPointOnCircumference (radii[j], angles[j]);
                if (i == 0 && j == 0)
                    gear.startNewSubPath (pt);
                else
                    gear.lineTo (pt);
            }
            gear.addCentredArc (c.x, c.y, root, root, 0.0f, a + 0.30f * pitch, a + 0.70f * pitch);
        }
        gear.closeSubPath();
        gear.addEllipse (juce::Rectangle<float> (2.0f * hole, 2.0f * hole).withCentre (c));
        gear.setUsingNonZeroWinding (false);

        auto colour = down ? accent.darker (0.3f) : over ? accent.brighter (0.3f) : accent;
        if (! isEnabled())
            colour = colour.withMultipliedSaturation (0.3f).withMultipliedAlpha (0.5f);
        g.setColour (colour);
        g.fillPath (gear);
    }

private:
    juce::Colour accent { 0xffff8a1f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GearButton)
};

} // namespace fxme
