/*
  ==============================================================================

    FxmeLookAndFeel.h

    FX-Mechanics LookAndFeel: rotary knobs with a centred value read-out and
    optional label, bipolar/unipolar linear bar sliders, pill-style toggle and
    text buttons, and combo boxes (plus their drop-down menus) drawn in the
    same vocabulary. Colours are read from the standard juce Slider/Button/
    ComboBox ColourIds, so callers theme it via setColour() on the widgets.

    A drop-down menu is a window of its own and cannot see the combo box that
    opened it, so it takes its colour from setAccentColour() on this object.
    Each Component here owns its own FxmeLookAndFeel, so one call in a
    component's constructor tints that component's menus to match its panel.

    Every control honours setEnabled(false): the accents desaturate and fade
    (see forState()), so a control switched off by another control — a Rate
    knob superseded by a tempo-sync division, say — reads as unavailable
    instead of looking live but ignoring the mouse. Controls also acknowledge
    the mouse: hovering lifts the accent (forHover()) and holding a button
    drops its pill onto its outline, so it reads as pushed in.

    Value arcs and bars all grow from originProportion(): the minimum for a
    normal control, or the "centralValue" / "drawFromCentre" property for a
    bipolar one. "centralValue" is given in the slider's own units, so it
    anchors correctly even on an asymmetric range such as -60…+6 dB.

    Two knob styles: classic (a disc with a light rim, a small tick, the
    value inside) and flat (setKnobStyle, or a slider's "knobStyle" property
    set to "flat"): a larger plain dark disc, thicker arcs, a bold pointer
    mark near the rim and a larger label, easier to read at small sizes.
    rotaryGeometry() gives a knob's centre and radius in either style, for a
    component that draws over its knob (a modulation ring, say).

    Faders (LinearVertical, LinearHorizontal) have a flat style too, opt-in
    (setLinearStyle, or a slider's "linearStyle" property set to "flat"): the
    flat knob's vocabulary on a straight track — a thick track, the value
    filled from its origin, a dark cap crossed by the accent, the value under
    it when "showLabel" is set. Classic stays the default, so existing panels
    whose knobs are flat keep their linear sliders as they were.

    A read-out that does not fit is never cut with "...": its font shrinks to
    a floor, then its unit goes (drawFittedText()).

    Read-outs use the slider's own getTextFromValue(), so decimal places and
    the text-value suffix carry through to knobs and faders alike, and their
    colours come from Slider::textBoxTextColourId / textBoxOutlineColourId /
    backgroundColourId rather than a hardcoded white and black. The defaults
    set in the constructor reproduce the old fixed colours exactly.

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

class FxmeLookAndFeel : public juce::LookAndFeel_V4
{
public:

  // Defined explicitly because the non-copyable macro at the bottom of the
  // class user-declares a (deleted) copy constructor, which would otherwise
  // suppress the implicit default one and break every `FxmeLookAndFeel laf;`
  // member in the codebase.
  FxmeLookAndFeel()
  {
      // Defaults so a combo box and its menu look like the rest of the panel
      // without every component having to theme them. A widget's own
      // setColour() still wins over these, so nothing existing changes.
      setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1e1e1e));
      setColour (juce::ComboBox::textColourId,       juce::Colours::white.withAlpha (0.85f));
      setColour (juce::ComboBox::outlineColourId,    juce::Colours::grey);
      setColour (juce::ComboBox::arrowColourId,      juce::Colours::white.withAlpha (0.7f));
      setColour (juce::ComboBox::buttonColourId,     juce::Colours::transparentBlack);

      setColour (juce::PopupMenu::backgroundColourId,          juce::Colour (0xff141414));
      setColour (juce::PopupMenu::textColourId,                juce::Colours::white.withAlpha (0.85f));
      setColour (juce::PopupMenu::headerTextColourId,          juce::Colours::white.withAlpha (0.6f));
      setColour (juce::PopupMenu::highlightedTextColourId,     juce::Colours::white);
      setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colours::grey.withAlpha (0.35f));

      // The slider read-outs, tracks and outlines used to be hardcoded white
      // and black. These reproduce that exactly while making them themeable:
      // a widget's own setColour() now actually reaches them, and the per-part
      // alphas below are applied as a multiplier so a caller's own alpha
      // survives.
      setColour (juce::Slider::textBoxTextColourId,    juce::Colours::white);
      setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::white);
      setColour (juce::Slider::backgroundColourId,     juce::Colours::black);

      setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));

      setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff141414));
      setColour (juce::TooltipWindow::textColourId,       juce::Colours::white.withAlpha (0.9f));
      setColour (juce::TooltipWindow::outlineColourId,    juce::Colours::transparentBlack);
  }

  /** Accent for the parts that have no widget to read a colour from.

      A ComboBox's drop-down is its own window: it never sees the combo that
      opened it, so it cannot pick up that combo's outline colour. Each
      Component in this codebase owns its own FxmeLookAndFeel, so calling
      this once in a component's constructor tints that component's menus to
      match its panel. Left alone, menus stay a neutral grey. */
  /** Draws a read-out in `area` without ever cutting it with "...": the font
      shrinks (down to minSize) until the text fits; if it still does not,
      the text loses what follows its first space (its unit: "2048 smp"
      reads "2048"); failing that it is drawn at minSize across `spill`, a
      wider box (the whole control), rather than clipped. */
  static void drawFittedText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                              juce::Rectangle<float> spill, float maxSize, float minSize, bool bold = false)
  {
      auto fontOf = [bold] (float size) { return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)); };
      auto widthOf = [] (const juce::Font& f, const juce::String& t) { return juce::GlyphArrangement::getStringWidth (f, t); };

      for (const auto& candidate : { text, text.upToFirstOccurrenceOf (" ", false, false) })
      {
          if (candidate.isEmpty())
              continue;
          for (float size = maxSize; size >= minSize - 0.01f; size -= 0.5f)
          {
              const auto f = fontOf (size);
              if (widthOf (f, candidate) <= area.getWidth())
              {
                  g.setFont (f);
                  g.drawText (candidate, area.withSizeKeepingCentre (area.getWidth(), juce::jmax (area.getHeight(), size * 1.2f)),
                              juce::Justification::centred, false);
                  return;
              }
          }
      }
      g.setFont (fontOf (minSize));
      g.drawText (text, spill.withSizeKeepingCentre (spill.getWidth(), juce::jmax (area.getHeight(), minSize * 1.2f))
                             .withY (area.getCentreY() - juce::jmax (area.getHeight(), minSize * 1.2f) * 0.5f),
                  juce::Justification::centred, false);
  }

  enum class KnobStyle { classic, flat };

  /** The style of every rotary knob drawn by this look-and-feel (a
      slider's "knobStyle" property, "classic" or "flat", overrides it). */
  void setKnobStyle (KnobStyle s) noexcept { knobStyle = s; }
  KnobStyle getKnobStyle() const noexcept  { return knobStyle; }

  enum class LinearStyle { classic, flat };

  /** The style of faders (LinearVertical and LinearHorizontal sliders); a
      slider's "linearStyle" property, "classic" or "flat", overrides it. */
  void setLinearStyle (LinearStyle s) noexcept { linearStyle = s; }
  LinearStyle getLinearStyle() const noexcept  { return linearStyle; }

  LinearStyle linearStyleFor (const juce::Slider& slider) const
  {
      const auto prop = slider.getProperties().getWithDefault ("linearStyle", juce::var()).toString();
      if (prop == "flat")    return LinearStyle::flat;
      if (prop == "classic") return LinearStyle::classic;
      return linearStyle;
  }

  /** Where a flat fader's parts sit in its bounds: the track's area (the
      cap travels along it), the track's thickness, the cap's length (along
      the track) and width (across it), and the read-out under it. */
  struct FaderGeometry
  {
      juce::Rectangle<float> track, labelArea;
      float thickness = 0.0f, capLength = 0.0f, capWidth = 0.0f;
  };

  static FaderGeometry faderGeometry (juce::Rectangle<float> bounds, bool vertical, bool showLabel)
  {
      FaderGeometry geo;
      const float labelHeight = showLabel ? juce::jlimit (0.0f, bounds.getHeight() * 0.4f, juce::jmax (bounds.getHeight() * 0.12f, 13.0f)) : 0.0f;
      geo.labelArea = bounds.removeFromBottom (labelHeight);
      geo.track = bounds;
      const float across = vertical ? bounds.getWidth() : bounds.getHeight();
      geo.thickness = juce::jlimit (3.0f, 8.0f, across * 0.18f);
      geo.capWidth = juce::jmin (across * 0.86f, 44.0f);
      geo.capLength = juce::jlimit (8.0f, 18.0f, geo.capWidth * 0.45f);
      return geo;
  }

  bool isFlatFader (const juce::Slider& slider) const
  {
      const auto style = slider.getSliderStyle();
      return (style == juce::Slider::LinearVertical || style == juce::Slider::LinearHorizontal)
          && linearStyleFor (slider) == LinearStyle::flat;
  }

  KnobStyle styleFor (const juce::Slider& slider) const
  {
      const auto prop = slider.getProperties().getWithDefault ("knobStyle", juce::var()).toString();
      if (prop == "flat")    return KnobStyle::flat;
      if (prop == "classic") return KnobStyle::classic;
      return knobStyle;
  }

  /** Where a rotary knob sits in its bounds: its centre, radius, arc
      thickness and the label's area under it. */
  struct RotaryGeometry
  {
      juce::Point<float> centre;
      float radius = 0.0f, thickness = 0.0f;
      juce::Rectangle<float> labelArea;
  };

  static RotaryGeometry rotaryGeometry (juce::Rectangle<float> bounds, bool showLabel, KnobStyle style)
  {
      RotaryGeometry geo;
      const float w = bounds.getWidth(), h = bounds.getHeight();
      float diameter, labelHeight;
      if (style == KnobStyle::flat)
      {
          // The label keeps a readable height; the knob takes the rest.
          labelHeight = showLabel ? juce::jlimit (0.0f, h * 0.45f, juce::jmax (h * 0.24f, 13.0f)) : 0.0f;
          // A margin round the knob, for anything a component draws round it.
          diameter = juce::jmax (4.0f, juce::jmin (w * 0.80f, (h - labelHeight) * 0.84f));
          geo.thickness = diameter / 9.0f;
      }
      else
      {
          diameter = 0.7f * juce::jmin (w, h);
          labelHeight = showLabel ? juce::jmin (diameter * 0.32f, h - diameter) : 0.0f;
          geo.thickness = diameter / 12.0f;
      }
      geo.radius = diameter * 0.5f;
      const float clusterHeight = diameter + labelHeight;
      geo.centre = { bounds.getCentreX(), bounds.getY() + (h - clusterHeight) * 0.5f + geo.radius };
      geo.labelArea = { bounds.getX(), geo.centre.y + geo.radius, w, labelHeight };
      return geo;
  }

  void setAccentColour (juce::Colour c)
  {
      accent = c;
      setColour (juce::PopupMenu::highlightedBackgroundColourId, c.withAlpha (0.85f));
      setColour (juce::PopupMenu::highlightedTextColourId,       c.contrasting (0.95f));
  }

  juce::Colour getAccentColour() const { return accent; }

  /** Colour to actually paint with, given the control's enabled state.

      A disabled control keeps its shape and the hue of its accent but is
      desaturated and faded, so "unavailable" reads at a glance without the
      control vanishing into the panel. Pass a larger disabledAlpha (up to 1)
      for the parts that carry the shape rather than the accent — a knob's
      disc body, for instance, should stay solid or the knob dissolves.

      Note that juce::Component::isEnabled() is false when *any* ancestor is
      disabled, so disabling a whole panel greys everything inside it. */
  static juce::Colour forState (juce::Colour c, bool isEnabled, float disabledAlpha = 0.4f)
  {
      if (isEnabled)
          return c;

      return c.withSaturation (c.getSaturation() * 0.3f)
              .withMultipliedAlpha (disabledAlpha);
  }

  /** Text alpha for a read-out or label, given the control's enabled state. */
  static float textAlpha (float enabledAlpha, bool isEnabled)
  {
      return isEnabled ? enabledAlpha : enabledAlpha * 0.4f;
  }

  /** Lift for a control the mouse is over. Deliberately slight: these panels
      carry a lot of small controls, and a loud hover on every one of them is
      noise rather than feedback. The pressed state is drawn separately (the
      pill bodies lose their offset, so the button reads as pushed in). */
  static juce::Colour forHover (juce::Colour c, bool isHighlighted)
  {
      return isHighlighted ? c.brighter (0.2f) : c;
  }

  /** Where a value arc or bar grows from, as a proportion of the slider's
      range.

      Reads the "centralValue" property first — that one is a value in the
      slider's own units, so an asymmetric bipolar range (-60…+6 dB, say)
      still anchors at the right place — then "drawFromCentre", which is the
      geometric midpoint, and otherwise falls back to the minimum for the
      usual unipolar fill. Shared by the rotary and both linear styles so a
      control looks the same however it is drawn. */
  static float originProportion (juce::Slider& slider)
  {
      if (slider.getProperties().contains ("centralValue"))
      {
          const double cv = slider.getProperties()["centralValue"];
          return (float) juce::jlimit (0.0, 1.0, slider.valueToProportionOfLength (cv));
      }

      if (slider.getProperties().getWithDefault ("drawFromCentre", false))
          return 0.5f;

      return 0.0f;
  }

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
    const bool enabled   = slider.isEnabled();
    // A disabled slider gets no mouse events, so hover can only be true here
    // while the control is live — the guard is for clarity, not safety.
    const bool hovered   = enabled && slider.isMouseOverOrDragging();

    if (styleFor (slider) == KnobStyle::flat)
    {
        drawFlatKnob (g, juce::Rectangle<int> (x, y, width, height).toFloat(), sliderPos,
                      rotaryStartAngle, rotaryEndAngle, slider, showLabel, enabled, hovered);
        return;
    }

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
    // The disc body keeps most of its opacity when disabled — it carries the
    // knob's shape, so fading it as hard as the accents would dissolve it.
    g.setColour(forState(slider.findColour(juce::Slider::rotarySliderFillColourId).brighter(2.f), enabled, 0.8f));
    g.drawEllipse(dialArea.reduced(thickness).translated(0.f,-thickness*0.12f),thickness*0.36f);
    g.setColour(forState(slider.findColour(juce::Slider::rotarySliderFillColourId), enabled, 0.9f));
    g.fillEllipse(dialArea.reduced(thickness));

    g.setColour(forHover(forState(slider.findColour(juce::Slider::thumbColourId), enabled), hovered));

    // Rectangle ?
    juce::Path dialTick;
    juce::Rectangle<float> rect(.25f*thickness,-radius+2.*thickness,.5*thickness,radius*0.2);
    dialTick.addRectangle(rect);
    g.fillPath(dialTick,juce::AffineTransform::rotation(angle).translated(centreX,centreY));

    // // Disc ?
    // juce::Rectangle<float> thumbArea(0.f,-radius+2*thickness,thickness,thickness);
    // g.fillEllipse(thumbArea.transformedBy(juce::AffineTransform::rotation(angle).translated(centreX,centreY)));

    g.setColour(forState(slider.findColour(juce::Slider::rotarySliderOutlineColourId), enabled));
    juce::Path arc1;
    arc1.addArc(centreX-diameter/2, centreY-diameter/2, diameter, diameter, rotaryStartAngle, rotaryEndAngle, true);
    g.strokePath(arc1, path);

    g.setColour(forHover(forState(slider.findColour(juce::Slider::trackColourId), enabled), hovered));
    juce::Path arc2;

    const float zeroPos = originProportion (slider);
    float zeroAngle = rotaryStartAngle + zeroPos * (rotaryEndAngle - rotaryStartAngle);

    arc2.addArc (rx, ry, diameter, diameter,
                 juce::jmin (angle, zeroAngle),
                 juce::jmax (angle, zeroAngle),
                 true);

    g.strokePath(arc2, path);

    // Draw the slider's value as text in the center
    auto text = slider.getTextFromValue(slider.getValue());
    g.setColour(slider.findColour(juce::Slider::textBoxTextColourId)
                      .withMultipliedAlpha(textAlpha(0.7f, enabled)));
    // Make font size proportional to the knob's diameter
    // Fitted inside the disc (never cut with "...").
    const float classicSize = juce::jmin (15.0f, diameter * 0.3f);
    drawFittedText (g, text, dialArea.reduced (thickness * 1.5f), dialArea.withWidth ((float) width).withX ((float) x),
                    classicSize, juce::jmin (classicSize, 8.0f));

    if (showLabel)
    {
        // Sit the label just below the knob; font scales with the knob so the
        // text grows with the available size.
        juce::Rectangle<float> nameArea ((float) x, centreY + radius, (float) width, labelHeight);
        g.setColour (slider.findColour (juce::Slider::textBoxTextColourId)
                           .withMultipliedAlpha (textAlpha (0.85f, enabled)));
        // Cap the label font so it stays readable on large knobs instead of
        // growing without bound with the diameter.
        constexpr float maxLabelFontSize = 16.0f;
        g.setFont (juce::jmin (diameter * 0.25f, labelHeight * 0.9f, maxLabelFontSize));
        g.drawText (slider.getName(), nameArea.toNearestInt(), juce::Justification::centred, true);
    }
  };

  /** A flat fader's cap must not run off its track: JUCE keeps the value's
      positions this far from each end, for the mouse as for the drawing. */
  int getSliderThumbRadius (juce::Slider& slider) override
  {
      if (isFlatFader (slider))
      {
          const bool vertical = slider.getSliderStyle() == juce::Slider::LinearVertical;
          const bool showLabel = slider.getProperties().getWithDefault ("showLabel", false);
          return juce::roundToInt (faderGeometry (slider.getLocalBounds().toFloat(), vertical, showLabel).capLength * 0.5f);
      }
      return LookAndFeel_V4::getSliderThumbRadius (slider);
  }

  juce::Slider::SliderLayout getSliderLayout (juce::Slider& slider) override
  {
      auto style = slider.getSliderStyle();

      // A flat fader travels along its track; the read-out below is drawn by us.
      if (isFlatFader (slider))
      {
          const bool showLabel = slider.getProperties().getWithDefault ("showLabel", false);
          juce::Slider::SliderLayout layout;
          layout.sliderBounds = faderGeometry (slider.getLocalBounds().toFloat(), style == juce::Slider::LinearVertical, showLabel)
                                    .track.toNearestInt();
          layout.textBoxBounds = {};
          return layout;
      }

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
    // "isDown" here is the *latched* state (the toggle is on), which is what
    // drives the two colour schemes below. The mouse being physically held is
    // shouldDrawButtonAsDown, and it moves the pill instead of recolouring it.
    auto isDown = b.getToggleState();
    const bool enabled     = b.isEnabled();
    const bool highlighted = enabled && shouldDrawButtonAsHighlighted;
    const bool pressed     = enabled && shouldDrawButtonAsDown;

    auto col = forHover(forState(b.findColour(juce::ToggleButton::tickColourId), enabled), highlighted);
    float t;
    t = w*.5f;

    // The pill body normally sits slightly below and right of its outline;
    // pressing it removes that offset, so the button reads as pushed in.
    const float bodyOffset = pressed ? 0.0f : w * 0.1f;

    if (isDown)
    {
      g.setColour(col.brighter(1.5f));
    }
      else
    {
      g.setColour(col);
    }
    g.drawRoundedRectangle(bounds.toFloat(),w*2,t);

    if (isDown)
    {
      g.setColour(col.brighter(0.7f));
      g.fillRoundedRectangle(bounds.toFloat().translated(bodyOffset,bodyOffset),w*2);
      g.setColour(col.darker(1.f));
      g.drawText(b.getButtonText(),bounds,juce::Justification::centred);
    }
    else
    {
      g.setColour(col.darker(.7f));
      g.fillRoundedRectangle(bounds.toFloat().translated(bodyOffset,bodyOffset),w*2);
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

      const bool enabled     = button.isEnabled();
      const bool highlighted = enabled && shouldDrawButtonAsHighlighted;
      const bool pressed     = enabled && shouldDrawButtonAsDown;

      // Paint with the colour JUCE resolved for us rather than re-reading
      // buttonColourId: Button::paintButton has already picked between
      // buttonColourId and buttonOnColourId for the current toggle state, so
      // taking it here is what makes an "on" TextButton colourable at all.
      auto col = forHover(forState(backgroundColour, enabled), highlighted);
      float t = w * 0.5f; // thickness

      // As on the toggle: the body loses its offset while held, so the button
      // reads as pushed in.
      const float bodyOffset = pressed ? 0.0f : w * 0.1f;

      if (pressed)
      {
          g.setColour(col.brighter(1.5f));
      }
      else
      {
          g.setColour(col);
      }
      g.drawRoundedRectangle(bounds.toFloat(), w * 2, t);

      if (pressed)
      {
          g.setColour(col.brighter(0.7f));
          g.fillRoundedRectangle(bounds.toFloat().translated(bodyOffset, bodyOffset), w * 2);
      }
      else
      {
          g.setColour(col.darker(0.7f));
          g.fillRoundedRectangle(bounds.toFloat().translated(bodyOffset, bodyOffset), w * 2);
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
    const bool enabled = slider.isEnabled();

    if (isFlatFader (slider))
    {
        drawFlatFader (g, sliderPos, slider, style == juce::Slider::LinearVertical, enabled,
                       enabled && slider.isMouseOverOrDragging());
        return;
    }

    if (style == juce::Slider::LinearBarVertical)
    {
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();

        // Background
        g.setColour (slider.findColour (juce::Slider::backgroundColourId));
        g.fillRoundedRectangle (bounds, 4.0f);

        // Get the proportion directly from the slider's value (0.0 to 1.0)
        const float levelProportion = (float) slider.valueToProportionOfLength (slider.getValue());
        auto filledBounds = bounds;

        // The bar runs between the value and its origin — the bottom for a
        // normal fader, the centre (or wherever "centralValue" lands) for a
        // bipolar one, so it can grow downwards as well as up.
        const float origin = originProportion (slider);
        const float lo = juce::jmin (origin, levelProportion);
        const float hi = juce::jmax (origin, levelProportion);

        if (hi > lo)
        {
            auto colour = forHover (forState (slider.findColour (juce::Slider::trackColourId), enabled),
                                    enabled && slider.isMouseOverOrDragging());
            // Proportion 0 is the bottom of the track, so trim the unfilled
            // part off both ends (both removals use the original height).
            filledBounds.removeFromTop    (bounds.getHeight() * (1.0f - hi));
            filledBounds.removeFromBottom (bounds.getHeight() * lo);
            g.setColour(colour.darker());
            g.fillRoundedRectangle(filledBounds, 4.0f);

            // Bright indicator line at the moving end of the bar.
            auto lineBounds = (levelProportion >= origin)
                ? filledBounds.withHeight (3.0f)
                : filledBounds.withHeight (3.0f).withY (filledBounds.getBottom() - 3.0f);
            g.setColour (colour.brighter(0.4f));
            g.fillRect (lineBounds);
        }

        // Draw the slider's value as text in the center (3 lines: sign, value, unit)
        float val = (float)slider.getValue();
        juce::String signStr = (val > 0.001f) ? "+" : (val < -0.001f ? "-" : "");
        juce::String valStr = juce::String (std::abs (val), 1);
        juce::String unitStr = slider.getTextValueSuffix();

        g.setColour(slider.findColour(juce::Slider::textBoxTextColourId)
                          .withMultipliedAlpha(textAlpha(0.7f, enabled)));
        float fontSize = juce::jmin(14.0f, height * 0.1f);
        g.setFont(fontSize);
        float lineHeight = fontSize;
        float startY = bounds.getCentreY() - (lineHeight * 3.0f) * 0.5f;

        g.drawText(signStr, bounds.getX(), (int)startY, (int)bounds.getWidth(), (int)lineHeight, juce::Justification::centred, false);
        g.drawText(valStr, bounds.getX(), (int)(startY + lineHeight), (int)bounds.getWidth(), (int)lineHeight, juce::Justification::centred, false);
        g.drawText(unitStr, bounds.getX(), (int)(startY + lineHeight * 2), (int)bounds.getWidth(), (int)lineHeight, juce::Justification::centred, false);

        g.setColour(slider.findColour(juce::Slider::textBoxOutlineColourId)
                          .withMultipliedAlpha(textAlpha(0.7f, enabled)));
        g.drawRoundedRectangle(bounds, 4.f, 1.f);

    }
    else if (style == juce::Slider::LinearHorizontal)
    {
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();

        // Background
        g.setColour (slider.findColour (juce::Slider::backgroundColourId));
        g.fillRoundedRectangle (bounds, 4.0f);

        // Get the proportion directly from the slider's value (0.0 to 1.0)
        const float levelProportion = (float) slider.valueToProportionOfLength (slider.getValue());
        auto filledBounds = bounds;

        auto colour = forHover (forState (slider.findColour (juce::Slider::trackColourId), enabled),
                                enabled && slider.isMouseOverOrDragging());

        // Same rule as the vertical bar: the fill spans value-to-origin, which
        // is the left edge for a normal fader and the centre (or "centralValue")
        // for a bipolar one.
        const float origin = originProportion (slider);
        const float lo = juce::jmin (origin, levelProportion);
        const float hi = juce::jmax (origin, levelProportion);

        if (hi > lo)
        {
            // Both removals are proportions of the original width.
            filledBounds.removeFromLeft  (bounds.getWidth() * lo);
            filledBounds.removeFromRight (bounds.getWidth() * (1.0f - hi));
            g.setColour(colour);
            g.fillRoundedRectangle(filledBounds, 4.0f);

            // Bright indicator line at the moving end of the bar.
            auto lineBounds = (levelProportion >= origin)
                ? filledBounds.withWidth(3.0f).withX(filledBounds.getRight() - 3.0f)
                : filledBounds.withWidth(3.0f).withX(filledBounds.getX());
            g.setColour (colour.brighter(0.4f));
            g.fillRect (lineBounds);
        }

        // Draw the slider's value as text in the center. Uses the slider's own
        // formatting (decimal places, text-value suffix, or a getTextFromValue
        // lambda) exactly as the rotary does — hardcoding two decimals here
        // meant a fader read "0.50" where its knob read "50 %".
        auto text = slider.getTextFromValue (slider.getValue());
        g.setColour(slider.findColour(juce::Slider::textBoxTextColourId)
                          .withMultipliedAlpha(textAlpha(0.7f, enabled)));
        const float barSize = juce::jmin (18.0f, height * 0.55f);
        drawFittedText (g, text, bounds.reduced (4.0f, 0.0f), bounds, barSize, juce::jmin (barSize, 8.0f));

        // Draw outline
        g.setColour(slider.findColour(juce::Slider::textBoxOutlineColourId)
                          .withMultipliedAlpha(textAlpha(0.7f, enabled)));
        g.drawRoundedRectangle(bounds, 4.f, 1.f);
    }
    else
    {
        // Fallback to the default implementation for other slider styles
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }
  }

  //============================================================================
  // Labels. Faithful to the JUCE default apart from the disabled fade, which
  // matches the rest of this look-and-feel (textAlpha) instead of a flat half
  // alpha — so a caption greyed out alongside its control fades with it.

  void drawLabel (juce::Graphics& g, juce::Label& label) override
  {
      g.fillAll (label.findColour (juce::Label::backgroundColourId));

      const bool enabled = label.isEnabled();

      if (! label.isBeingEdited())
      {
          const auto font = getLabelFont (label);

          g.setColour (label.findColour (juce::Label::textColourId)
                            .withMultipliedAlpha (textAlpha (1.0f, enabled)));
          g.setFont (font);

          const auto textArea = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());

          g.drawFittedText (label.getText(), textArea, label.getJustificationType(),
                            juce::jmax (1, (int) ((float) textArea.getHeight() / font.getHeight())),
                            label.getMinimumHorizontalScale());

          g.setColour (label.findColour (juce::Label::outlineColourId)
                            .withMultipliedAlpha (textAlpha (1.0f, enabled)));
      }
      else if (enabled)
      {
          g.setColour (label.findColour (juce::Label::outlineColourId));
      }

      g.drawRect (label.getLocalBounds());
  }

  //============================================================================
  // Tooltips. Same dark panel and accent hairline as the drop-down menus, so
  // the two floating surfaces match.
  //
  // Note that a tooltip only ever appears if something in the application owns
  // a juce::TooltipWindow, and it is drawn by *that window's* look-and-feel —
  // which for a desktop window is the default one unless it is pointed at this
  // object explicitly (tooltipWindow.setLookAndFeel (&laf)).

  void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
  {
      const auto bounds = juce::Rectangle<float> ((float) width, (float) height);

      g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
      g.fillRoundedRectangle (bounds, 5.0f);

      g.setColour (accent.withAlpha (0.55f));
      g.drawRoundedRectangle (bounds.reduced (0.5f), 5.0f, 1.0f);

      // The bounds were sized by getTooltipBounds() against the default
      // tooltip font, so the text is laid out to fit rather than re-wrapped.
      g.setColour (findColour (juce::TooltipWindow::textColourId));
      g.setFont (juce::Font (juce::FontOptions (13.0f)));
      g.drawFittedText (text,
                        juce::Rectangle<int> (width, height).reduced (7, 3),
                        juce::Justification::centredLeft,
                        juce::jmax (1, height / 13),
                        0.9f);
  }

  //============================================================================
  // Combo boxes. Drawn in the same vocabulary as the pill buttons — the same
  // inset, the same corner radius, an accent outline over a dark body — so a
  // combo sitting next to a button belongs to the same panel.

  /** The pill a combo box is drawn inside: its bounds less the standard inset. */
  static juce::Rectangle<float> comboBody (float width, float height)
  {
      const float w = juce::jmin (width, height) * 0.1f;
      return juce::Rectangle<float> (0.0f, 0.0f, width, height).reduced (w);
  }

  /** The chevron's zone, taken off the right of the body. Takes the body by
      value, so the caller keeps theirs intact. */
  static juce::Rectangle<float> comboArrowZone (juce::Rectangle<float> body)
  {
      return body.removeFromRight (juce::jmin (body.getWidth() * 0.3f, body.getHeight() * 1.2f));
  }

  void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                     int buttonX, int buttonY, int buttonW, int buttonH,
                     juce::ComboBox& box) override
  {
      // The arrow zone is ours to place (see comboArrowZone), so the button
      // rectangle JUCE suggests is not used.
      juce::ignoreUnused (buttonX, buttonY, buttonW, buttonH);

      const bool enabled = box.isEnabled();
      const bool hovered = enabled && box.isMouseOver (true);

      const auto bounds = comboBody ((float) width, (float) height);
      const float w      = juce::jmin ((float) width, (float) height) * 0.1f;
      const float corner = w * 2.0f;

      g.setColour (forState (box.findColour (juce::ComboBox::backgroundColourId), enabled, 0.9f));
      g.fillRoundedRectangle (bounds, corner);

      // An open menu reads like a held button.
      auto outline = forHover (forState (box.findColour (juce::ComboBox::outlineColourId), enabled), hovered);
      if (isButtonDown)
          outline = outline.brighter (0.5f);

      g.setColour (outline);
      g.drawRoundedRectangle (bounds, corner, juce::jmax (1.0f, w * 0.5f));

      const auto arrowZone = comboArrowZone (bounds);
      const float cx = arrowZone.getCentreX();
      const float cy = arrowZone.getCentreY();
      const float s  = juce::jmin (arrowZone.getWidth(), arrowZone.getHeight()) * 0.22f;

      juce::Path chevron;
      chevron.startNewSubPath (cx - s, cy - s * 0.5f);
      chevron.lineTo          (cx,     cy + s * 0.6f);
      chevron.lineTo          (cx + s, cy - s * 0.5f);

      g.setColour (forHover (forState (box.findColour (juce::ComboBox::arrowColourId), enabled), hovered));
      g.strokePath (chevron, juce::PathStrokeType (juce::jmax (1.0f, s * 0.35f),
                                                   juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
  }

  juce::Font getComboBoxFont (juce::ComboBox& box) override
  {
      // Sized against the inset body rather than the full height, so the text
      // does not crowd the pill.
      return juce::Font (juce::FontOptions (juce::jmin (15.0f, (float) box.getHeight() * 0.6f)));
  }

  void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
  {
      auto body = comboBody ((float) box.getWidth(), (float) box.getHeight());
      const float arrowWidth = comboArrowZone (body).getWidth();
      const float padding    = body.getHeight() * 0.25f;

      label.setBounds (body.withTrimmedRight (arrowWidth)
                           .withTrimmedLeft (padding)
                           .toNearestInt());
      label.setFont (getComboBoxFont (box));
  }

  //============================================================================
  // The drop-down a combo box opens. It is a window of its own with no handle
  // on the combo that spawned it, so it takes its colour from the accent set
  // on this look-and-feel (see setAccentColour).

  void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
  {
      const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);

      g.setColour (findColour (juce::PopupMenu::backgroundColourId));
      g.fillRoundedRectangle (bounds, 4.0f);

      g.setColour (accent.withAlpha (0.55f));
      g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);
  }

  juce::Font getPopupMenuFont() override
  {
      return juce::Font (juce::FontOptions (15.0f));
  }

  void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                          bool isSeparator, bool isActive, bool isHighlighted,
                          bool isTicked, bool hasSubMenu,
                          const juce::String& text,
                          const juce::String& shortcutKeyText,
                          const juce::Drawable* icon,
                          const juce::Colour* textColourToUse) override
  {
      if (isSeparator)
      {
          auto r = area.reduced (6, 0);
          r.removeFromTop (juce::roundToInt (((float) r.getHeight() * 0.5f) - 0.5f));

          g.setColour (findColour (juce::PopupMenu::textColourId).withAlpha (0.25f));
          g.fillRect (r.removeFromTop (1));
          return;
      }

      auto textColour = (textColourToUse == nullptr ? findColour (juce::PopupMenu::textColourId)
                                                    : *textColourToUse);
      auto r = area.reduced (2, 1);

      if (isHighlighted && isActive)
      {
          // Rounded, like every other filled shape here.
          g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
          g.fillRoundedRectangle (r.toFloat(), 3.0f);
          g.setColour (findColour (juce::PopupMenu::highlightedTextColourId));
      }
      else
      {
          g.setColour (textColour.withMultipliedAlpha (isActive ? 1.0f : 0.4f));
      }

      r.reduce (juce::jmin (6, area.getWidth() / 20), 0);

      auto font = getPopupMenuFont();
      const auto maxFontHeight = (float) r.getHeight() / 1.3f;
      if (font.getHeight() > maxFontHeight)
          font.setHeight (maxFontHeight);
      g.setFont (font);

      auto iconArea = r.removeFromLeft (juce::roundToInt (maxFontHeight)).toFloat();

      if (icon != nullptr)
      {
          icon->drawWithin (g, iconArea,
                            juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                            1.0f);
          r.removeFromLeft (juce::roundToInt (maxFontHeight * 0.5f));
      }
      else if (isTicked)
      {
          // The tick carries the accent when the row is not highlighted —
          // that is what makes the current selection findable at a glance.
          const auto tickColour = (isHighlighted && isActive)
              ? findColour (juce::PopupMenu::highlightedTextColourId)
              : accent.brighter (0.2f);

          auto tick = getTickShape (1.0f);
          g.setColour (tickColour);
          g.fillPath (tick, tick.getTransformToScaleToFit (iconArea.reduced (iconArea.getWidth() / 5.0f, 0.0f), true));

          // Restore the text colour the row was going to use.
          g.setColour ((isHighlighted && isActive) ? findColour (juce::PopupMenu::highlightedTextColourId)
                                                   : textColour.withMultipliedAlpha (isActive ? 1.0f : 0.4f));
      }

      if (hasSubMenu)
      {
          const auto arrowH = 0.6f * getPopupMenuFont().getAscent();
          const auto x      = static_cast<float> (r.removeFromRight ((int) arrowH).getX());
          const auto halfH  = static_cast<float> (r.getCentreY());

          juce::Path path;
          path.startNewSubPath (x, halfH - arrowH * 0.5f);
          path.lineTo (x + arrowH * 0.6f, halfH);
          path.lineTo (x, halfH + arrowH * 0.5f);

          g.strokePath (path, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
      }

      r.removeFromRight (3);
      g.drawFittedText (text, r, juce::Justification::centredLeft, 1);

      if (shortcutKeyText.isNotEmpty())
      {
          auto f2 = font;
          f2.setHeight (f2.getHeight() * 0.75f);
          f2.setHorizontalScale (0.95f);
          g.setFont (f2);
          g.drawText (shortcutKeyText, r, juce::Justification::centredRight, true);
      }
  }


private:
  KnobStyle knobStyle = KnobStyle::classic;
  LinearStyle linearStyle = LinearStyle::classic;

  /** The flat fader: a thick track, the value filled from its origin (the
      bottom or left, or "centralValue" for a bipolar one), a dark cap crossed
      by the accent at `pos` (pixels, as JUCE computed it), the value below. */
  void drawFlatFader (juce::Graphics& g, float pos, juce::Slider& slider, bool vertical, bool enabled, bool hovered)
  {
      const bool showLabel = slider.getProperties().getWithDefault ("showLabel", false);
      const auto geo = faderGeometry (slider.getLocalBounds().toFloat(), vertical, showLabel);
      const auto area = geo.track;
      const float t = geo.thickness;

      // The track runs between the cap's two extreme positions.
      const float inset = geo.capLength * 0.5f;
      const auto line = vertical
          ? juce::Line<float> (area.getCentreX(), area.getBottom() - inset, area.getCentreX(), area.getY() + inset)
          : juce::Line<float> (area.getX() + inset, area.getCentreY(), area.getRight() - inset, area.getCentreY());
      g.setColour (forState (slider.findColour (juce::Slider::rotarySliderOutlineColourId), enabled));
      g.drawLine (line, t);

      // The value, from its origin.
      const double originValue = slider.proportionOfLengthToValue (originProportion (slider));
      const float origin = (float) slider.getPositionOfValue (originValue);
      if (std::abs (pos - origin) > 0.5f)
      {
          const auto fill = vertical ? juce::Line<float> (area.getCentreX(), origin, area.getCentreX(), pos)
                                     : juce::Line<float> (origin, area.getCentreY(), pos, area.getCentreY());
          g.setColour (forHover (forState (slider.findColour (juce::Slider::trackColourId), enabled), hovered));
          g.drawLine (fill, t);
      }

      // The cap: dark, crossed by the accent.
      const auto centre = vertical ? juce::Point<float> (area.getCentreX(), pos) : juce::Point<float> (pos, area.getCentreY());
      const auto cap = vertical ? juce::Rectangle<float> (geo.capWidth, geo.capLength).withCentre (centre)
                                : juce::Rectangle<float> (geo.capLength, geo.capWidth).withCentre (centre);
      const float corner = juce::jmin (cap.getWidth(), cap.getHeight()) * 0.3f;
      g.setColour (forState (slider.findColour (juce::Slider::rotarySliderFillColourId), enabled, 0.9f));
      g.fillRoundedRectangle (cap, corner);
      g.setColour (forState (slider.findColour (juce::Slider::rotarySliderOutlineColourId), enabled));
      g.drawRoundedRectangle (cap, corner, 1.0f);
      const float mark = juce::jmax (2.0f, t * 0.6f);
      g.setColour (forHover (forState (slider.findColour (juce::Slider::thumbColourId), enabled), hovered));
      if (vertical)
          g.drawLine (cap.getX() + corner, centre.y, cap.getRight() - corner, centre.y, mark);
      else
          g.drawLine (centre.x, cap.getY() + corner, centre.x, cap.getBottom() - corner, mark);

      // The value under it, fitted (never cut with "...").
      if (showLabel && geo.labelArea.getHeight() > 4.0f)
      {
          g.setColour (slider.findColour (juce::Slider::textBoxTextColourId).withMultipliedAlpha (textAlpha (0.85f, enabled)));
          const float size = juce::jmin (13.0f, geo.labelArea.getHeight() * 0.9f);
          drawFittedText (g, slider.getTextFromValue (slider.getValue()), geo.labelArea, geo.labelArea, size, juce::jmin (size, 7.5f));
      }
  }

  /** The flat knob: a plain dark disc, a thick value arc over its track, a
      bold pointer mark near the rim, the value in the middle, a large
      label under it. */
  void drawFlatKnob (juce::Graphics& g, juce::Rectangle<float> bounds, float sliderPos,
                     float startAngle, float endAngle, juce::Slider& slider,
                     bool showLabel, bool enabled, bool hovered)
  {
      const auto geo = rotaryGeometry (bounds, showLabel, KnobStyle::flat);
      const auto c = geo.centre;
      const float r = geo.radius, t = geo.thickness;
      const float arcR = r - t * 0.5f;
      const float angle = startAngle + sliderPos * (endAngle - startAngle);
      const juce::PathStrokeType stroke { t, juce::PathStrokeType::curved, juce::PathStrokeType::rounded };

      // Disc.
      g.setColour (forState (slider.findColour (juce::Slider::rotarySliderFillColourId), enabled, 0.9f));
      g.fillEllipse (juce::Rectangle<float> (2.0f * (r - t * 1.25f), 2.0f * (r - t * 1.25f)).withCentre (c));

      // Track, then the value from its origin.
      juce::Path track;
      track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
      g.setColour (forState (slider.findColour (juce::Slider::rotarySliderOutlineColourId), enabled));
      g.strokePath (track, stroke);

      const float originAngle = startAngle + originProportion (slider) * (endAngle - startAngle);
      if (std::abs (angle - originAngle) > 1.0e-3f)
      {
          juce::Path value;
          value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, juce::jmin (angle, originAngle), juce::jmax (angle, originAngle), true);
          g.setColour (forHover (forState (slider.findColour (juce::Slider::trackColourId), enabled), hovered));
          g.strokePath (value, stroke);
      }

      // Pointer: a bold mark from the disc's edge inwards.
      g.setColour (forHover (forState (slider.findColour (juce::Slider::thumbColourId), enabled), hovered));
      g.drawLine ({ c.getPointOnCircumference (r * 0.50f, angle), c.getPointOnCircumference (r - t * 1.5f, angle) },
                  juce::jmax (2.0f, t * 0.9f));

      // Value in the middle.
      // Value in the middle, fitted inside the disc (never cut with "...").
      const float valueFont = juce::jmin (13.0f, r * 0.42f);
      if (valueFont >= 6.0f)
      {
          g.setColour (slider.findColour (juce::Slider::textBoxTextColourId).withMultipliedAlpha (textAlpha (0.8f, enabled)));
          const float inner = 2.0f * (r - t * 1.3f) * 0.94f;
          drawFittedText (g, slider.getTextFromValue (slider.getValue()),
                          juce::Rectangle<float> (inner, valueFont * 1.2f).withCentre (c),
                          juce::Rectangle<float> (2.0f * r, valueFont * 1.2f).withCentre (c),
                          valueFont, juce::jmin (valueFont, 7.5f));
      }

      if (showLabel && geo.labelArea.getHeight() > 4.0f)
      {
          g.setColour (slider.findColour (juce::Slider::textBoxTextColourId).withMultipliedAlpha (textAlpha (0.9f, enabled)));
          g.setFont (juce::Font (juce::FontOptions (juce::jmin (14.5f, geo.labelArea.getHeight() * 0.92f), juce::Font::bold)));
          g.drawText (slider.getName(), geo.labelArea.toNearestInt(), juce::Justification::centred, true);
      }
  }

    juce::Colour accent { juce::Colours::grey };

    // A LookAndFeel that outlives the widgets pointing at it (or the reverse)
    // is a classic JUCE crash; the detector turns that into a debug assertion
    // at shutdown instead of a dangling vtable at paint time.
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxmeLookAndFeel)
};

} // namespace fxme
