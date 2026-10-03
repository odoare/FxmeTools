/*
  ==============================================================================

    PresetBarComponent.h

    Compact companion to PresetComponent: a one-line preset strip showing
    only the current preset name (with dirty marker) between previous/next
    arrow buttons. Designed to sit in leftover chrome space, e.g. at the
    right end of a TabbedComponent's tab bar:

        presetBar = std::make_unique<fxme::PresetBarComponent> (processor.getPresetManager());
        presetBar->setAccentColour (myThemeColour);
        addAndMakeVisible (*presetBar);   // after the tabs, so it stacks on top
        // in resized():
        presetBar->setBounds (getLocalBounds().removeFromTop (tabs.getTabBarDepth())
                                              .removeFromRight (300).reduced (4, 3));

    All preset handling is delegated to a PresetBank (the plugin's
    PresetManager, or a module preset target), so this stays in sync with a
    full PresetComponent shown elsewhere in the GUI.

    Optionally, a browse button ("...") at the right end opens that full
    PresetComponent in a callout anchored on it, inside the plugin window
    (off by default; existing bars are unchanged):

        presetBar->setBrowserButtonVisible (true);
        presetBar->setBrowserSize (320, 380);   // the callout's content, the default

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../presets/PresetBank.h"
#include "../presets/PresetManager.h"   // not needed here; kept for code that relied on it
#include "../lookandfeels/FxmeLookAndFeel.h"
#include "PresetComponent.h"

namespace fxme
{

class PresetBarComponent : public juce::Component,
                           private juce::ChangeListener
{
public:
    explicit PresetBarComponent (PresetBank& bankToUse)
        : manager (bankToUse)
    {
        manager.addChangeListener (this);

        for (auto* b : { &prevButton, &nextButton })
        {
            b->setLookAndFeel (&lookAndFeel);
            addAndMakeVisible (*b);
        }
        prevButton.setTooltip ("Previous preset");
        nextButton.setTooltip ("Next preset");
        prevButton.onClick = [this] { manager.loadPrevious(); };
        nextButton.onClick = [this] { manager.loadNext(); };

        browseButton.setLookAndFeel (&lookAndFeel);
        browseButton.setTooltip ("Browse, save and delete presets");
        browseButton.onClick = [this] { showBrowser(); };
        addChildComponent (browseButton);

        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (nameLabel);

        setAccentColour (accent);
        refresh();
    }

    ~PresetBarComponent() override
    {
        manager.removeChangeListener (this);
        for (auto* b : { &prevButton, &nextButton, &browseButton })
            b->setLookAndFeel (nullptr);
    }

    //==========================================================================
    // The browse button (off by default)

    /** Shows or hides the "..." button at the right end of the bar. */
    void setBrowserButtonVisible (bool shouldBeVisible)
    {
        browseButton.setVisible (shouldBeVisible);
        resized();
    }

    bool isBrowserButtonVisible() const noexcept     { return browseButton.isVisible(); }

    /** The size of the PresetComponent the button opens (default 320 x 380). */
    void setBrowserSize (int width, int height)
    {
        browserSize = { juce::jmax (1, width), juce::jmax (1, height) };
    }

    /** The browser's accent colour; by default the bar's own. */
    void setBrowserAccentColour (juce::Colour colour)
    {
        browserAccent = colour;
        hasBrowserAccent = true;
    }

    /** Opens the browser as if the button had been clicked (it need not be
        visible). */
    void showBrowser()
    {
        // Another instance (or another plugin) may have saved presets since.
        manager.rescanUserPresets();

        auto browser = std::make_unique<PresetComponent> (manager);
        browser->setAccentColour (hasBrowserAccent ? browserAccent : accent);
        browser->setSize (browserSize.x, browserSize.y);

        // Inside the plugin window when there is one: a callout parented to
        // the desktop misbehaves in some hosts.
        juce::Component* parent = findParentComponentOfClass<juce::AudioProcessorEditor>();
        if (parent == nullptr)
            parent = getTopLevelComponent();

        const auto anchor = browseButton.isVisible() ? browseButton.getBounds() : getLocalBounds();
        const auto area = parent != nullptr && parent != this
                              ? parent->getLocalArea (this, anchor)
                              : localAreaToGlobal (anchor);

        juce::CallOutBox::launchAsynchronously (std::move (browser), area,
                                                parent != this ? parent : nullptr);
    }

    void setAccentColour (juce::Colour newAccent)
    {
        accent = newAccent;
        nameLabel.setColour (juce::Label::textColourId, accent.brighter (0.5f));
        for (auto* b : { &prevButton, &nextButton, &browseButton })
        {
            b->setColour (juce::TextButton::buttonColourId,  juce::Colours::black.withAlpha (0.4f));
            b->setColour (juce::TextButton::textColourOffId, accent.brighter (0.3f));
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (b.reduced (0.5f), b.getHeight() * 0.5f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const int btnW = area.getHeight();
        if (browseButton.isVisible())
            browseButton.setBounds (area.removeFromRight (btnW));
        prevButton.setBounds (area.removeFromLeft (btnW));
        nextButton.setBounds (area.removeFromRight (btnW));
        nameLabel.setBounds (area);
        nameLabel.setFont (juce::Font (juce::FontOptions ((float) getHeight() * 0.55f, juce::Font::bold)));
    }

private:
    void refresh()
    {
        auto name = manager.getCurrentPresetName();
        if (name.isEmpty())
            name = "(no preset)";
        if (manager.isDirty())
            name << " *";
        nameLabel.setText (name, juce::dontSendNotification);
        nameLabel.setTooltip (name);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    PresetBank& manager;

    juce::Label nameLabel;
    juce::TextButton prevButton { "<" }, nextButton { ">" }, browseButton { "..." };

    juce::Colour accent { juce::Colours::orange };
    juce::Colour browserAccent;
    bool hasBrowserAccent = false;
    juce::Point<int> browserSize { 320, 380 };
    FxmeLookAndFeel lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBarComponent)
};

} // namespace fxme
