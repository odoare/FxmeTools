/*
  ==============================================================================

    FxmeLookAndFeel.h

    FX-Mechanics LookAndFeel: rotary knobs with a centred value read-out and
    optional label, bipolar/unipolar linear bar sliders, and pill-style toggle
    and text buttons. Colours are read from the standard juce Slider/Button
    ColourIds, so callers theme it via setColour() on the widgets.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace fxme
{

class FxmeLookAndFeel : public juce::LookAndFeel_V4
{
public:

  void drawRotarySlider(juce::Graphics& g,
          int x, int y,
          int width,
          int height,
          float sliderPos,
          float rotaryStartAngle,
          float rotaryEndAngle,
          juce::Slider& slider) override
  {
    // Optional label drawn just below the knob — opt-in via the "showLabel"
    // property so mixer knobs (no room) stay unchanged while effect knobs
    // can display their name compactly.
    const bool showLabel = slider.getProperties().getWithDefault ("showLabel", false);

    float diameter = 0.7f * juce::jmin ((float) width, (float) height);
    // Label height scales with the knob so the text stays proportionate; the
    // (knob + label) cluster is then centred vertically so we don't waste
    // space below the knob.
    const float labelHeight = showLabel
        ? juce::jmin (diameter * 0.32f, (float) height - diameter)
        : 0.0f;
    const float clusterHeight = diameter + labelHeight;
    float radius = diameter * 0.5f;
    float centreX = (float) x + (float) width * 0.5f;
    float centreY = (float) y + ((float) height - clusterHeight) * 0.5f + radius;
    float rx = centreX - radius;
    float ry = centreY - radius;
    float angle = rotaryStartAngle + (sliderPos * (rotaryEndAngle-rotaryStartAngle));
    float thickness = diameter/12;

    juce::PathStrokeType path{thickness, juce::PathStrokeType::JointStyle::curved, juce::PathStrokeType::EndCapStyle::rounded};

    juce::Rectangle<float> dialArea(rx,ry,diameter,diameter);
    g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId).brighter(2.f));
    g.drawEllipse(dialArea.reduced(thickness).translated(0.f,-thickness*0.12f),thickness*0.36f);
    g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId));
    g.fillEllipse(dialArea.reduced(thickness));

    g.setColour(slider.findColour(juce::Slider::thumbColourId));

    // Rectangle ?
    juce::Path dialTick;
    juce::Rectangle<float> rect(.25f*thickness,-radius+2.*thickness,.5*thickness,radius*0.2);
    dialTick.addRectangle(rect);
    g.fillPath(dialTick,juce::AffineTransform::rotation(angle).translated(centreX,centreY));

    // // Disc ?
    // juce::Rectangle<float> thumbArea(0.f,-radius+2*thickness,thickness,thickness);
    // g.fillEllipse(thumbArea.transformedBy(juce::AffineTransform::rotation(angle).translated(centreX,centreY)));

    g.setColour(slider.findColour(juce::Slider::rotarySliderOutlineColourId));
    juce::Path arc1;
    arc1.addArc(centreX-diameter/2, centreY-diameter/2, diameter, diameter, rotaryStartAngle, rotaryEndAngle, true);
    g.strokePath(arc1, path);

    g.setColour(slider.findColour(juce::Slider::trackColourId));
    juce::Path arc2;

    float zeroPos = 0.0f; // Default start at min value

    if (slider.getProperties().contains("centralValue"))
    {
        double cv = slider.getProperties()["centralValue"];
        zeroPos = (float)slider.valueToProportionOfLength(cv);
    }
    else if (slider.getProperties().getWithDefault ("drawFromCentre", false))
    {
        zeroPos = 0.5f;
    }

    float zeroAngle = rotaryStartAngle + zeroPos * (rotaryEndAngle - rotaryStartAngle);

    arc2.addArc (rx, ry, diameter, diameter,
                 juce::jmin (angle, zeroAngle),
                 juce::jmax (angle, zeroAngle),
                 true);

    g.strokePath(arc2, path);

    // Draw the slider's value as text in the center
    auto text = slider.getTextFromValue(slider.getValue());
    g.setColour(juce::Colours::white.withAlpha(0.7f));
    // Make font size proportional to the knob's diameter
    g.setFont(juce::jmin(15.0f, diameter * 0.3f));
    g.drawText(text, dialArea.toNearestInt(), juce::Justification::centred, true);

    if (showLabel)
    {
        // Sit the label just below the knob; font scales with the knob so the
        // text grows with the available size.
        juce::Rectangle<float> nameArea ((float) x, centreY + radius, (float) width, labelHeight);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        // Cap the label font so it stays readable on large knobs instead of
        // growing without bound with the diameter.
        constexpr float maxLabelFontSize = 16.0f;
        g.setFont (juce::jmin (diameter * 0.25f, labelHeight * 0.9f, maxLabelFontSize));
        g.drawText (slider.getName(), nameArea.toNearestInt(), juce::Justification::centred, true);
    }
  };

  juce::Slider::SliderLayout getSliderLayout (juce::Slider& slider) override
  {
      auto style = slider.getSliderStyle();

      // For our custom linear sliders, we want the bar to fill the entire component bounds.
      if (style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearBarVertical)
      {
          juce::Slider::SliderLayout layout;
          layout.sliderBounds = slider.getLocalBounds();
          layout.textBoxBounds = {}; // No text box
          return layout;
      }

      // For all other styles (like rotary), use the default V4 implementation.
      return LookAndFeel_V4::getSliderLayout (slider);
  }

  void drawToggleButton(juce::Graphics &g,
                            juce::ToggleButton &b,
                            bool 	shouldDrawButtonAsHighlighted,
                            bool 	shouldDrawButtonAsDown ) override
  {
    auto bounds = b.getLocalBounds();
    float w = juce::jmin<float>(bounds.getWidth(), bounds.getHeight())*.1f;
    bounds = bounds.reduced(2*w);
    auto isDown = b.getToggleState();
    auto col = b.findColour(juce::ToggleButton::tickColourId);
    float t;
    t = w*.5f;

    if (isDown)
    {
      g.setColour(col.brighter(1.5f));
    }
      else
    {
      g.setColour(col);
    }
    g.drawRoundedRectangle(bounds.toFloat(),w*2,t);

    // if (b.isMouseOver())
    // {
    //   g.setFont(juce::Font(juce::Font::getDefaultSansSerifFontName(),w*3,juce::Font::bold));
    // }
    // else
    // {
    //   g.setFont(juce::Font(juce::Font::getDefaultSansSerifFontName(),w*3,juce::Font::plain));
    // }

    if (isDown)
    {
      g.setColour(col.brighter(0.7f));
      g.fillRoundedRectangle(bounds.toFloat().translated(w*.1,w*.1),w*2);
      g.setColour(col.darker(1.f));
      g.drawText(b.getButtonText(),bounds,juce::Justification::centred);
    }
    else
    {
      g.setColour(col.darker(.7f));
      g.fillRoundedRectangle(bounds.toFloat().translated(w*.1,w*.1),w*2);
      g.setColour(col.brighter(1.f));
      g.drawText(b.getButtonText(),bounds,juce::Justification::centred);
    }
  }

  void drawButtonBackground(juce::Graphics& g,
                            juce::Button& button,
                            const juce::Colour& backgroundColour,
                            bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
  {
      auto bounds = button.getLocalBounds();
      float w = juce::jmin<float>(bounds.getWidth(), bounds.getHeight()) * 0.1f;
      bounds = bounds.reduced(2 * w);

      // Use buttonColourId for TextButtons, fallback to a default
      auto col = button.findColour(juce::TextButton::buttonColourId);
      float t = w * 0.5f; // thickness

      if (shouldDrawButtonAsDown)
      {
          g.setColour(col.brighter(1.5f));
      }
      else
      {
          g.setColour(col);
      }
      g.drawRoundedRectangle(bounds.toFloat(), w * 2, t);

      if (shouldDrawButtonAsDown)
      {
          g.setColour(col.brighter(0.7f));
          g.fillRoundedRectangle(bounds.toFloat().translated(w * 0.1, w * 0.1), w * 2);
      }
      else
      {
          g.setColour(col.darker(0.7f));
          g.fillRoundedRectangle(bounds.toFloat().translated(w * 0.1, w * 0.1), w * 2);
      }
  }

  void drawLinearSlider(juce::Graphics& g,
          int x, int y,
          int width,
          int height,
          float sliderPos,
          float minSliderPos,
          float maxSliderPos,
          const juce::Slider::SliderStyle style,
          juce::Slider& slider) override
  {
    if (style == juce::Slider::LinearBarVertical)
    {
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();

        // Background
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (bounds, 4.0f);

        // Get the proportion directly from the slider's value (0.0 to 1.0)
        const float levelProportion = (float) slider.valueToProportionOfLength (slider.getValue());
        auto filledBounds = bounds;

        // Only draw the bar if the level is greater than the minimum
        if (levelProportion > 0.0f)
        {
            auto colour = slider.findColour (juce::Slider::trackColourId);
            // Remove the unfilled part from the top
            filledBounds.removeFromTop (filledBounds.getHeight() * (1.0f - levelProportion));
            g.setColour(colour.darker());
            g.fillRoundedRectangle(filledBounds, 4.0f);

            // Draw a bright indicator line on top
            auto lineBounds = filledBounds.withHeight (3.0f);
            g.setColour (colour.brighter(0.4f));
            g.fillRect (lineBounds);
        }

        // Draw the slider's value as text in the center (3 lines: sign, value, unit)
        float val = (float)slider.getValue();
        juce::String signStr = (val > 0.001f) ? "+" : (val < -0.001f ? "-" : "");
        juce::String valStr = juce::String (std::abs (val), 1);
        juce::String unitStr = slider.getTextValueSuffix();

        g.setColour(juce::Colours::white.withAlpha(0.7f));
        float fontSize = juce::jmin(14.0f, height * 0.1f);
        g.setFont(fontSize);
        float lineHeight = fontSize;
        float startY = bounds.getCentreY() - (lineHeight * 3.0f) * 0.5f;

        g.drawText(signStr, bounds.getX(), (int)startY, (int)bounds.getWidth(), (int)lineHeight, juce::Justification::centred, false);
        g.drawText(valStr, bounds.getX(), (int)(startY + lineHeight), (int)bounds.getWidth(), (int)lineHeight, juce::Justification::centred, false);
        g.drawText(unitStr, bounds.getX(), (int)(startY + lineHeight * 2), (int)bounds.getWidth(), (int)lineHeight, juce::Justification::centred, false);

        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.drawRoundedRectangle(bounds, 4.f, 1.f);

    }
    else if (style == juce::Slider::LinearHorizontal)
    {
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();

        // Background
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (bounds, 4.0f);

        // Get the proportion directly from the slider's value (0.0 to 1.0)
        const float levelProportion = (float) slider.valueToProportionOfLength (slider.getValue());
        auto filledBounds = bounds;

        const bool drawFromCentre = slider.getProperties().getWithDefault ("drawFromCentre", false);
        auto colour = slider.findColour (juce::Slider::trackColourId);

        if (drawFromCentre)
        {
            const float centreProportion = 0.5f;
            g.setColour(colour);

            if (levelProportion > centreProportion) // Value is in the right half
            {
                filledBounds.removeFromLeft(bounds.getWidth() * centreProportion);
                filledBounds.removeFromRight(bounds.getWidth() * (1.0f - levelProportion));
                g.fillRoundedRectangle(filledBounds, 4.0f);

                auto lineBounds = filledBounds.withWidth(3.0f).withX(filledBounds.getRight() - 3.0f);
                g.setColour (colour.brighter(0.4f));
                g.fillRect (lineBounds);
            }
            else // Value is in the left half
            {
                filledBounds.removeFromRight(bounds.getWidth() * (1.0f - centreProportion));
                filledBounds.removeFromLeft(bounds.getWidth() * levelProportion);
                g.fillRoundedRectangle(filledBounds, 4.0f);

                auto lineBounds = filledBounds.withWidth(3.0f).withX(filledBounds.getX());
                g.setColour (colour.brighter(0.4f));
                g.fillRect (lineBounds);
            }
        }
        else // Default unipolar drawing
        {
            if (levelProportion > 0.0f)
            {
                filledBounds.removeFromRight (filledBounds.getWidth() * (1.0f - levelProportion));
                g.setColour(colour);
                g.fillRoundedRectangle(filledBounds, 4.0f);

                auto lineBounds = filledBounds.withWidth(3.0f).withX(filledBounds.getRight() - 3.0f);
                g.setColour (colour.brighter(0.4f));
                g.fillRect (lineBounds);
            }
        }

        // Draw the slider's value as text in the center
        auto text = juce::String (slider.getValue(), 2);
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.setFont(juce::jmin(18.0f, height * 0.55f));
        g.drawText(text, bounds.toNearestInt(), juce::Justification::centred, true);

        // Draw outline
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.drawRoundedRectangle(bounds, 4.f, 1.f);
    }
    else
    {
        // Fallback to the default implementation for other slider styles
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }
  }


private:
    // JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxmeKnobLookAndFeel)

};

} // namespace fxme
