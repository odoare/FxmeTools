/*
  ==============================================================================

    FxmeSlider.h

    A juce::Slider with no text box and right-click value entry (a small inline
    editable label). Optional construction from an APVTS parameter.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

class FxmeSlider : public juce::Slider
{
public:
    FxmeSlider()
    {
        setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    }
    FxmeSlider(juce::AudioProcessorValueTreeState& apvts,
               const juce::String& paramID,
               const juce::String& labelText,
               const juce::Colour& colour)
    {
      setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
      setName(labelText);

      attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, paramID, *this);
    }
    ~FxmeSlider() override
    {
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
    {
        auto* label = new juce::Label();
        label->setEditable(true);
        label->setText(getTextFromValue(getValue()), juce::NotificationType::dontSendNotification);
        label->setJustificationType(juce::Justification::centred);

        label->setColour(juce::Label::backgroundColourId, juce::Colours::black);
        label->setColour(juce::Label::textColourId, juce::Colours::white);
        label->setColour(juce::Label::outlineColourId, juce::Colours::grey);

        auto bounds = getScreenBounds();
        label->setBounds(0, 0, juce::jmin(80, bounds.getWidth()), 20);
        label->setCentrePosition(bounds.getCentre());

        juce::Component::SafePointer<FxmeSlider> safeThis(this);

        label->onTextChange = [safeThis, label]() {
            if (safeThis)
                safeThis->setValue(safeThis->getValueFromText(label->getText()));
        };

        label->onEditorHide = [label]() {
            juce::MessageManager::getInstance()->callAsync([label]() { delete label; });
        };

        label->addToDesktop(juce::ComponentPeer::windowIsTemporary |         juce::ComponentPeer::windowHasDropShadow);
        label->setVisible(true);
        label->showEditor();
    }
    else
    {
        juce::Slider::mouseDown(e);
    }

    }

    void setAttachment(juce::AudioProcessorValueTreeState::SliderAttachment* a)
    {
        attachment.reset(a);
    }

    void setCentralValue(double value)
    {
        getProperties().set("centralValue", value);
    }

    void setShowLabel(bool shouldShowLabel)
    {
        getProperties().set("showLabel", shouldShowLabel);
    }


private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment{nullptr};

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxmeSlider)
};

} // namespace fxme
