/*
  ==============================================================================

    FxmeButton.h

    A small Component wrapping a juce::ToggleButton bound to an APVTS parameter.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

class FxmeButton : public juce::Component
{

public:
    FxmeButton(juce::AudioProcessorValueTreeState& apvts
                , juce::String paramName = "dummy"
                , juce::Colour colour = juce::Colours::white)
        : apvtsRef(apvts), parameterID(paramName)
    {
        button.setColour(juce::ToggleButton::tickColourId,colour);
        button.setButtonText(paramName);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(apvts,paramName,button);
        addAndMakeVisible(button);
    }

    FxmeButton(juce::AudioProcessorValueTreeState& apvts
                , juce::String paramName = "dummy"
                , juce::String labelText = "Button"
                , juce::Colour colour = juce::Colours::white)
        : apvtsRef(apvts), parameterID(paramName)
    {
        button.setColour(juce::ToggleButton::tickColourId,colour);
        button.setButtonText(labelText);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(apvts,paramName,button);
        addAndMakeVisible(button);
    }

    ~FxmeButton() override
    {
    }

    void resized() override
    {
        button.setBounds(getLocalBounds());
    }

    // Allow external LookAndFeel to be set on the internal button
    void setLookAndFeel(juce::LookAndFeel* newLookAndFeel)
    {
        button.setLookAndFeel(newLookAndFeel);
    }

    juce::RangedAudioParameter* getParameter() const
    {
        return apvtsRef.getParameter(parameterID);
    }

    juce::ToggleButton button;

private:
    juce::AudioProcessorValueTreeState& apvtsRef;
    juce::String parameterID;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxmeButton)

};

} // namespace fxme
