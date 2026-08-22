/*
  ------------------------------------------------------------------------------
    PanelBackground.h

    The two backdrops behind an FX-Mechanics plugin GUI. Both take the panel's
    own accent colour and return near-black with only a whisper of it, which is
    what keeps a family of differently-tinted plugins looking like one product
    rather than a paint chart.

      paintTintedBackground     flat vertical gradient, for the outermost
                                editor. Almost all of it ends up behind the
                                effect component below, so it only shows in
                                whatever chrome is left over.

      paintComponentBackground  the same treatment along the component's
                                corner-to-corner diagonal, for an embeddable
                                effect component's own paint(). This is the one
                                that actually fills a plugin window.

    Free functions rather than LookAndFeel overrides: a Component's backdrop is
    its own business (it knows its accent), and this way a component can paint
    it without owning a look-and-feel.

    Usage:

        void MyComponent::paint (juce::Graphics& g)
        {
            fxme::paintComponentBackground (g, getLocalBounds().toFloat(), myTint);
        }


    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_graphics/juce_graphics.h>

namespace fxme
{

/** Near-black with a whisper of the accent, as a flat vertical gradient. For
    the outermost editor's paint(). */
inline void paintTintedBackground (juce::Graphics& g, juce::Rectangle<float> bounds,
                                   juce::Colour accent)
{
    const auto topColour    = juce::Colours::black.interpolatedWith (accent, 0.05f);
    const auto bottomColour = juce::Colours::black.interpolatedWith (accent, 0.02f);
    juce::ColourGradient grad (bottomColour, bounds.getBottomLeft(),
                               topColour, bounds.getTopRight(), false);
    g.setGradientFill (grad);
    g.fillRect (bounds);
}

/** The same near-black/whisper-of-accent treatment, but running along the
    component's corner-to-corner diagonal, so the gradient reads the same
    whatever aspect ratio the component is laid out at. For an embeddable
    effect component's own paint(). */
inline void paintComponentBackground (juce::Graphics& g, juce::Rectangle<float> bounds,
                                      juce::Colour accent)
{
    const auto diagonale     = bounds.getTopLeft() - bounds.getBottomRight();
    const auto length        = diagonale.getDistanceFromOrigin();
    const auto perpendicular = diagonale.rotatedAboutOrigin (juce::degreesToRadians (270.0f)) / length;
    const auto height        = (bounds.getWidth() * bounds.getHeight()) / length;

    const auto darkBase    = juce::Colour (0xff181818);
    const auto lightCorner = darkBase.interpolatedWith (accent, 0.14f);
    const auto darkCorner  = darkBase.interpolatedWith (accent, 0.05f);

    juce::ColourGradient grad (darkCorner,  perpendicular *  height,
                               lightCorner, perpendicular * -height, false);
    g.setGradientFill (grad);
    g.fillRect (bounds);
}

} // namespace fxme
