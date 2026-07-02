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

    All preset handling is delegated to the shared PresetManager, so this
    stays in sync with a full PresetComponent shown elsewhere in the GUI.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../presets/PresetManager.h"
#include "../lookandfeels/FxmeLookAndFeel.h"

namespace fxme
{

class PresetBarComponent : public juce::Component,
                           private juce::ChangeListener
{
public:
    explicit PresetBarComponent (PresetManager& managerToUse)
        : manager (managerToUse)
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

        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (nameLabel);

        setAccentColour (accent);
        refresh();
    }

    ~PresetBarComponent() override
    {
        manager.removeChangeListener (this);
        for (auto* b : { &prevButton, &nextButton })
            b->setLookAndFeel (nullptr);
    }

    void setAccentColour (juce::Colour newAccent)
    {
        accent = newAccent;
        nameLabel.setColour (juce::Label::textColourId, accent.brighter (0.5f));
        for (auto* b : { &prevButton, &nextButton })
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

    PresetManager& manager;

    juce::Label nameLabel;
    juce::TextButton prevButton { "<" }, nextButton { ">" };

    juce::Colour accent { juce::Colours::orange };
    FxmeLookAndFeel lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBarComponent)
};

} // namespace fxme
