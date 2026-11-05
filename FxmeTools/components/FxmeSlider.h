/*
  ==============================================================================

    FxmeSlider.h

    A juce::Slider with no text box and right-click value entry (a small inline
    editable label). Optional construction from an APVTS parameter.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
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
        // An attachment sets the slider's range from its parameter's: put the
        // logarithmic travel back on top of it.
        if (logarithmic)
            applyLogarithmicRange();
    }

    /** Logarithmic travel, for a frequency: equal movements make equal
        ratios, so the low end gets as much room as the high end (20 Hz to
        20 kHz puts 200 Hz a third of the way and 2 kHz two thirds).

        Only the knob's travel changes: the parameter, its values, its
        automation and its host-side display are untouched. Before or after
        setAttachment(), either works; a later setRange() makes it linear
        again (call this again after it). Ignored unless the range is
        positive. Keeps the range's interval. */
    void setLogarithmic(bool shouldBeLogarithmic = true)
    {
        logarithmic = shouldBeLogarithmic;
        if (logarithmic)
            applyLogarithmicRange();
        else
            setNormalisableRange(juce::NormalisableRange<double>(getMinimum(), getMaximum(), getInterval()));
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
    bool logarithmic = false;

    void applyLogarithmicRange()
    {
        const double start = getMinimum(), end = getMaximum(), interval = getInterval();
        if (start <= 0.0 || end <= start)
            return;

        juce::NormalisableRange<double> range(start, end,
            [](double s, double e, double proportion) { return s * std::pow(e / s, proportion); },
            [](double s, double e, double value)      { return std::log(value / s) / std::log(e / s); },
            [interval](double s, double e, double value)
            {
                if (interval > 0.0)
                    value = s + interval * std::round((value - s) / interval);
                return juce::jlimit(s, e, value);
            });
        range.interval = interval;
        const double value = getValue();
        setNormalisableRange(range);
        setValue(value, juce::dontSendNotification);
    }

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxmeSlider)
};

} // namespace fxme
