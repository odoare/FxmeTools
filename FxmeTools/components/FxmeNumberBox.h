/*
  ==============================================================================

    FxmeNumberBox.h

    A compact numeric control for panels with too many parameters for knobs
    to stay legible: a bordered box showing the parameter's name and value as
    text, with a thin fill strip along the bottom edge for an at-a-glance
    sense of where the value sits in its range. Dragging up/down adjusts it
    (RotaryVerticalDrag's relative, delta-based mapping — not tied to
    absolute position the way a linear slider's track would be, which would
    make a box this small unusably twitchy); right-click opens the same
    inline text-entry FxmeSlider already has. Both name and value get as
    much of the box's own height as there is, so they stay readable down to
    sizes a knob's label would already have shrunk past.

    A plain FxmeSlider subclass: everything about attachments, ranges,
    setCentralValue()/setShowLabel() and right-click entry is inherited
    unchanged. Only paint() is new. Reads the same Slider ColourIds
    Theme::styleKnob (or equivalent) already sets on a knob —
    rotarySliderFillColourId (body), rotarySliderOutlineColourId (border),
    trackColourId (fill strip), thumbColourId (value text) — so swapping a
    knob for a number box is a type change, not a re-theming.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "FxmeSlider.h"
#include "../lookandfeels/FxmeLookAndFeel.h"

namespace fxme
{

class FxmeNumberBox : public FxmeSlider
{
public:
    FxmeNumberBox()
    {
        setSliderStyle (juce::Slider::RotaryVerticalDrag);
    }

    FxmeNumberBox (juce::AudioProcessorValueTreeState& apvts,
                   const juce::String& paramID,
                   const juce::String& labelText,
                   const juce::Colour& colour)
        : FxmeSlider (apvts, paramID, labelText, colour)
    {
        setSliderStyle (juce::Slider::RotaryVerticalDrag);
    }

    void paint (juce::Graphics& g) override
    {
        const bool enabled = isEnabled();
        const bool hovered = enabled && isMouseOverOrDragging();

        auto bounds = getLocalBounds().toFloat();
        const float w      = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.1f;
        const float corner = w * 0.8f;
        bounds = bounds.reduced (w * 0.5f);

        g.setColour (FxmeLookAndFeel::forState (
            findColour (juce::Slider::rotarySliderFillColourId), enabled, 0.9f));
        g.fillRoundedRectangle (bounds, corner);

        const auto outline = FxmeLookAndFeel::forHover (FxmeLookAndFeel::forState (
            findColour (juce::Slider::rotarySliderOutlineColourId), enabled), hovered);
        g.setColour (outline);
        g.drawRoundedRectangle (bounds, corner, juce::jmax (1.0f, w * 0.4f));

        // Fill strip along the bottom edge, growing from originProportion() -
        // the minimum normally, or the "centralValue"/"drawFromCentre" origin
        // for a bipolar control, same rule as every other fxme control.
        const float stripH = juce::jmax (2.0f, bounds.getHeight() * 0.08f);
        auto stripArea = bounds.removeFromBottom (stripH).reduced (w * 0.5f, 0.0f);

        g.setColour (FxmeLookAndFeel::forState (
            findColour (juce::Slider::trackColourId), enabled).withMultipliedAlpha (0.2f));
        g.drawRoundedRectangle (stripArea, stripH * 0.4f, 1.0f);

        const float levelProportion = (float) valueToProportionOfLength (getValue());
        const float origin = FxmeLookAndFeel::originProportion (*this);
        const float lo = juce::jmin (origin, levelProportion);
        const float hi = juce::jmax (origin, levelProportion);
        if (hi > lo)
        {
            auto filled = stripArea;
            filled.removeFromLeft  (stripArea.getWidth() * lo);
            filled.removeFromRight (stripArea.getWidth() * (1.0f - hi));
            g.setColour (FxmeLookAndFeel::forHover (FxmeLookAndFeel::forState (
                findColour (juce::Slider::trackColourId), enabled), hovered));
            g.fillRoundedRectangle (filled, stripH * 0.4f);
        }

        bounds.removeFromBottom (2.0f);   // small gap above the strip

        const float nameH = juce::jlimit (9.0f, 15.0f, bounds.getHeight() * 0.18f);
        auto nameArea = bounds.removeFromTop (nameH * 1.3f);
        g.setColour (findColour (juce::Slider::textBoxTextColourId)
                          .withMultipliedAlpha (FxmeLookAndFeel::textAlpha (0.6f, enabled)));
        g.setFont (juce::Font (juce::FontOptions (nameH)));
        g.drawFittedText (getName().toUpperCase(), nameArea.toNearestInt(),
                          juce::Justification::centred, 1);

        const float valueH = juce::jlimit (12.0f, 30.0f, bounds.getHeight() * 0.42f);
        g.setColour (FxmeLookAndFeel::forHover (
            findColour (juce::Slider::thumbColourId), hovered)
                          .withMultipliedAlpha (FxmeLookAndFeel::textAlpha (1.0f, enabled)));
        g.setFont (juce::Font (juce::FontOptions (valueH, juce::Font::bold)));
        g.drawFittedText (getTextFromValue (getValue()), bounds.toNearestInt(),
                          juce::Justification::centred, 1);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxmeNumberBox)
};

} // namespace fxme
