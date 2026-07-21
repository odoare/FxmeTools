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
#include <utility>
#include <vector>

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

    /** Parks externally-owned controls (a level-meter strip, the compact
        PresetBarComponent, its toggle button...) in the free space to the
        left of the version string, laid out left to right in the order
        given; the blurb is shortened so it never runs under them. Entries
        with a null component are skipped, so a caller can pass an optional
        control unconditionally. The bar does not take ownership. */
    void setRightControls (std::initializer_list<std::pair<juce::Component*, int>> controls)
    {
        rightControls.clear();
        for (const auto& [component, width] : controls)
            if (component != nullptr)
            {
                rightControls.emplace_back (component, width);
                addAndMakeVisible (*component);
            }
        resized();
    }

    /** The common bar + toggle pairing. */
    void setRightControls (juce::Component* bar, int barWidth,
                           juce::Component* button, int buttonWidth)
    {
        setRightControls ({ { bar, barWidth }, { button, buttonWidth } });
    }

    void resized() override
    {
        if (rightControls.empty())
            return;

        auto area = getLocalBounds().reduced (12, 6);
        area.removeFromRight (kVersionWidth + 8);   // leave the version alone

        // The strip is exactly as wide as the controls plus their gaps, so
        // filling it left to right lands the last one against the version.
        auto strip = area.removeFromRight (reservedRight() - 8);
        const int stripH = juce::jmin (kControlHeight, strip.getHeight());
        strip = strip.withSizeKeepingCentre (strip.getWidth(), stripH);

        for (size_t i = 0; i < rightControls.size(); ++i)
        {
            if (i > 0)
                strip.removeFromLeft (kControlGap);
            rightControls[i].first->setBounds (strip.removeFromLeft (rightControls[i].second));
        }
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
        if (rightControls.empty())
            return 0;

        int total = 8;   // gap to the version string
        for (size_t i = 0; i < rightControls.size(); ++i)
            total += rightControls[i].second + (i > 0 ? kControlGap : 0);
        return total;
    }

    static constexpr int kVersionWidth  = 150;
    static constexpr int kControlGap    = 6;
    static constexpr int kControlHeight = 28;

    juce::String name, blurb, version;
    juce::Image logo;

    std::vector<std::pair<juce::Component*, int>> rightControls;

    // House palette (same values as Spread's spr::theme).
    juce::Colour background { 0xff14101a };
    juce::Colour accent     { 0xffe0784a };
    juce::Colour text       { 0xffd8d8e0 };
    juce::Colour dimText    { 0xff9a9aa8 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopBar)
};

} // namespace fxme
