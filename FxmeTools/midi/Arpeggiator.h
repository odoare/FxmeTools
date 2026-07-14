/*
  ------------------------------------------------------------------------------
    Arpeggiator.h

    Text-pattern MIDI arpeggiator engine (the core of the TeAr plugin). Moved
    here from the CppMusicTools repository, now living in the `fxme` namespace.

    Depends on juce_core and juce_audio_basics.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "MidiTools.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <vector>

namespace fxme
{

/**
    A base class for creating MIDI arpeggiators.

    This class takes a Chord, an octave, and a pattern string to generate
    a sequence of MIDI notes. The getNext() method processes the pattern
    and returns MIDI messages for note-on and note-off events.

    The pattern string consists of characters that define the arpeggio's behavior at each step: 
    - '1' to '7': Plays a specific degree of the chord/scale (1=fundamental, 2=2nd, ..., 6=sixth).
      - The `playNoteOff` property determines behavior for absent degrees ("Off", "Next", "Previous").
    - '_': Sustains the previously played note.
    - '.': A rest; no note is played.
    - '+': Plays the next degree in the chord (e.g., from 1 to 2).
    - '-': Plays the previous degree in the chord (e.g., from 2 to 1).
    - '?': Plays a random, valid note from the current chord.
    - '"' or '=': Repeats the last played degree.
    - 'pN': Plays the next note with probability N×10% (N is 1–9). On a failed roll the step becomes a rest. Example: "p5 1" plays the root 50% of the time.
    - '#' (Sharp): Pitches the next note up by one semitone. This is a local effect. Example: "#0"
    - 'b' (Flat): Pitches the next note down by one semitone. This is a local effect. Example: "b0"

    Velocity Modifiers (prefixed to a note command):
    - 'vN': Sets velocity for the next note only. N is a digit from 1-8 (16-127). Example: "v80"
    - 'VN': Sets velocity globally until the next 'V' command. N is a digit from 1-8. Example: "V40"
      - v1/V1=16, v2/V2=32, v3/V3=48, v4/V4=64, v5/V5=80, v6/V6=96, v7/V7=112, v8/V8=127.

    Octave Modifiers (prefixed to a note command):
    - 'oN': Sets octave for the next note only. N is a digit from 0-7. Example: "o30"
    - 'o+': Increases octave by one for the next note only. Example: "o+0"
    - 'o-': Decreases octave by one for the next note only. Example: "o-0"
    - 'ON', 'O+', 'O-': Same as above, but sets the octave globally until the next 'O' command.

    Note: Octave modifiers are prefixes. "o-o-" means "decrease octave, then decrease octave again".
    To decrease the octave and then play the previous degree, you would use "o--".
*/
class Arpeggiator
{
public:
    /**
        Default constructor.
        Initializes with a default C Major chord, a simple pattern, and a base octave.
    */
    Arpeggiator()
        : chord(MidiTools::Chord("CM")), pattern("012"), octave(baseOctave)
    {
    }

    /**
        Constructs an Arpeggiator.
        @param initialChord The chord to be arpeggiated.
        @param arpPattern The string pattern defining the arpeggio.
        @param baseOctave The starting MIDI octave.
    */
    Arpeggiator(const MidiTools::Chord& initialChord, const juce::String& arpPattern, int baseOctave)
        : chord(initialChord), pattern(arpPattern), octave(baseOctave)
    {
    }

    virtual ~Arpeggiator() = default;

    /**
        Call this before playback to set the sample rate.
        @param rate The host's sample rate.
    */
    void prepareToPlay(double rate)
    {
        sampleRate = rate;
        updateSamplesPerNote();
    }

    /**
        Generates MIDI events for the current block of audio samples.
        @param numSamples The number of samples in the current audio block.
        @return A juce::MidiBuffer containing any events generated during this block.
    */    
    juce::MidiBuffer processBlock(int numSamples, int midiChannel = 1)
    {
        juce::MidiBuffer generatedMidi;
        if (sampleRate <= 0.0 || samplesPerNote <= 0.0 || pattern.isEmpty())
            return generatedMidi;
        if (midiChannel < 1 || midiChannel > 16) midiChannel = 1;

        int time = 0;
        while (time < numSamples)
        {
            if (samplesUntilNextNote <= 0.0)
            {
                generatedMidi.addEvents(getNext(midiChannel), 0, -1, time);
                // Use 'while' to handle cases where the block size is larger than the note duration.
                while (samplesUntilNextNote <= 0.0)
                    samplesUntilNextNote += samplesPerNote;
            }
 
            // Ensure we always advance time, even if samplesUntilNextNote is 0.
            const int samplesToAdvance = (int)std::ceil(samplesUntilNextNote);
            const int samplesThisStep = juce::jmin(numSamples - time, juce::jmax(1, samplesToAdvance));
 
            time += samplesThisStep;
            samplesUntilNextNote -= samplesThisStep;
        }
        return generatedMidi;
    }

private:
    /**
        Processes the next step in the arpeggio pattern and returns MIDI messages.
        @return A juce::MidiBuffer containing note-on and/or note-off messages.
    */
    juce::MidiBuffer getNext(int midiChannel)
    {
        juce::MidiBuffer midiBuffer;
        int samplePosition = 0; // All events happen at the start of the block

        if (pattern.isEmpty())
            return midiBuffer;


        // --- 2. Parse the pattern using a robust loop ---
        int noteToPlay = -1;
        int currentDegreeIndex = lastPlayedDegreeIndex;
        int semitoneOffset = 0; // For local sharp/flat modifiers
        int localVelocity = -1; // For local velocity modifier
        int localOctave = -1;   // For local octave modifier
        bool noteCommandFound = false;
        bool shouldUpdateLastDegree = true;
        bool forcedRest = false;
        bool isSustain = false;
        bool isRest = false;
        bool hasProbability = false;
        
        // This outer loop ensures we will always find a note command, even if we have to
        // wrap around the pattern string after processing prefixes at the end.
        for (int i = 0; i < pattern.length() * 2 && !noteCommandFound; ++i)
        {
            // This inner loop consumes any number of prefix commands.
            while (true)
            {
                char command = pattern[pos];
                
                if (command == 'o' || command == 'O')
                {
                    currentStepIndex = getStepForPatternIndex(pos);
                    pos = (pos + 1) % pattern.length(); // Consume 'o' or 'O'
                    char octaveCommand = pattern[pos];
                    pos = (pos + 1) % pattern.length(); // Consume octave value
                    
                    int currentStepOctave = (localOctave != -1) ? localOctave : octave;
                    int targetOctave = currentStepOctave;
                    if (octaveCommand == '+') targetOctave = juce::jmin(7, currentStepOctave + 1);
                    else if (octaveCommand == '-') targetOctave = juce::jmax(0, currentStepOctave - 1);
                    else if (octaveCommand == '?') targetOctave = juce::Random::getSystemRandom().nextInt(9) + 1;
                    else if (juce::CharacterFunctions::isDigit(octaveCommand)) targetOctave = octaveCommand - '0';
                    
                    if (command == 'o') localOctave = targetOctave;
                    else octave = targetOctave;
                }
                else if (command == 'v' || command == 'V')
                {
                    currentStepIndex = getStepForPatternIndex(pos);
                    pos = (pos + 1) % pattern.length(); // Consume 'v' or 'V'
                    char velocityValueChar = pattern[pos];
                    pos = (pos + 1) % pattern.length(); // Consume velocity value
                    
                    int velocityLevel = 0;
                    if (juce::CharacterFunctions::isDigit(velocityValueChar))
                        velocityLevel = velocityValueChar - '0';
                    else if (velocityValueChar == '?')
                        velocityLevel = juce::Random::getSystemRandom().nextInt(9) + 1;
                    if (velocityLevel > 0)
                    {
                        int velocity = juce::jmin(127, velocityLevel * 16);
                        if (command == 'v') localVelocity = velocity;
                        else globalVelocity = velocity;
                    }
                }
                else if (command == 'p')
                {
                    currentStepIndex = getStepForPatternIndex(pos);
                    pos = (pos + 1) % pattern.length(); // consume 'p'
                    char probChar = pattern[pos];
                    pos = (pos + 1) % pattern.length(); // consume digit

                    bool rollSucceeded = true;
                    if (juce::CharacterFunctions::isDigit(probChar))
                    {
                        int level = probChar - '0';
                        rollSucceeded = juce::Random::getSystemRandom().nextFloat() < level * 0.1f;
                    }

                    // Skip spaces between 'pN' and '(' to support "p5 (12):(34)" style
                    int groupCheckPos = pos;
                    while (groupCheckPos < pattern.length() && pattern[groupCheckPos] == ' ')
                        ++groupCheckPos;

                    if (groupCheckPos < pattern.length() && pattern[groupCheckPos] == '(')
                    {
                        // Group probability: pN (success)[:( fallback)]
                        pos = (groupCheckPos + 1) % pattern.length(); // skip spaces + '('
                        if (rollSucceeded)
                        {
                            scopeStack.push_back ({ octave, globalVelocity });
                            probGroupStack.push_back ({ (int) scopeStack.size() - 1 });
                        }
                        else
                        {
                            skipToMatchingParen(); // advance pos past matching ')'
                            if (!pattern.isEmpty() && pattern[pos] == ':')
                            {
                                pos = (pos + 1) % pattern.length(); // consume ':'
                                if (!pattern.isEmpty() && pattern[pos] == '(')
                                {
                                    pos = (pos + 1) % pattern.length(); // consume '('
                                    scopeStack.push_back ({ octave, globalVelocity });
                                    probGroupStack.push_back ({ (int) scopeStack.size() - 1 });
                                }
                                // else single-char fallback: leave pos there for note command loop
                            }
                            else
                            {
                                forcedRest = true; // no fallback → rest
                            }
                        }
                    }
                    else
                    {
                        // Single-step form: pN X[:Y or :(group)]
                        // Look ahead (linear, no wrap) to detect ':(group)' after the note command
                        int lp = pos;
                        while (lp < pattern.length() && pattern[lp] == ' ') lp++;
                        while (lp < pattern.length()) // skip prefix commands before note command
                        {
                            char lc = pattern[lp];
                            if ((lc == 'o' || lc == 'O' || lc == 'v' || lc == 'V' || lc == 'p') && lp + 1 < pattern.length())
                                lp += 2;
                            else if (lc == '#' || lc == 'b')
                                lp++;
                            else
                                break;
                        }
                        if (lp < pattern.length()) lp++; // skip the note command char
                        while (lp < pattern.length() && pattern[lp] == ' ') lp++;

                        const bool hasGroupFallback = (lp     < pattern.length() && pattern[lp]     == ':' &&
                                                       lp + 1 < pattern.length() && pattern[lp + 1] == '(');
                        if (hasGroupFallback && !rollSucceeded)
                        {
                            // Fail: skip success note, enter fallback group directly (no rest)
                            pos = (lp + 2) % pattern.length(); // first char inside '('
                            scopeStack.push_back ({ octave, globalVelocity });
                            probGroupStack.push_back ({ (int) scopeStack.size() - 1 });
                            // prefix loop continues; outer for loop finds first group note
                        }
                        else
                        {
                            hasProbability = true;
                            if (!rollSucceeded) forcedRest = true;
                            // success + hasGroupFallback: post-loop will skip ':(group)'
                        }
                    }
                }
                else if (command == '#')
                {
                    currentStepIndex = getStepForPatternIndex(pos);
                    semitoneOffset = 1;
                    pos = (pos + 1) % pattern.length();
                }
                else if (command == 'b')
                {
                    currentStepIndex = getStepForPatternIndex(pos);
                    semitoneOffset = -1;
                    pos = (pos + 1) % pattern.length();
                }
                else if (command == '(')
                {
                    pos = (pos + 1) % pattern.length();
                    scopeStack.push_back ({ octave, globalVelocity });
                }
                else if (command == ')')
                {
                    pos = (pos + 1) % pattern.length();
                    if (!scopeStack.empty())
                    {
                        octave         = scopeStack.back().octave;
                        globalVelocity = scopeStack.back().velocity;
                        scopeStack.pop_back();
                    }
                    // If this closes a prob group's taken branch, skip the other branch
                    if (!probGroupStack.empty() && (int) scopeStack.size() == probGroupStack.back().nestDepth)
                    {
                        if (!pattern.isEmpty() && pattern[pos] == ':')
                        {
                            pos = (pos + 1) % pattern.length(); // consume ':'
                            skipProbFallback();
                        }
                        probGroupStack.pop_back();
                    }
                }
                else if (command == '"')
                {
                    pos = (pos + 1) % pattern.length();
                    inRootRelativeBlock = !inRootRelativeBlock;
                }
                else
                {
                    break; // Not a prefix, break to handle note commands.
                }
            }

            char command = pattern[pos];
            currentStepIndex = getStepForPatternIndex(pos);
            pos = (pos + 1) % pattern.length(); // Consume character

            if (command == '_')
            {
                isSustain = true;
                noteCommandFound = true;
            }
            else if (juce::CharacterFunctions::isDigit(command))
            {
                // Convert 1-indexed pattern digit to 0-indexed internal degree.
                // '1' -> 0, '2' -> 1, etc. '0' is not a valid note.
                int degreeValue = command - '0';
                if (degreeValue > 0)
                    currentDegreeIndex = degreeValue - 1;

                noteCommandFound = true;
            }
            else
            {
                // Handle other note commands
                switch (command)
                {
                    case '+':
                        currentDegreeIndex = (currentDegreeIndex + 1) % chord.getDegrees().size();
                        noteCommandFound = true; break;
                    case '-':
                        currentDegreeIndex = (currentDegreeIndex + chord.getDegrees().size() - 1) % chord.getDegrees().size();
                        noteCommandFound = true; break;
                    case '?': 
                        currentDegreeIndex = getRandomPresentDegree(); 
                        noteCommandFound = true;
                        // There are two possible behaviours
                        // 1. The last played degree is updated by a '?' command,
                        //    then it is taken ito account by '+' or '-'
                        // 2. The last played degree is not updated by a '?' command
                        // We choose option 1 for now
                        // shouldUpdateLastDegree = false; 
                        break;
                    case '=': /* currentDegreeIndex remains the same */ noteCommandFound = true; break;
                    case '.': currentDegreeIndex = -1; isRest = true; noteCommandFound = true; break;
                    default: // Ignore invalid characters (like spaces) and continue loop.
                        break;
                }
            }
        }

        // Single-step explicit fallback: pN X:Y or pN X:(group) — success path only
        // (fail+group is handled inside the prefix loop above, so hasProbability is not set there)
        if (hasProbability && !pattern.isEmpty() && pattern[pos] == ':')
        {
            pos = (pos + 1) % pattern.length(); // consume ':'
            if (!pattern.isEmpty() && pattern[pos] == '(')
            {
                // Success + group fallback: skip the entire fallback group
                skipProbFallback(); // pos is at '('; advances past matching ')'
            }
            else
            {
                // Single-char fallback: pN X:Y
                char fallback = pattern[pos];
                pos = (pos + 1) % pattern.length(); // consume fallback char
                if (forcedRest)
                {
                    forcedRest = false; isSustain = false; isRest = false;
                    currentDegreeIndex = lastPlayedDegreeIndex;
                    if (fallback == '_') { isSustain = true; }
                    else if (fallback == '.') { isRest = true; currentDegreeIndex = -1; }
                    else if (fallback == '?') { currentDegreeIndex = getRandomPresentDegree(); }
                    else if (juce::CharacterFunctions::isDigit(fallback) && fallback != '0')
                        currentDegreeIndex = (fallback - '0') - 1;
                }
                // success: fallback char already consumed (skipped)
            }
        }

        if (forcedRest)
        {
            if (isSustain)      { isSustain = false; isRest = true; }   // _ → .
            else if (isRest)    { isRest = false; isSustain = true; }   // . → _
            else                { isRest = true; currentDegreeIndex = -1; } // note → .
        }
        if (isSustain)
            return midiBuffer;  // sustain: keep previous note ringing

        // --- Turn off the previous note ---
        // This now happens *after* we've decided what the next command is.
        if (lastPlayedMidiNote != -1)
        {
            midiBuffer.addEvent(juce::MidiMessage::noteOff(lastPlayedMidiChannel, lastPlayedMidiNote), samplePosition);
            lastPlayedMidiNote = -1;
        }

        // --- 3. Determine the final MIDI note to play ---
        if (noteToPlay == -1 && currentDegreeIndex != -1) // If not sustaining and not a rest
        {
            // std::cout << "We should play a note!" << std::endl;
            // std::cout << "currentDegreeIndex = " << currentDegreeIndex << std::endl;

            int finalNote = getNoteForDegree(currentDegreeIndex);
            // std::cout << "     semitone = " << semitone << std::endl;

            if (finalNote != -1)
            {
                // Use local octave if set, otherwise use global octave.
                int octaveToUse = (localOctave != -1) ? localOctave : octave;

                // Calculate the octave offset from the base.
                int octaveOffset = octaveToUse - baseOctave;
                // For "Notes played" (0) and "Single note" (2) modes, the finalNote is a semitone (0-11)
                // that needs to be placed in an absolute octave.
                // For "Chord played as is" (1) mode, the finalNote is a full MIDI note that needs a relative offset.
                if (chordMethod == 1)
                    noteToPlay = finalNote + (octaveOffset * 12);
                else // Modes 0 and 2
                    noteToPlay = finalNote + (octaveToUse * 12);
            }
        }
        
        // --- 4. Generate MIDI event ---
        if (noteToPlay != -1 )
        {
            noteToPlay += semitoneOffset; // Apply sharp/flat

            // Use local velocity if set, otherwise use global velocity.
            juce::uint8 velocityToUse = (localVelocity != -1) ? (juce::uint8)localVelocity : (juce::uint8)globalVelocity;
            midiBuffer.addEvent(juce::MidiMessage::noteOn(midiChannel, noteToPlay, velocityToUse), samplePosition);
            lastPlayedMidiNote = noteToPlay;
            lastPlayedMidiChannel = midiChannel;
            if (shouldUpdateLastDegree)
                lastPlayedDegreeIndex = currentDegreeIndex;
        }

        // std::cout << "     noteToplay = " << noteToPlay << std::endl;
        // std::cout << "     Num events = " << midiBuffer.getNumEvents() << std::endl;

        return midiBuffer;
    }

public:
    /** Returns the number of samples remaining until the next note event. */
    double getSamplesUntilNextNote() const
    {
        return samplesUntilNextNote;
    }

    /** Sets the number of samples remaining until the next note event. */
    void setSamplesUntilNextNote(double samples)
    {
        samplesUntilNextNote = samples;
    }
public:
    // --- Setters for properties ---
    void setChord(const MidiTools::Chord& newChord)
    {
        chord = newChord;
    }
    void setPattern(const juce::String& newPattern)
    {
        pattern = newPattern;
        pos = 0;
        octave = baseOctave;
        scopeStack.clear();
        probGroupStack.clear();
        inRootRelativeBlock = false;
    }
    void setOctave(int newOctave) { octave = juce::jlimit(0, 7, newOctave); }
    void setPlayNoteOffMode(const juce::String& mode) { playNoteOff = mode; }
    void setTempo(double newTempoBPM)
    {
        tempoBPM = newTempoBPM > 0 ? newTempoBPM : 120.0;
        updateSamplesPerNote();
    }

    void setSubdivision(int subdivisionIndex)
    {
        subdivision = subdivisionIndex;
        updateSamplesPerNote();
    }

    void setChordMethod(int methodIndex)
    {
        chordMethod = methodIndex;
    }

    /** Supplies the root chord used inside " " blocks in single-note mode. */
    void setRootChord (const MidiTools::Chord& rc) { rootChord = rc; }

    /**
        Sets the base octave based on an incoming MIDI note.
        This is used in "Single Note" mode to make the output octave follow the input.
        @param midiNoteNumber The MIDI note number from which to derive the octave.
    */
    void setBaseOctaveFromNote(int midiNoteNumber)
    {
        // MIDI note 60 (C4) is in octave 4. (60 / 12) - 1 = 4.
        int newBaseOctave = (midiNoteNumber / 12) - 1;
        newBaseOctave = juce::jlimit(0, 7, newBaseOctave);

        int diff = newBaseOctave - baseOctave;
        baseOctave = newBaseOctave;
        // Adjust the current octave by the difference to maintain relative shifts
        octave = juce::jlimit(0, 7, octave + diff);
    }

    /**
        Sets the global arpeggiator velocity based on an incoming MIDI note's velocity.
        It converts the 0-127 MIDI velocity into an internal 1-8 level.
        @param midiVelocity The velocity of the incoming MIDI note (1-127).
    */
    void setGlobalVelocityFromMidi(int midiVelocity)
    {
        if (midiVelocity > 0)
        {
            int velocityLevel = static_cast<int>(std::ceil(static_cast<float>(midiVelocity) / 16.0f));
            velocityLevel = juce::jlimit(1, 8, velocityLevel); // Ensure it's within 1-8 range
            globalVelocity = juce::jmin(127, velocityLevel * 16);
        }
    }

    /**
        Generates a Euclidean pattern string.
        @param hits Number of notes.
        @param steps Total length of the sequence.
    */
    juce::String makeEuclidianPattern(int hits, int steps, int rotation)
    {
        auto bools = MidiTools::euclidianRythm(hits, steps, rotation);
        juce::String s;
        for (bool b : bools)
            s += (b ? "1 " : ". ");
        return s.trim();
    }

    /**
        Generates a random pattern string without applying it.
        Rules:
        - It shouldn't start with a "_"
        - Balanced global relative modifiers (O+, V+, O-, V-) to prevent drift.
        - Note values between 1 and 9.
    */
    juce::String makeRandomPattern()
    {
        juce::Random& rng = juce::Random::getSystemRandom();
        int length = rng.nextInt(13) + 4; // Random length between 4 and 16

        struct Step {
            juce::String prefixes;
            juce::String note;
        };
        juce::Array<Step> steps;

        for (int i = 0; i < length; ++i)
        {
            Step s;
            int r = rng.nextInt(100);

            if (i == 0)
            {
                // First step: ensure it's not a sustain ('_') or rest ('.') for a strong start
                if (r < 60) s.note = juce::String(rng.nextInt(5) + 1); // Start with 1-5
                else if (r < 80) s.note = "+";
                else s.note = "?";
            }
            else
            {
                if (r < 40) s.note = juce::String(rng.nextInt(9) + 1); // 1-9
                else if (r < 55) s.note = "_";
                else if (r < 65) s.note = ".";
                else if (r < 75) s.note = "+";
                else if (r < 85) s.note = "-";
                else if (r < 90) s.note = "?";
                else s.note = "=";
            }
            steps.add(s);
        }

        // Add Local Modifiers
        for (auto& s : steps)
        {
            if (s.note == "_" || s.note == ".") continue;

            // Local Octave
            if (rng.nextFloat() < 0.15f)
            {
                float r2 = rng.nextFloat();
                if (r2 < 0.4f) s.prefixes += "o+";
                else if (r2 < 0.8f) s.prefixes += "o-";
                else s.prefixes += "o" + juce::String(rng.nextInt(3) + 3); // o3-o5
            }
            
            // Local Pitch
            if (rng.nextFloat() < 0.1f)
                s.prefixes += (rng.nextBool() ? "#" : "b");

            // Local Velocity
            if (rng.nextFloat() < 0.1f)
                s.prefixes += "v" + juce::String(rng.nextInt(4) + 5); // v5-v8
        }

        // Add Global Modifiers (Balanced)
        auto addBalancedGlobal = [&](juce::String plus, juce::String minus) {
            int idx1 = rng.nextInt(length);
            int idx2 = rng.nextInt(length);
            while (idx1 == idx2) idx2 = rng.nextInt(length);
            steps.getReference(idx1).prefixes += plus;
            steps.getReference(idx2).prefixes += minus;
        };

        if (rng.nextFloat() < 0.3f) addBalancedGlobal("O+", "O-");
        if (rng.nextFloat() < 0.3f) addBalancedGlobal("V+", "V-");

        juce::String newPattern;
        for (const auto& s : steps)
            newPattern += s.prefixes + s.note + " ";

        return newPattern.trim();
    }

    void randomize()
    {
        setPattern(makeRandomPattern());
    }

    /** Returns the current pattern string. */
    const juce::String& getPattern() const
    {
        return pattern;
    }

    /** Returns a const reference to the currently active chord. */
    const MidiTools::Chord& getChord() const
    {
        return chord;
    }

    /** Returns the index of the current musical step being played. */
    int getCurrentStepIndex() const
    {
        return currentStepIndex;
    }

    /** Returns the last MIDI note number that was played. */
    int getLastPlayedNote() const
    {
        return lastPlayedMidiNote;
    }
    /** Calculates the number of musical steps in the pattern string. */
    int numSteps() const
    {
        if (pattern.isEmpty())
            return 0;

        int steps = 0;
        int i = 0;
        while (i < pattern.length())
        {
            const char command = pattern[i];
            if (command == 'o' || command == 'O' || command == 'v' || command == 'V' || command == 'p')
            {
                i += 2; // Skip prefix and its argument
            }
            else if (command == '#' || command == 'b')
            {
                i++; // Skip sharp/flat prefix
            }
            else if (command == '(' || command == ')' || command == '"')
            {
                i++; // Block markers, not musical steps
            }
            else if (command == ':')
            {
                i++; // Skip ':' and its fallback (group or single char)
                if (i < pattern.length() && pattern[i] == '(')
                {
                    int depth = 1; i++;
                    while (i < pattern.length() && depth > 0)
                    {
                        if (pattern[i] == '(') ++depth;
                        else if (pattern[i] == ')') --depth;
                        i++;
                    }
                }
                else if (i < pattern.length()) i++;
            }
            else if (juce::CharacterFunctions::isDigit(command) || command == '+' ||
                     command == '-' || command == '?' ||
                     command == '=' || command == '.' || command == '_')
            {
                steps++;
                i++;
            }
            else
            {
                i++; // Ignore spaces and other unknown characters
            }
        }
        return steps;
    }

    /** Given a step index (0, 1, 2...), find the corresponding character index in the pattern string. */
    int getPatternIndexForStep(int stepIndex) const
    {
        if (pattern.isEmpty())
            return 0;

        int currentStepCount = 0;
        int i = 0;
        while (i < pattern.length())
        {
            if (currentStepCount == stepIndex)
                return i; // Found the start of the desired step

            const char command = pattern[i];
            if (command == 'o' || command == 'O' || command == 'v' || command == 'V' || command == 'p')
            {
                i += 2; // Skip prefix and its argument
            }
            else if (command == '#' || command == 'b')
            {
                i++; // Skip sharp/flat prefix
            }
            else if (command == '(' || command == ')' || command == '"')
            {
                i++; // Block markers, not musical steps
            }
            else if (command == ':')
            {
                i++;
                if (i < pattern.length() && pattern[i] == '(')
                {
                    int depth = 1; i++;
                    while (i < pattern.length() && depth > 0)
                    {
                        if (pattern[i] == '(') ++depth;
                        else if (pattern[i] == ')') --depth;
                        i++;
                    }
                }
                else if (i < pattern.length()) i++;
            }
            else if (juce::CharacterFunctions::isDigit(command) || command == '+' ||
                     command == '-' || command == '?' ||
                     command == '=' || command == '.' || command == '_')
            {
                currentStepCount++;
                i++;
            }
            else
            {
                i++; // Ignore spaces and other unknown characters
            }
        }
        return 0; // Fallback if stepIndex is out of bounds
    }

    /** Given a character index in the pattern string, find the corresponding musical step index. */
    int getStepForPatternIndex(int patternIndex) const
    {
        if (pattern.isEmpty() || patternIndex < 0)
            return 0;

        int stepCount = 0;
        int i = 0;
        while (i < pattern.length() && i < patternIndex)
        {
            const char command = pattern[i];
            if (command == 'o' || command == 'O' || command == 'v' || command == 'V' || command == 'p')
            {
                i += 2; // Skip prefix and its argument
            }
            else if (command == '#' || command == 'b')
            {
                i++; // Skip prefix
            }
            else if (command == ':')
            {
                i++;
                if (i < pattern.length() && pattern[i] == '(')
                {
                    int depth = 1; i++;
                    while (i < pattern.length() && depth > 0)
                    {
                        if (pattern[i] == '(') ++depth;
                        else if (pattern[i] == ')') --depth;
                        i++;
                    }
                }
                else if (i < pattern.length()) i++;
            }
            else if (juce::CharacterFunctions::isDigit(command) || command == '+' ||
                     command == '-' || command == '?' || command == '"' ||
                     command == '=' || command == '.' || command == '_')
            {
                stepCount++;
                i++;
            }
            else
            {
                i++; // Ignore other characters
            }
        }
        return stepCount;
    }

    /** Returns the total duration of one full pattern loop in PPQ. */
    double ppqDuration() const
    {
        const int steps = numSteps();
        if (steps == 0)
            return 0.0;

        // The duration of one step in PPQ is 1.0 / notesPerQuarter.
        return steps / getNoteDivisor();
    }

    /**
        Synchronizes the arpeggiator's internal clock to the host's transport position.
        This should be called on every process block while the host is playing.
        @param positionInfo The host's current position information.
    */
    void syncToPlayHead(const juce::AudioPlayHead::CurrentPositionInfo& positionInfo)
    {
        if (samplesPerNote <= 0.0 || positionInfo.ppqPosition < 0.0 || pattern.isEmpty())
            return;
    
        const double patternDurationPPQ = ppqDuration();
        if (patternDurationPPQ <= 0.0)
            return;
    
        const double stepDurationPPQ = 1.0 / getNoteDivisor();
        const double songPosInSteps = positionInfo.ppqPosition / stepDurationPPQ;
        const double patternDurationInSteps = patternDurationPPQ / stepDurationPPQ;
    
    
        // Calculate how many samples until the next step boundary in the host timeline
        const double nextStepInSong = std::ceil(songPosInSteps);
        const double stepsUntilNext = nextStepInSong - songPosInSteps;
        const double ppqUntilNext = stepsUntilNext * stepDurationPPQ;
        const double secondsPerPPQ = 60.0 / (tempoBPM * 1.0); // 1.0 is quarter note
        samplesUntilNextNote = ppqUntilNext * secondsPerPPQ * sampleRate;
    }

    /** Resets the arpeggiator's position to the beginning of the pattern. */
    juce::MidiBuffer reset(int midiChannel = 1, const juce::Optional<juce::AudioPlayHead::CurrentPositionInfo> positionInfo = {})
    {
        juce::MidiBuffer noteOffBuffer;
        if (lastPlayedMidiNote != -1)
        {
            noteOffBuffer.addEvent(juce::MidiMessage::noteOff(lastPlayedMidiChannel, lastPlayedMidiNote), 0);
            lastPlayedMidiNote = -1;
        }

        octave = baseOctave;
        globalVelocity = 96; // Reset global velocity to default
        pos = 0;
        lastPlayedDegreeIndex = 0;
        samplesUntilNextNote = 0;
        scopeStack.clear();
        probGroupStack.clear();
        inRootRelativeBlock = false;

        // If host position is provided (i.e., transport just started), sync to it.
        if (positionInfo.hasValue())
        {
            const double patternDurationPPQ = ppqDuration();
            if (patternDurationPPQ > 0.0)
            {
                const double stepDurationPPQ = 1.0 / getNoteDivisor();
                const double songPosInSteps = positionInfo->ppqPosition / stepDurationPPQ;
                const double patternDurationInSteps = patternDurationPPQ / stepDurationPPQ;
                const int nextStepIndex = static_cast<int>(std::floor(songPosInSteps)) % static_cast<int>(patternDurationInSteps);
                pos = getPatternIndexForStep(nextStepIndex);
                samplesUntilNextNote = 0; // Trigger immediate evaluation for the current position
            }
        }

        return noteOffBuffer;
    }

    /** Generates a note-off for the last played note and resets the state. */
    juce::MidiBuffer turnOff(int midiChannel = 1)
    {
        juce::MidiBuffer noteOffBuffer;
        if (lastPlayedMidiNote != -1)
        {
            noteOffBuffer.addEvent(juce::MidiMessage::noteOff(lastPlayedMidiChannel, lastPlayedMidiNote), 0);
            lastPlayedMidiNote = -1;
        }
        // Also reset pattern position and other state variables for a clean start next time.
        pos = 0;
        lastPlayedDegreeIndex = 0;
        octave = baseOctave;
        scopeStack.clear();
        probGroupStack.clear();
        inRootRelativeBlock = false;
        return noteOffBuffer;
    }

protected:
    // -----------------------------------------------------------------------
    // Scope stack for ( ) local-modifier blocks
    struct ScopeState { int octave; int velocity; };
    std::vector<ScopeState> scopeStack;

    // Stack for pN(success):(fallback) group probability tracking
    struct ProbGroupState { int nestDepth; };
    std::vector<ProbGroupState> probGroupStack;

    // Root-relative mode: inside " " blocks in single-note mode (chordMethod==2),
    // notes are resolved against rootChord (degree-0 of the scale) rather than
    // the chord built from the pressed MIDI note's degree.
    bool             inRootRelativeBlock = false;
    MidiTools::Chord rootChord { "" };

    /**
        Finds a valid semitone for a given degree index, handling absent notes.
        For "Notes played" mode, it returns a semitone (0-11).
        For "Chord played as is" mode, it returns a full MIDI note number (0-127).
        Inside a " " block in single-note mode the rootChord is used instead.
    */
    int getNoteForDegree(int degreeIndex)
    {
        const MidiTools::Chord& activeChord = (inRootRelativeBlock && chordMethod == 2)
                                              ? rootChord : chord;

        if (chordMethod == 1) // "Chord played as is"
        {
            const auto& rawNotes = activeChord.getRawNotes();
            if (rawNotes.isEmpty())
                return -1;
            return rawNotes[degreeIndex % rawNotes.size()];
        }
        else // "Notes played" (mode 0) and "Single note" (mode 2)
        {
            const auto& degrees = activeChord.getDegrees();
            if (degrees.isEmpty())
                return -1;
            if (!juce::isPositiveAndBelow(degreeIndex, degrees.size()))
                degreeIndex %= degrees.size();

            if (activeChord.getName() == "Custom")
            {
                juce::SortedSet<int> playedNotes = activeChord.getSortedSet();
                int numPlayedNotes = playedNotes.size();
                if (numPlayedNotes > 0)
                    return playedNotes[degreeIndex % numPlayedNotes];
            }

            int semitone = activeChord.getDegree(degreeIndex);
            if (semitone != -1)
                return semitone;

            if (playNoteOff == "Off")
            {
                return -1;
            }
            else if (playNoteOff == "Next")
            {
                for (int i = 1; i < degrees.size(); ++i)
                {
                    semitone = activeChord.getDegree((degreeIndex + i) % degrees.size());
                    if (semitone != -1) return semitone;
                }
            }
            else if (playNoteOff == "Previous")
            {
                for (int i = 1; i < degrees.size(); ++i)
                {
                    semitone = activeChord.getDegree((degreeIndex + degrees.size() - i) % degrees.size());
                    if (semitone != -1) return semitone;
                }
            }
        }

        return activeChord.getDegree(0);
    }

    /**
        Selects a random degree index that is actually present in the chord, for use with the '?' command.
        @return A valid degree index (0-6), or -1 if the chord is empty.
    */
    int getRandomPresentDegree()
    {
        if (chordMethod == 1)
        {
            if (chord.getRawNotes().isEmpty())
                return -1;
            return juce::Random::getSystemRandom().nextInt(chord.getRawNotes().size());
        }

        juce::Array<int> presentDegrees;
        const auto& degrees = chord.getDegrees();
        for (int i = 0; i < degrees.size(); ++i)
        {
            if (degrees[i] != -1)
                presentDegrees.add(i);
        }

        if (presentDegrees.isEmpty())
            return -1;

        return presentDegrees[juce::Random::getSystemRandom().nextInt(presentDegrees.size())];
    }

    MidiTools::Chord chord;
    juce::String pattern;
    int baseOctave = 4;
    int octave = baseOctave;
    juce::String playNoteOff = "Next"; // "Off", "Next", "Previous"
    int chordMethod = 0; // 0: Notes played, 1: Chord played as is, 2: Single note
    int globalVelocity = 96; // Default velocity

    int pos = 0;
    int lastPlayedMidiNote = -1;
    int lastPlayedMidiChannel = 1;
    int lastPlayedDegreeIndex = 0;
    int currentStepIndex = 0;

private:
    // Advance pos past the matching ')'. Call with pos already past the opening '('.
    void skipToMatchingParen()
    {
        int depth = 1;
        for (int guard = pattern.length(); guard > 0 && depth > 0; --guard)
        {
            char c = pattern[pos];
            if (c == '(') ++depth;
            else if (c == ')') --depth;
            pos = (pos + 1) % pattern.length();
        }
    }

    // Skip a ':fallback' target — either a '(group)' or a single char.
    void skipProbFallback()
    {
        if (pattern.isEmpty()) return;
        if (pattern[pos] == '(')
        {
            pos = (pos + 1) % pattern.length(); // consume '('
            skipToMatchingParen();
        }
        else
        {
            pos = (pos + 1) % pattern.length(); // skip single char
        }
    }

    double getNoteDivisor() const
    {
        switch (subdivision)
        {
            case 0: return 1.0;  // 1/4
            case 1: return 1.5;  // 1/4T
            case 2: return 2.0;  // 1/8
            case 3: return 3.0;  // 1/8T
            case 4: return 4.0;  // 1/16
            case 5: return 6.0;  // 1/16T
            case 6: return 8.0;  // 1/32
            case 7: return 12.0; // 1/32T
            case 8: return 16.0; // 1/64
            case 9: return 24.0; // 1/64T
            default: return 4.0;
        }
    }

    void updateSamplesPerNote()
    {
        if (sampleRate > 0 && tempoBPM > 0)
        {
            const double noteDivisor = getNoteDivisor();
            double quarterNoteDurationSeconds = 60.0 / tempoBPM;
            samplesPerNote = sampleRate * quarterNoteDurationSeconds / noteDivisor;
        }
    }
    double sampleRate = 0.0;
    double tempoBPM = 120.0;
    int subdivision = 4; // Default to 1/16
    double samplesPerNote = 0.0;
    double samplesUntilNextNote = 0.0;
};

} // namespace fxme
