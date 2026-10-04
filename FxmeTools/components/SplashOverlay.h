/*
  ------------------------------------------------------------------------------
    SplashOverlay.h

    A cover-the-window splash / about screen: a dimmed backdrop with the
    plugin's artwork centred on it, fading in, holding, then fading out on
    its own (or, shown with a negative hold, staying until clicked). Clicking
    anywhere dismisses it early, and while it is up it swallows mouse events
    so nothing underneath can be touched by accident.

    Optionally (setLink), a row under the artwork with a small logo and a
    web address, both clickable: they open the address in the browser and
    leave the splash up; the address is underlined while the pointer is on
    the row.

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
        splash.show (-1);                      // until clicked (an about box)

        splash.setLink ("fx-mechanics.com", juce::URL ("https://fx-mechanics.com"),
                        companyLogo);          // optional

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
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

    /** A clickable row under the artwork: `logo` (optional, drawn small)
        then `text`, both opening `url`. Clicking them leaves the splash up.
        An empty text removes the row. */
    void setLink (const juce::String& text, const juce::URL& url, juce::Image logo = {})
    {
        linkText = text;
        linkUrl = url;
        linkLogo = std::move (logo);
        repaint();
    }

    /** The link's colour (the logo is drawn as it is). */
    void setLinkColour (juce::Colour c) { linkColour = c; repaint(); }

    /** Fades in, holds for `holdMs`, fades out; with a negative `holdMs` it
        stays until clicked. Calling it while already up restarts the hold —
        clicking the logo repeatedly keeps it visible rather than stacking
        timers. Does nothing without a valid image. */
    void show (int holdMs = 2000)
    {
        if (! image.isValid())
            return;

        holdMillis  = holdMs;
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
        startTimerHz (60);     // it may have stopped, holding until clicked
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
        auto content = getLocalBounds().reduced (inset);
        const auto linkRow = linkText.isNotEmpty() ? content.removeFromBottom (kLinkRowHeight)
                                                   : juce::Rectangle<int>();

        g.setOpacity (alpha);
        g.drawImage (image, content.toFloat(),
                     juce::RectanglePlacement::centred
                   | juce::RectanglePlacement::onlyReduceInSize);

        paintLink (g, linkRow);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (linkHit.contains (e.getPosition()) && linkUrl.isWellFormed())
            linkUrl.launchInDefaultBrowser();     // the splash stays up
        else
            dismiss();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool over = linkHit.contains (e.getPosition());
        if (over != linkHot)
        {
            linkHot = over;
            repaint (linkHit);
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (linkHot)
        {
            linkHot = false;
            repaint (linkHit);
        }
    }

private:
    /** The logo and the address, centred in `row`; records their area for
        the hit test. */
    void paintLink (juce::Graphics& g, juce::Rectangle<int> row)
    {
        linkHit = {};
        if (row.isEmpty() || linkText.isEmpty())
            return;

        const juce::Font font (juce::FontOptions (16.0f));
        const int textW = juce::GlyphArrangement::getStringWidthInt (font, linkText) + 2;
        const int logoH = linkLogo.isValid() ? row.getHeight() - 6 : 0;
        const int logoW = linkLogo.isValid() ? logoH * linkLogo.getWidth() / juce::jmax (1, linkLogo.getHeight()) : 0;
        const int gap   = linkLogo.isValid() ? 10 : 0;

        auto r = row.withSizeKeepingCentre (logoW + gap + textW, row.getHeight());
        linkHit = r;

        if (linkLogo.isValid())
        {
            g.setOpacity (alpha);
            g.drawImage (linkLogo, r.removeFromLeft (logoW).withSizeKeepingCentre (logoW, logoH).toFloat(),
                         juce::RectanglePlacement::centred);
            r.removeFromLeft (gap);
        }

        g.setFont (font);
        g.setColour (linkColour.withMultipliedAlpha (alpha * (linkHot ? 1.0f : 0.8f)));
        g.drawText (linkText, r, juce::Justification::centredLeft, false);
        if (linkHot)
            g.fillRect (r.getX(), r.getCentreY() + (int) (font.getHeight() * 0.45f), textW - 2, 1);
    }

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
            if (holdMillis >= 0 && elapsed >= kFadeInMs + holdMillis)
            {
                dismiss();
                return;             // the next tick starts fading out
            }
            alpha = elapsed < kFadeInMs ? (float) elapsed / (float) kFadeInMs : 1.0f;

            // Holding until clicked: nothing moves any more, so stop ticking
            // (dismiss() starts the timer again for the fade-out).
            if (holdMillis < 0 && alpha >= 1.0f)
            {
                stopTimer();
                repaint();
                return;
            }
        }

        repaint();
    }

    static constexpr int kFadeInMs  = 180;
    static constexpr int kFadeOutMs = 320;
    static constexpr int kLinkRowHeight = 34;

    juce::String linkText;
    juce::URL    linkUrl;
    juce::Image  linkLogo;
    juce::Colour linkColour { 0xff35d6d0 };
    juce::Rectangle<int> linkHit;   // as last painted
    bool linkHot = false;

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
