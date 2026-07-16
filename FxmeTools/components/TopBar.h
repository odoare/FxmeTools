/*
  ------------------------------------------------------------------------------
    TopBar.h

    The FX-Mechanics plugin header bar: dark background with a subtle vertical
    sheen, the company logo on the left, the plugin name in large type, a short
    description next to it, an accent hairline along the bottom and
    "v<version>  -  FX-Mechanics" on the right.

    Shared by all FX-Mechanics plugins for a consistent brand identity
    (promoted from the local copy first written for Spread).

    Usage (message thread only, like any juce::Component):

        fxme::TopBar topBar { "Mango", "modular sound glitcher",
                              JucePlugin_VersionString,
                              juce::ImageCache::getFromMemory (
                                  BinaryData::logo686_png,
                                  BinaryData::logo686_pngSize) };
        ...
        topBar.setBounds (bounds.removeFromTop (54));

    The logo image is passed in by the caller because binary assets stay
    per-plugin (each plugin embeds its own BinaryData). Colours default to the
    house palette and can be overridden with the setters.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace fxme
{

class TopBar : public juce::Component
{
public:
    TopBar (juce::String pluginName, juce::String description,
            juce::String versionString, juce::Image logoImage)
        : name (std::move (pluginName)), blurb (std::move (description)),
          version (std::move (versionString)), logo (std::move (logoImage))
    {
    }

    void setAccentColour (juce::Colour c)     { accent = c; repaint(); }
    void setBackgroundColour (juce::Colour c) { background = c; repaint(); }

    /** Parks externally-owned controls (e.g. the compact PresetBarComponent
        and its toggle button) in the free space to the left of the version
        string; the blurb is shortened so it never runs under them. Pass
        nullptr for a slot to leave it out. */
    void setRightControls (juce::Component* bar, int barWidth,
                           juce::Component* button, int buttonWidth)
    {
        rightBar    = bar;    rightBarW    = barWidth;
        rightButton = button; rightButtonW = buttonWidth;

        if (rightBar != nullptr)    addAndMakeVisible (*rightBar);
        if (rightButton != nullptr) addAndMakeVisible (*rightButton);

        resized();
    }

    void resized() override
    {
        if (rightBar == nullptr && rightButton == nullptr)
            return;

        auto area = getLocalBounds().reduced (12, 6);
        area.removeFromRight (kVersionWidth + 8);   // leave the version alone

        auto strip = area.removeFromRight (reservedRight() - 8);
        const int stripH = juce::jmin (28, strip.getHeight());
        strip = strip.withSizeKeepingCentre (strip.getWidth(), stripH);

        if (rightButton != nullptr)
            rightButton->setBounds (strip.removeFromRight (rightButtonW));
        strip.removeFromRight (6);
        if (rightBar != nullptr)
            rightBar->setBounds (strip);
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();

        // Dark header with a subtle vertical sheen and an accent hairline below.
        juce::ColourGradient grad (background.brighter (0.12f), b.getTopLeft(),
                                   background, b.getBottomLeft(), false);
        g.setGradientFill (grad);
        g.fillRect (b);
        g.setColour (accent.withAlpha (0.55f));
        g.fillRect (b.removeFromBottom (1.5f));

        auto area = getLocalBounds().reduced (12, 6);

        // Logo, left, square-ish, preserving aspect ratio.
        if (logo.isValid())
        {
            const int side = area.getHeight();
            auto logoArea = area.removeFromLeft (side);
            g.drawImage (logo, logoArea.toFloat(),
                         juce::RectanglePlacement::centred
                       | juce::RectanglePlacement::onlyReduceInSize);
            area.removeFromLeft (12);
        }

        // Plugin name, large.
        g.setColour (text);
        g.setFont (juce::Font (juce::FontOptions ((float) getHeight() * 0.52f,
                                                  juce::Font::bold)));
        const int nameWidth = juce::GlyphArrangement::getStringWidthInt (
                                  g.getCurrentFont(), name) + 8;
        g.drawText (name, area.removeFromLeft (nameWidth),
                    juce::Justification::centredLeft);

        // Version, right.
        g.setColour (dimText);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("v" + version + "  -  FX-Mechanics",
                    area.removeFromRight (kVersionWidth), juce::Justification::centredRight);

        // Right controls (if any) sit between the version and the blurb;
        // keep the blurb clear of them.
        area.removeFromRight (reservedRight());

        // Short description next to the name.
        area.removeFromLeft (10);
        g.setColour (dimText);
        g.setFont (juce::Font (juce::FontOptions ((float) getHeight() * 0.26f)));
        g.drawText (blurb, area, juce::Justification::centredLeft);
    }

private:
    int reservedRight() const
    {
        if (rightBar == nullptr && rightButton == nullptr)
            return 0;
        return rightBarW + 6 + rightButtonW + 8;   // strip + gap to the version
    }

    static constexpr int kVersionWidth = 150;

    juce::String name, blurb, version;
    juce::Image logo;

    juce::Component* rightBar = nullptr;
    juce::Component* rightButton = nullptr;
    int rightBarW = 0, rightButtonW = 0;

    // House palette (same values as Spread's spr::theme).
    juce::Colour background { 0xff14101a };
    juce::Colour accent     { 0xffe0784a };
    juce::Colour text       { 0xffd8d8e0 };
    juce::Colour dimText    { 0xff9a9aa8 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopBar)
};

} // namespace fxme
