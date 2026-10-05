/*
  ------------------------------------------------------------------------------
    AbCompareBar.h

    The widget of an fxme::AbComparison: "A" and "B" latching buttons (the
    lit one is the side in use) and a copy button ("A>B" or "B>A", copying
    the side in use onto the other). Drawn by the look-and-feel's buttons,
    tinted with setAccentColour().

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include "../presets/AbComparison.h"

namespace fxme
{

class AbCompareBar : public juce::Component,
                     private juce::ChangeListener
{
public:
    explicit AbCompareBar (AbComparison& comparisonToUse)
        : comparison (comparisonToUse)
    {
        for (auto* b : { &aButton, &bButton, &copyButton })
        {
            b->setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (*b);
        }

        aButton.onClick = [this] { comparison.select (0); refresh(); };
        bButton.onClick = [this] { comparison.select (1); refresh(); };
        copyButton.onClick = [this] { comparison.copyToOther(); };

        setAccentColour (juce::Colour (0xff35d6d0));
        comparison.addChangeListener (this);
        refresh();
    }

    ~AbCompareBar() override { comparison.removeChangeListener (this); }

    void setAccentColour (juce::Colour accent, juce::Colour body = juce::Colour (0xff2b2b2b),
                          juce::Colour text = juce::Colour (0xffd8d8e0))
    {
        for (auto* b : { &aButton, &bButton, &copyButton })
        {
            b->setColour (juce::TextButton::buttonColourId, body);
            b->setColour (juce::TextButton::buttonOnColourId, accent);
            b->setColour (juce::TextButton::textColourOffId, text);
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        }
    }

    /** Tooltips of the three buttons. */
    void setTooltips (const juce::String& a, const juce::String& b, const juce::String& copy)
    {
        aButton.setTooltip (a);
        bButton.setTooltip (b);
        copyButton.setTooltip (copy);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int w = (r.getWidth() - 8) / 4;
        aButton.setBounds (r.removeFromLeft (w));
        r.removeFromLeft (2);
        bButton.setBounds (r.removeFromLeft (w));
        r.removeFromLeft (6);
        copyButton.setBounds (r);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void refresh()
    {
        const int s = comparison.getActiveSlot();
        aButton.setToggleState (s == 0, juce::dontSendNotification);
        bButton.setToggleState (s == 1, juce::dontSendNotification);
        copyButton.setButtonText (s == 0 ? "A>B" : "B>A");
    }

    AbComparison& comparison;
    juce::TextButton aButton { "A" }, bButton { "B" }, copyButton { "A>B" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AbCompareBar)
};

} // namespace fxme
