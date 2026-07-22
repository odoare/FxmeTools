/*
  ------------------------------------------------------------------------------
    SplashOverlay.h

    A cover-the-window splash / about screen: a dimmed backdrop with the
    plugin's artwork centred on it, fading in, holding, then fading out on
    its own. Clicking anywhere dismisses it early, and while it is up it
    swallows mouse events so nothing underneath can be touched by accident.

    Purely a display; it holds no policy about *when* to appear. The owner
    decides that — typically "once per plugin instance" for the startup
    showing (a flag on the processor, which outlives the editor, rather than
    on the editor, which is rebuilt every time the window opens) plus
    whatever gesture opens it on demand, e.g. fxme::TopBar::onLogoClicked.

    Usage (message thread only, like any juce::Component):

        fxme::SplashOverlay splash;
        ...
        splash.setImage (juce::ImageCache::getFromMemory (
            BinaryData::Splash_png, BinaryData::Splash_pngSize));
        addChildComponent (splash);            // hidden until shown
        ...
        splash.setBounds (getLocalBounds());   // in resized()
        splash.show();                         // 2 s by default

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace fxme
{

class SplashOverlay : public juce::Component,
                      private juce::Timer
{
public:
    SplashOverlay()
    {
        setVisible (false);
        setInterceptsMouseClicks (true, false);   // eat clicks while up
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    ~SplashOverlay() override { stopTimer(); }

    /** The artwork. Drawn centred, scaled down to fit but never enlarged. */
    void setImage (juce::Image newImage) { image = std::move (newImage); }

    /** Colour washed over the window behind the artwork (its alpha is the
        maximum dimming; the fade scales it). */
    void setBackdropColour (juce::Colour c) { backdrop = c; }

    /** Fraction of the shorter edge left as a margin around the artwork. */
    void setMarginFraction (float f) { margin = juce::jlimit (0.0f, 0.4f, f); }

    /** Fades in, holds for `holdMs`, fades out. Calling it while already up
        restarts the hold — clicking the logo repeatedly keeps it visible
        rather than stacking timers. Does nothing without a valid image. */
    void show (int holdMs = 2000)
    {
        if (! image.isValid())
            return;

        holdMillis  = juce::jmax (0, holdMs);
        startMillis = juce::Time::getMillisecondCounter();
        dismissing  = false;
        setVisible (true);
        toFront (false);
        startTimerHz (60);
    }

    /** Starts the fade-out from wherever the fade currently is. */
    void dismiss()
    {
        if (! isVisible() || dismissing)
            return;

        dismissing    = true;
        dismissAlpha  = alpha;
        dismissMillis = juce::Time::getMillisecondCounter();
    }

    /** Fired once the overlay has finished fading out. */
    std::function<void()> onDismissed;

    void paint (juce::Graphics& g) override
    {
        if (! image.isValid() || alpha <= 0.0f)
            return;

        g.setColour (backdrop.withMultipliedAlpha (alpha));
        g.fillAll();

        const auto inset = (int) (margin * (float) juce::jmin (getWidth(), getHeight()));
        g.setOpacity (alpha);
        g.drawImage (image, getLocalBounds().reduced (inset).toFloat(),
                     juce::RectanglePlacement::centred
                   | juce::RectanglePlacement::onlyReduceInSize);
    }

    void mouseUp (const juce::MouseEvent&) override { dismiss(); }

private:
    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounter();

        if (dismissing)
        {
            const float t = (float) (now - dismissMillis) / (float) kFadeOutMs;
            alpha = dismissAlpha * (1.0f - juce::jlimit (0.0f, 1.0f, t));

            if (t >= 1.0f)
            {
                stopTimer();
                alpha = 0.0f;
                setVisible (false);
                if (onDismissed)
                    onDismissed();
                return;
            }
        }
        else
        {
            const auto elapsed = (int) (now - startMillis);
            if (elapsed >= kFadeInMs + holdMillis)
            {
                dismiss();
                return;             // the next tick starts fading out
            }
            alpha = elapsed < kFadeInMs ? (float) elapsed / (float) kFadeInMs : 1.0f;
        }

        repaint();
    }

    static constexpr int kFadeInMs  = 180;
    static constexpr int kFadeOutMs = 320;

    juce::Image  image;
    juce::Colour backdrop { 0xd8101010 };
    float        margin = 0.06f;

    float    alpha = 0.0f;
    bool     dismissing = false;
    float    dismissAlpha = 0.0f;
    int      holdMillis = 2000;
    juce::uint32 startMillis = 0, dismissMillis = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplashOverlay)
};

} // namespace fxme
