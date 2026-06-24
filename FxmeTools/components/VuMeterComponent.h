/*
  ------------------------------------------------------------------------------
    VuMeterComponent.h

    Simple push-style level-bar component. The owner feeds it a dB value with
    setValue() (typically from a GUI timer reading a fxme::VuMeter); the bar
    fills bottom-to-top (or left-to-right when horizontal) over a configurable
    range, with an optional zero-level marker line. Repaints at 30 Hz.

    Pure display, depends only on JUCE. For lambda-driven (pull-style) meters
    see fxme::FxmeMeters; for a calibrated dBFS/SPL bar see
    fxme::SplMeterComponent.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

/**
 * @class VuMeterComponent
 * @brief A component that displays a VU meter.
 */
class VuMeterComponent : public juce::Component,
                         public juce::Timer
{
public:
    VuMeterComponent()
    {
        startTimerHz (30);
    }

    ~VuMeterComponent() override
    {
        stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);

        float displayValue = juce::jlimit (minValue, maxValue, value);
        float proportion = juce::jmap (displayValue, minValue, maxValue, 0.0f, 1.0f);

        g.setColour (meterColor);

        if (horizontal)
        {
            int width = juce::roundToInt (proportion * getWidth());
            g.fillRect (0, 0, width, getHeight());

            // Zero Level (vertical mark).
            if (zeroLevel > minValue && zeroLevel < maxValue)
            {
                float zeroProp = juce::jmap (zeroLevel, minValue, maxValue, 0.0f, 1.0f);
                int zeroX = juce::roundToInt (zeroProp * getWidth());
                g.setColour (juce::Colours::white);
                g.drawLine ((float) zeroX, 0.0f, (float) zeroX, (float) getHeight(), 1.0f);
            }
        }
        else
        {
            int height = juce::roundToInt (proportion * getHeight());
            g.fillRect (0, getHeight() - height, getWidth(), height);

            // Zero Level (horizontal mark).
            if (zeroLevel > minValue && zeroLevel < maxValue)
            {
                float zeroProp = juce::jmap (zeroLevel, minValue, maxValue, 0.0f, 1.0f);
                int zeroY = juce::roundToInt ((1.0f - zeroProp) * getHeight());
                g.setColour (juce::Colours::white);
                g.drawLine (0, (float) zeroY, (float) getWidth(), (float) zeroY, 1.0f);
            }
        }
    }

    void resized() override
    {
    }

    /** Sets the current value to display, in dB. */
    void setValue (float newValue)
    {
        value = newValue;
    }

    /** Sets the color of the meter bar. */
    void setMeterColor (juce::Colour newColor)
    {
        meterColor = newColor;
    }

    /** Sets the display range of the meter (dB). */
    void setRange (float newMin, float newMax)
    {
        minValue = newMin;
        maxValue = newMax;
    }

    /** Sets the zero level mark (dB). */
    void setZeroLevel (float newZeroLevel)
    {
        zeroLevel = newZeroLevel;
    }

    /** Selects the bar orientation. If true the bar fills left-to-right;
        otherwise bottom-to-top (the default). */
    void setHorizontal (bool shouldBeHorizontal)
    {
        horizontal = shouldBeHorizontal;
    }

private:
    void timerCallback() override
    {
        repaint();
    }

    float value = -100.0f;
    juce::Colour meterColor = juce::Colours::green;
    float minValue = -60.0f;
    float maxValue = 0.0f;
    float zeroLevel = -100.0f; // -Inf
    bool horizontal = false;
};

} // namespace fxme
