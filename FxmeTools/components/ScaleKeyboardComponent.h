/*
  ------------------------------------------------------------------------------
    ScaleKeyboardComponent.h

    A read-only piano strip that shows, at a glance, what a scale-aware MIDI
    plugin is doing: which keys belong to the current scale, where its root is,
    which notes are being played in, and which notes each voice is sounding.

    It is a display, not a keyboard: it takes no mouse input and generates no
    MIDI. juce::MidiKeyboardComponent is the thing to reach for when you want a
    playable keyboard; this is for showing state underneath one.

    Usage:

        fxme::ScaleKeyboardComponent keys;          // 7 octaves from C1
        keys.colourForVoice = [] (int v) { return myPalette (v); };
        addAndMakeVisible (keys);

        // once per frame, or whenever the state changes
        keys.update (scale.getNotes(), scale.getRootNote(), playing, held);

    The four layers it draws, from the bottom up:

      scale notes    non-scale keys keep their full white or black, scale keys
                     are dimmed, so the scale reads as the *quieter* set. Pass
                     an empty array to switch the whole layer off.
      root note      a small dot on every octave's root.
      playing notes  filled in the colour of the voice sounding them, which is
                     what colourForVoice is for. Without a callback every voice
                     draws white.
      input notes    outlined, for keys held down on the way in.

    Promoted from TeAr's local KeyboardComponent, which had the arpeggiator
    palette compiled into it. Nothing here knows what a voice is: it is an
    integer the caller gives meaning to.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <functional>

namespace fxme
{

class ScaleKeyboardComponent : public juce::Component
{
public:
    /** A note currently sounding, and which voice is sounding it. `voice` is
        handed straight back to colourForVoice and means nothing here. */
    struct PlayingNote
    {
        int midiNote = -1;
        int voice    = 0;
    };

    /** Every colour the strip uses. The defaults are the ones this was drawn
        with before it moved here, so leaving them alone changes nothing. */
    struct Colours
    {
        juce::Colour background   { juce::Colours::black.withAlpha (0.8f) };
        juce::Colour border       { juce::Colours::white.withAlpha (0.35f) };
        juce::Colour whiteKey     { juce::Colours::white.withAlpha (0.92f) };
        juce::Colour blackKey     { juce::Colours::black.withAlpha (0.88f) };
        juce::Colour whiteInScale { 0xFFAAAAAA };
        juce::Colour blackInScale { 0xFF555555 };
        juce::Colour marker       { juce::Colours::red.brighter (0.2f) };  // root dot, input outline
        juce::Colour voiceless    { juce::Colours::white };                // no colourForVoice set
    };

    explicit ScaleKeyboardComponent (int octaveCount = 7, int firstOctave = 1)
        : numOctaves (juce::jmax (1, octaveCount)),
          startOctave (firstOctave),
          startMidi (12 + firstOctave * 12)
    {
        setOpaque (false);
        setInterceptsMouseClicks (false, false);
    }

    /** Colour for a voice index. Left unset, playing notes are drawn in
        `colours.voiceless`. */
    std::function<juce::Colour (int voice)> colourForVoice;

    void setColours (Colours c)   { colours = c; repaint(); }
    const Colours& getColours() const noexcept { return colours; }

    /** The whole visible state in one call, so a caller polling at frame rate
        triggers one repaint rather than four.

        @param newScaleNotes  semitones of the scale; empty switches the layer off
        @param newRootNote    semitone of the root, or -1 for none
        @param newPlaying     notes currently sounding, with their voice
        @param newInputNotes  notes held down on the way in */
    void update (const juce::Array<int>&         newScaleNotes,
                 int                             newRootNote,
                 const juce::Array<PlayingNote>& newPlaying,
                 const juce::Array<int>&         newInputNotes = {})
    {
        scaleNotes   = newScaleNotes;
        rootNote     = newRootNote;
        playingNotes = newPlaying;
        inputNotes   = newInputNotes;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const int   numWhiteKeys = numOctaves * 7;
        const float h   = (float) getHeight();
        const float wkW = (float) getWidth() / (float) numWhiteKeys;
        const float bkW = wkW * 0.62f;
        const float bkH = h * 0.62f;
        const float gap = 0.8f;

        g.setColour (colours.background);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

        const bool hasScale = ! scaleNotes.isEmpty();

        auto isInScale = [this] (int midiNote)
        {
            const int semi = midiNote % 12;
            return std::any_of (scaleNotes.begin(), scaleNotes.end(),
                                [semi] (int n) { return (n % 12) == semi; });
        };

        auto isRoot = [this] (int midiNote)
        {
            return rootNote >= 0 && (midiNote % 12) == (rootNote % 12);
        };

        auto playColour = [this] (int midiNote)
        {
            for (const auto& n : playingNotes)
                if (n.midiNote == midiNote)
                    return colourForVoice ? colourForVoice (n.voice) : colours.voiceless;

            return juce::Colours::transparentBlack;
        };

        // --- White keys ------------------------------------------------------
        for (int wk = 0; wk < numWhiteKeys; ++wk)
        {
            const int   oct      = wk / 7;
            const int   midiNote = startMidi + oct * 12 + whiteToSemitone[wk % 7];
            const float x        = (float) wk * wkW;
            const juce::Rectangle<float> rect (x + gap, 1.0f, wkW - gap * 2.0f, h - 2.0f);

            g.setColour (hasScale && isInScale (midiNote) ? colours.whiteInScale
                                                          : colours.whiteKey);
            g.fillRoundedRectangle (rect, 2.0f);

            if (const auto pc = playColour (midiNote); pc.getAlpha() > 0)
            {
                g.setColour (pc.withAlpha (0.75f));
                g.fillRoundedRectangle (rect, 2.0f);
            }

            if (inputNotes.contains (midiNote))
            {
                g.setColour (colours.marker);
                g.drawRoundedRectangle (rect.reduced (2.5f), 2.0f, 2.0f);
            }

            if (isRoot (midiNote))
            {
                const float dotR = juce::jmin (4.0f, wkW * 0.25f);
                g.setColour (colours.marker);
                g.fillEllipse (rect.getCentreX() - dotR,
                               rect.getBottom() - dotR * 2.0f - 2.0f,
                               dotR * 2.0f, dotR * 2.0f);
            }

            // Octave label on each C.
            if (wk % 7 == 0)
            {
                g.setColour (juce::Colours::darkgrey.darker());
                g.setFont (9.0f);
                g.drawText ("C" + juce::String (startOctave + oct),
                            x, h - 14.0f, wkW, 12.0f, juce::Justification::centred);
            }
        }

        // --- Black keys ------------------------------------------------------
        for (int oct = 0; oct < numOctaves; ++oct)
        {
            for (int bi = 0; bi < 5; ++bi)
            {
                const int   midiNote = startMidi + oct * 12 + blackSemitones[bi];
                const float cx       = (float) (oct * 7 + blackCentreWhite[bi]) * wkW;
                const juce::Rectangle<float> rect (cx - bkW * 0.5f, 1.0f, bkW, bkH);

                g.setColour (hasScale && isInScale (midiNote) ? colours.blackInScale
                                                              : colours.blackKey);
                g.fillRoundedRectangle (rect, 2.0f);

                if (const auto pc = playColour (midiNote); pc.getAlpha() > 0)
                {
                    g.setColour (pc.withAlpha (0.85f));
                    g.fillRoundedRectangle (rect, 2.0f);
                }

                if (inputNotes.contains (midiNote))
                {
                    g.setColour (colours.marker);
                    g.drawRoundedRectangle (rect.reduced (1.5f), 2.0f, 1.5f);
                }

                if (isRoot (midiNote))
                {
                    const float dotR = juce::jmin (3.0f, bkW * 0.25f);
                    g.setColour (colours.marker);
                    g.fillEllipse (rect.getCentreX() - dotR,
                                   rect.getBottom() - dotR * 2.0f - 1.0f,
                                   dotR * 2.0f, dotR * 2.0f);
                }
            }
        }

        g.setColour (colours.border);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

private:
    static constexpr int whiteToSemitone[7]  = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int blackSemitones[5]   = { 1, 3, 6, 8, 10 };
    static constexpr int blackCentreWhite[5] = { 1, 2, 4, 5, 6 };

    const int numOctaves;
    const int startOctave;
    const int startMidi;

    Colours                  colours;
    juce::Array<int>         scaleNotes;
    int                      rootNote { -1 };
    juce::Array<PlayingNote> playingNotes;
    juce::Array<int>         inputNotes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaleKeyboardComponent)
};

} // namespace fxme
