/*
  ------------------------------------------------------------------------------
    AccentToggle.h

    A latching on/off button in the FX-Mechanics style: a rounded body that
    lights up in its accent colour when engaged, with bold centred text that
    stays legible down to ~18 px squares.

    Custom-painted on purpose — the stock LookAndFeel's text indents leave no
    room for a letter in a small square button, and its toggle rendering does
    not read as "lit" at that size. Use it for mute / solo / bypass letters,
    view toggles, and any other latching button that should share one look
    across a plugin.

    Usage (message thread only, like any juce::Component):

        fxme::AccentToggle mute;
        mute.setButtonText ("M");
        mute.setAccent (juce::Colour (0xffd9b13a), myTheme::text);
        addAndMakeVisible (mute);
        // it already latches (setClickingTogglesState(true) in the ctor):
        mute.onClick = [this] { ... mute.getToggleState() ... };

    Or drive it from an APVTS parameter with a plain ButtonAttachment. The
    body/text colours are the usual TextButton colour ids, so anything
    setAccent() does not cover can be set with setColour() afterwards.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace fxme
{

struct AccentToggle : juce::TextButton
{
    AccentToggle()
    {
        setClickingTogglesState (true);
        setMouseClickGrabsKeyboardFocus (false);
    }

    /** Dark body when off, `accent` when on, with matching text colours.
        Defaults match the FX-Mechanics house palette. */
    void setAccent (juce::Colour accent,
                    juce::Colour offTextColour = juce::Colour (0xffd8d8e0),
                    juce::Colour bodyColour    = juce::Colour (0xff2b2b2b))
    {
        setColour (juce::TextButton::buttonColourId,   bodyColour);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId,  offTextColour);
        setColour (juce::TextButton::textColourOnId,   juce::Colours::black);
    }

    void setCornerSize (float newCornerSize)   { cornerSize = newCornerSize; repaint(); }
    void setMaxFontHeight (float newMaxHeight) { maxFontHeight = newMaxHeight; repaint(); }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const bool on = getToggleState();
        auto bg = findColour (on ? buttonOnColourId : buttonColourId);
        if (! isEnabled())
            bg = bg.withAlpha (0.5f);

        g.setColour (highlighted && isEnabled() ? bg.brighter (0.2f) : bg);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), cornerSize);

        g.setColour (findColour (on ? textColourOnId : textColourOffId)
                         .withMultipliedAlpha (isEnabled() ? 1.0f : 0.6f));
        g.setFont (juce::Font (juce::FontOptions (
            juce::jmin (maxFontHeight, (float) getHeight() * 0.62f), juce::Font::bold)));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);
    }

private:
    float cornerSize = 4.0f;
    float maxFontHeight = 13.0f;
};

} // namespace fxme
