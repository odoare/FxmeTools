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

    Degrees and velocity levels are written as a single uppercase hexadecimal
    character ('0'-'9' then 'A'-'F') since syntax version 2. Octave (0-7) and
    probability (0-9) stay decimal. See migratePatternV1toV2() for what changed.

    The pattern string consists of characters that define the arpeggio's behavior at each step:
    - '1' to 'F': Plays a specific degree of the chord/scale (1=fundamental, 2=2nd, ..., up to 15).
      - The `playNoteOff` property determines behavior for absent degrees ("Off", "Next", "Previous").
    - '0': Closes a step without naming a degree, so it fires on the current one. Example: "vF0"
    - '_': Sustains the previously played note.
    - '.': A rest; no note is played.
    - '+': Plays the next degree in the chord (e.g., from 1 to 2).
    - '-': Plays the previous degree in the chord (e.g., from 2 to 1).
    - '?': Plays a random, valid note from the current chord.
    - '=': Repeats the last played degree.
    - 'pN': Plays the next note with probability N×10% (N is 1–9). On a failed roll the step becomes a rest. Example: "p5 1" plays the root 50% of the time.
    - '#' (Sharp): Pitches the next note up by one semitone. This is a local effect. Example: "#0"
    - 'b' (Flat): Pitches the next note down by one semitone. This is a local effect. Example: "b0"
    - '( … )': Scopes global modifiers, restoring octave and velocity at the closing bracket.
    - '" … "': Root-relative block (scale mode): notes anchor to the scale root
      rather than to the pressed note's degree. Not a musical step itself.

    Velocity Modifiers (prefixed to a note command):
    - Where the pattern sets no velocity, notes take the velocity of the note the
      player is holding (see setPlayedVelocityFromMidi). A 'V' overrides that
      until another 'V' changes it or the enclosing '( )' block closes, at which
      point control returns to whatever applied outside the block, playing
      velocity included.
    - 'vN': Sets velocity for the next note only. N is 1-F, spread over 1-127. Example: "vF0"
    - 'VN': Sets velocity globally until the next 'V' command. Example: "V80"
    - 'v+' / 'v-': One level louder / quieter, for the next note only.
    - 'V+' / 'V-': One level louder / quieter, from here on. Both saturate rather than wrap.
    - 'v?' / 'V?': A random level.

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
    //==========================================================================
    // Pattern alphabet
    //
    // Values in the pattern language are a single character, so the range a
    // value can cover is the size of the alphabet. Since syntax version 2 that
    // alphabet is hexadecimal, '0'-'9' then 'A'-'F', which gives degrees up to
    // 15 (chords and scales with more than nine notes) and 15 velocity levels
    // instead of 8.
    //
    // Uppercase only, deliberately: 'b' is the flat command, so lowercase hex
    // would be ambiguous wherever a value sits in command position, and a
    // single case is easier to read than a rule about which position you are
    // in.

    /** Value of a pattern value character: '0'-'9' -> 0-9, 'A'-'F' -> 10-15.
        Returns -1 for anything else. */
    static int hexValue (char c) noexcept
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    }

    /** Inverse of hexValue: 0-15 -> '0'-'9', 'A'-'F'. For pattern generators. */
    static char valueChar (int v) noexcept
    {
        v = juce::jlimit (0, 15, v);
        return (char) (v < 10 ? ('0' + v) : ('A' + v - 10));
    }

    /** True if `c` opens a musical step, as opposed to a prefix ('o', 'v',
        '#'...) or a block marker. Shared by the parser and the three step
        traversals so that they cannot drift apart. */
    static bool isStepCommand (char c) noexcept
    {
        return hexValue (c) >= 0 || c == '+' || c == '-' || c == '?'
            || c == '=' || c == '.' || c == '_';
    }

    /** Number of velocity levels, i.e. the top of the value alphabet. */
    static constexpr int maxVelocityLevel = 15;

    /** MIDI velocity for a level in 1..maxVelocityLevel. */
    static int velocityForLevel (int level) noexcept
    {
        return juce::jlimit (1, 127, juce::roundToInt (
            (float) level * 127.0f / (float) maxVelocityLevel));
    }

    /** Nearest level to a MIDI velocity: the inverse of velocityForLevel, used
        by the relative commands ('v+', 'V-') to step from whatever velocity is
        currently in effect. Floors at 1 so a run of 'V-' cannot silence the
        pattern outright. */
    static int levelForVelocity (int velocity) noexcept
    {
        return juce::jlimit (1, maxVelocityLevel, juce::roundToInt (
            (float) velocity * (float) maxVelocityLevel / 127.0f));
    }

    /**
        Rewrites a syntax-version-1 pattern into version 2.

        Only velocity changed meaning: v1 read the argument of 'v'/'V' as a
        decimal 1-9 on a `level * 16` scale (so 8 and 9 both clamped to 127),
        while v2 reads it as a hex 1-F spread over the full range. Every other
        command, degrees included, means in v2 exactly what it meant in v1, so
        this touches nothing else — importantly not the arguments of 'o', 'O'
        and 'p', which are still decimal.

        Each old level is mapped to whichever new level lands nearest the
        velocity it used to produce.
    */
    static juce::String migratePatternV1toV2 (const juce::String& oldPattern)
    {
        // Old level -> nearest new level, indexed 0-9, via the velocity each
        // produced: round(min(127, n * 16) * 15 / 127). Level 0 means "no
        // velocity change" in both versions, and old 8 and 9 both clamped to
        // 127, so both land on F.
        static const char* const velocityMap = "024689BDFF";

        juce::String result;
        result.preallocateBytes ((size_t) oldPattern.getNumBytesAsUTF8());

        int i = 0;
        while (i < oldPattern.length())
        {
            const char c = oldPattern[i];

            if (c == 'v' || c == 'V')
            {
                result += c;
                if (i + 1 < oldPattern.length())
                {
                    const char arg = oldPattern[i + 1];
                    const int  old = (arg >= '0' && arg <= '9') ? arg - '0' : -1;
                    // '?', '+', '-' and anything else pass through untouched.
                    result += (old >= 0) ? velocityMap[old] : arg;
                    i += 2;
                }
                else
                {
                    ++i;
                }
            }
            else if (c == 'o' || c == 'O' || c == 'p')
            {
                // Prefix and its argument, both unchanged: copy them together
                // so an argument that happens to be a digit is never mistaken
                // for a velocity value.
                result += c;
                if (i + 1 < oldPattern.length())
                    result += oldPattern[i + 1];
                i += 2;
            }
            else
            {
                result += c;
                ++i;
            }
        }

        return result;
    }

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

        // Grow the two output buffers here, once, so that the audio thread
        // never has to. MidiBuffer::clear() is Array::clearQuick(), which drops
        // the element count without releasing the block, so from here on the
        // clear-and-refill cycle below allocates nothing at all.
        //
        // 2 kB is well beyond what one block of the fastest subdivision can
        // produce (two events per step, a dozen or so bytes each); if some
        // pathological block ever exceeds it, the buffer grows once and then
        // stays grown.
        outMidi.ensureSize (2048);
        stepMidi.ensureSize (64);
    }

    //=========================================================================
    // processBlock(), reset() and turnOff() all return a reference to one
    // buffer owned by this engine, rather than a fresh juce::MidiBuffer by
    // value. Returning by value meant a heap allocation on the audio thread
    // every time a step fired, which is what this avoids.
    //
    // The lifetime rule that comes with it: the returned reference is only
    // valid until the next call to any of the three. Consume it straight away,
    // which is what the natural call shape already does:
    //
    //     midiMessages.addEvents (arp.processBlock (n, ch), 0, -1, 0);
    //
    // Copying it into a juce::MidiBuffer of your own still works and still
    // costs an allocation, so `auto buf = arp.processBlock (n);` behaves
    // exactly as it did before. One engine per voice, as the callers already
    // do, means the three never overlap.
    //=========================================================================

    /**
        Generates MIDI events for the current block of audio samples.
        @param numSamples The number of samples in the current audio block.
        @return A reference to this engine's output buffer, holding any events
                generated during this block. Valid until the next call to
                processBlock(), reset() or turnOff().
    */
    const juce::MidiBuffer& processBlock(int numSamples, int midiChannel = 1)
    {
        outMidi.clear();
        if (sampleRate <= 0.0 || samplesPerNote <= 0.0 || pattern.isEmpty())
            return outMidi;
        if (midiChannel < 1 || midiChannel > 16) midiChannel = 1;

        int time = 0;
        while (time < numSamples)
        {
            if (samplesUntilNextNote <= 0.0)
            {
                // getNext() fills stepMidi, a different buffer, so this is not
                // a self-aliasing addEvents.
                outMidi.addEvents(getNext(midiChannel), 0, -1, time);
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
        return outMidi;
    }

private:
    /**
        Processes the next step in the arpeggio pattern and returns MIDI messages.
        @return A reference to stepMidi, holding this step's note-off and/or
                note-on. Valid until the next call. Private, and only
                processBlock() calls it.
    */
    const juce::MidiBuffer& getNext(int midiChannel)
    {
        juce::MidiBuffer& midiBuffer = stepMidi;
        midiBuffer.clear();
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
                    
                    // Same shape as the octave block above: '+'/'-' step one
                    // level from whatever is in effect for this step, '?' picks
                    // one at random, and a value sets the level outright.
                    // Levels are hex since syntax v2: 1-F over the full range.
                    int currentLevel = levelForVelocity((localVelocity != -1) ? localVelocity
                                                                              : effectiveVelocity());
                    int velocityLevel;

                    if (velocityValueChar == '+')
                        velocityLevel = juce::jmin(maxVelocityLevel, currentLevel + 1);
                    else if (velocityValueChar == '-')
                        velocityLevel = juce::jmax(1, currentLevel - 1);
                    else if (velocityValueChar == '?')
                        velocityLevel = juce::Random::getSystemRandom().nextInt(maxVelocityLevel) + 1;
                    else
                        velocityLevel = hexValue(velocityValueChar);   // 0 or -1 = leave alone

                    if (velocityLevel > 0)
                    {
                        int velocity = velocityForLevel(velocityLevel);
                        if (command == 'v') localVelocity = velocity;
                        else patternVelocity = velocity;
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
                            scopeStack.push_back ({ octave, patternVelocity });
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
                                    scopeStack.push_back ({ octave, patternVelocity });
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
                            scopeStack.push_back ({ octave, patternVelocity });
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
                    scopeStack.push_back ({ octave, patternVelocity });
                }
                else if (command == ')')
                {
                    pos = (pos + 1) % pattern.length();
                    if (!scopeStack.empty())
                    {
                        octave          = scopeStack.back().octave;
                        patternVelocity = scopeStack.back().velocity;
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
            else if (hexValue(command) >= 0)
            {
                // Convert 1-indexed pattern value to 0-indexed internal degree.
                // '1' -> 0, '2' -> 1 ... 'F' -> 14. '0' is not a valid note: it
                // closes the step without naming a degree, so the step fires on
                // whichever degree is current (the idiom behind "vF0").
                int degreeValue = hexValue(command);
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

            // Local 'v' wins for this one note; otherwise whatever is in effect.
            juce::uint8 velocityToUse = (localVelocity != -1) ? (juce::uint8)localVelocity
                                                              : (juce::uint8)effectiveVelocity();
            midiBuffer.addEvent(juce::MidiMessage::noteOn(midiChannel, noteToPlay, velocityToUse), samplePosition);
            lastPlayedMidiNote = noteToPlay;
            lastPlayedMidiChannel = midiChannel;
            noteOnCounter.bump();
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
        patternLengthIsFixed = patternHasFixedLength (pattern);
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
        Records the velocity of an incoming note-on as the played velocity: what
        the pattern uses wherever it does not set a velocity of its own.

        The value is rounded to the nearest level of the pattern alphabet, so a
        following 'V+' or 'v-' steps from a level the language can name rather
        than from an arbitrary point between two.

        This never touches the pattern's own 'V' setting; see effectiveVelocity().

        @param midiVelocity The velocity of the incoming MIDI note (1-127).
    */
    void setPlayedVelocityFromMidi(int midiVelocity)
    {
        if (midiVelocity > 0)
            playedVelocity = velocityForLevel (levelForVelocity (midiVelocity));
    }

    /** The velocity a note fires at when the step carries no local 'v': the
        pattern's 'V' if one is in effect here, else the played velocity. */
    int effectiveVelocity() const noexcept
    {
        return (patternVelocity >= 0) ? patternVelocity : playedVelocity;
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

            // Local Velocity: an accent, so the top half of the range. Levels
            // are hex since syntax v2, so v9-vF is what v5-v8 used to be.
            if (rng.nextFloat() < 0.1f)
                s.prefixes += juce::String("v") + valueChar(rng.nextInt(7) + 9); // v9-vF
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

    /** Monotonic count of note-ons emitted since construction. Poll it from a
        GUI timer and compare with the previous reading to detect that this
        arpeggiator fired: a change means at least one note started since the
        last poll. Wrapping after 2^32 notes is harmless, since only
        inequality is meaningful. Safe to call from any thread. */
    uint32_t getNoteOnCount() const noexcept
    {
        return noteOnCounter.get();
    }
private:
    /**
        Advances `i` past exactly one token of the pattern and reports whether
        that token was a musical step.

        The three public step queries below are the same walk asked three
        different questions, so they share this one definition. They used to
        carry a copy each, and the copies had drifted: one of them counted '"'
        as a step while the others treated it as a block marker, which made the
        playing-step highlight slide out of position in any pattern using
        root-relative blocks.
    */
    /** The host's position expressed in steps, with floating-point jitter
        snapped away. A host reporting a position a millionth of a step before
        a boundary must not be read as "a whole step still to go". */
    double songPositionInSteps (double ppqPosition) const noexcept
    {
        const double raw     = ppqPosition * getNoteDivisor();
        const double nearest = std::round (raw);
        return (std::abs (raw - nearest) < 1.0e-6) ? nearest : raw;
    }

    /** Wraps an absolute song step onto the pattern. */
    static int patternStepForSongStep (double songStep, int patternSteps) noexcept
    {
        if (patternSteps <= 0)
            return 0;

        int s = (int) std::fmod (songStep, (double) patternSteps);
        return (s < 0) ? s + patternSteps : s;
    }

    bool advanceOneToken(int& i) const
    {
        const char command = pattern[i];

        if (command == 'o' || command == 'O' || command == 'v' ||
            command == 'V' || command == 'p')
        {
            i += 2;             // prefix and its argument
        }
        else if (command == '#' || command == 'b')
        {
            i++;                // sharp / flat prefix
        }
        else if (command == '(' || command == ')' || command == '"')
        {
            i++;                // block markers, not musical steps
        }
        else if (command == ':')
        {
            i++;                // probability fallback: skip it whole
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
            else if (i < pattern.length())
            {
                i++;
            }
        }
        else if (isStepCommand(command))
        {
            i++;
            return true;
        }
        else
        {
            i++;                // spaces and anything unrecognised
        }

        return false;
    }

public:
    /** Calculates the number of musical steps in the pattern string. */
    int numSteps() const
    {
        int steps = 0;
        int i = 0;
        while (i < pattern.length())
            if (advanceOneToken(i))
                ++steps;
        return steps;
    }

    /** Given a step index (0, 1, 2...), find the corresponding character index
        in the pattern string. The index returned is the start of the whole
        step, prefixes and block markers included, so that highlighting a step
        highlights its modifiers too. */
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

            if (advanceOneToken(i))
                ++currentStepCount;
        }
        return 0; // Fallback if stepIndex is out of bounds
    }

    /** Given a character index in the pattern string, find the corresponding
        musical step index. Exact inverse of getPatternIndexForStep(). */
    int getStepForPatternIndex(int patternIndex) const
    {
        if (pattern.isEmpty() || patternIndex < 0)
            return 0;

        int stepCount = 0;
        int i = 0;
        while (i < pattern.length() && i < patternIndex)
            if (advanceOneToken(i))
                ++stepCount;

        return stepCount;
    }


    /**
        True if every pass through the pattern lasts the same number of steps.

        Probability groups break that: `p5 (1 2):(3 4 5)` runs two steps or
        three depending on the roll, and a bare `p5 (1 2)` runs two steps or
        one, so there is no fixed length to anchor a song position against.
        numSteps() reports the success branch, which is only one of the
        possible lengths. Single-step probability (`p5 1`, `p5 1:2`) is always
        one step and does not count.

        Patterns that fail this keep the step grid but not the bar alignment,
        until the rolls are made at the start of each pass rather than at the
        step (the pre-roll design in doc/architecture.md).
    */
    static bool patternHasFixedLength (const juce::String& p)
    {
        for (int i = 0; i < p.length(); ++i)
        {
            // A group fallback, whichever branch it belongs to.
            if (p[i] == ':' && i + 1 < p.length() && p[i + 1] == '(')
                return false;

            // A probability opening a group rather than tagging one step.
            if (p[i] == 'p')
            {
                int j = i + 2;   // skip 'p' and its digit
                while (j < p.length() && p[j] == ' ')
                    ++j;
                if (j < p.length() && p[j] == '(')
                    return false;
            }
        }
        return true;
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

        const int steps = numSteps();
        if (steps <= 0)
            return;

        const double songPosInSteps  = songPositionInSteps (positionInfo.ppqPosition);
        const double nextStepInSong  = std::ceil (songPosInSteps);

        // Grid: how far the next step boundary is from the start of this block.
        const double stepsUntilNext = nextStepInSong - songPosInSteps;
        const double secondsPerPPQ  = 60.0 / tempoBPM;   // 1.0 PPQ = one quarter
        samplesUntilNextNote = stepsUntilNext / getNoteDivisor() * secondsPerPPQ * sampleRate;

        // Phase: which step of the pattern belongs at that boundary. Aligning
        // the grid alone is not enough — it keeps steps on the beat but lets
        // the pattern sit at any rotation against the bar, so where a pattern
        // started depended on how many steps had been consumed since the
        // plugin loaded. Anchoring the phase to the song makes step n of the
        // pattern fall on song step n modulo the pattern length, always.
        if (!patternLengthIsFixed)
            return;   // see the comment on patternHasFixedLength()

        const int targetStep = patternStepForSongStep (nextStepInSong, steps);

        // Only on genuine drift. In the steady state pos is already at the
        // right step, and reassigning it would re-enter the step's leading
        // '(' or '"' and push a second scope entry every block.
        if (getStepForPatternIndex (pos) != targetStep)
        {
            pos = getPatternIndexForStep (targetStep);
            // Jumping abandons whatever blocks were open, so the modifier
            // state they were holding has to go with them.
            scopeStack.clear();
            probGroupStack.clear();
            inRootRelativeBlock = false;
            octave = baseOctave;
            patternVelocity = -1;
        }
    }

    /** Resets the arpeggiator's position to the beginning of the pattern.
        @return A reference to this engine's output buffer, holding a note-off
                for the note left ringing, if there was one. Valid until the
                next call to processBlock(), reset() or turnOff(). */
    const juce::MidiBuffer& reset(int midiChannel = 1, const juce::Optional<juce::AudioPlayHead::CurrentPositionInfo> positionInfo = {})
    {
        juce::MidiBuffer& noteOffBuffer = outMidi;
        noteOffBuffer.clear();
        if (lastPlayedMidiNote != -1)
        {
            noteOffBuffer.addEvent(juce::MidiMessage::noteOff(lastPlayedMidiChannel, lastPlayedMidiNote), 0);
            lastPlayedMidiNote = -1;
        }

        octave = baseOctave;
        // Drop any 'V' the pattern had set, so playback restarts following the
        // played velocity. playedVelocity itself survives: it belongs to the
        // note the player is holding, not to our position in the pattern.
        patternVelocity = -1;
        pos = 0;
        lastPlayedDegreeIndex = 0;
        samplesUntilNextNote = 0;
        scopeStack.clear();
        probGroupStack.clear();
        inRootRelativeBlock = false;

        // If host position is provided (i.e., transport just started), sync to it.
        // Unlike syncToPlayHead this lands on the step the position sits inside
        // rather than the next boundary, because it fires immediately.
        if (positionInfo.hasValue() && positionInfo->ppqPosition >= 0.0)
        {
            const int steps = numSteps();
            if (steps > 0 && patternLengthIsFixed)
            {
                const double songPosInSteps = songPositionInSteps (positionInfo->ppqPosition);
                pos = getPatternIndexForStep (patternStepForSongStep (std::floor (songPosInSteps), steps));
                samplesUntilNextNote = 0; // Trigger immediate evaluation for the current position
            }
        }

        return noteOffBuffer;
    }

    /** Generates a note-off for the last played note and resets the state.
        @return A reference to this engine's output buffer. Valid until the
                next call to processBlock(), reset() or turnOff(). */
    const juce::MidiBuffer& turnOff(int midiChannel = 1)
    {
        juce::MidiBuffer& noteOffBuffer = outMidi;
        noteOffBuffer.clear();
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
    // `velocity` here holds patternVelocity, so it may be -1: closing a block
    // that set a global 'V' hands control back to the played velocity.
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

    // Velocity comes from two independent sources. `playedVelocity` tracks the
    // incoming note-on and is what a pattern that says nothing about velocity
    // will use. `patternVelocity` is the 'V' override: -1 means "the pattern
    // has not set one", so the two never overwrite each other and a ')' can
    // restore the override to "unset" and fall back to the player again.
    int playedVelocity = 96;    // until a note has been played
    int patternVelocity = -1;   // -1 = unset, follow playedVelocity

    // Cached with the pattern: syncToPlayHead consults it every block, and it
    // only changes when the pattern does. See patternHasFixedLength().
    bool patternLengthIsFixed = true;

    int pos = 0;
    int lastPlayedMidiNote = -1;
    int lastPlayedMidiChannel = 1;
    int lastPlayedDegreeIndex = 0;
    int currentStepIndex = 0;

    /** Monotonic count of note-ons emitted, bumped on the audio thread and
        polled by the GUI (see getNoteOnCount). Copyable/movable on purpose:
        a bare std::atomic member would make Arpeggiator non-movable, and
        hosts keep instances in std::vector. */
    struct NoteOnCounter
    {
        NoteOnCounter() = default;
        NoteOnCounter (const NoteOnCounter& other)
            : value (other.value.load (std::memory_order_relaxed)) {}
        NoteOnCounter& operator= (const NoteOnCounter& other)
        {
            value.store (other.value.load (std::memory_order_relaxed),
                         std::memory_order_relaxed);
            return *this;
        }

        void bump() noexcept   { value.fetch_add (1, std::memory_order_relaxed); }
        uint32_t get() const noexcept { return value.load (std::memory_order_relaxed); }

        std::atomic<uint32_t> value { 0 };
    };

    NoteOnCounter noteOnCounter;

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

    // Output buffers, reused across calls so that nothing allocates on the
    // audio thread. Sized in prepareToPlay(); see the note above processBlock()
    // for the lifetime rule this places on callers.
    //
    // Two of them rather than one, because processBlock() merges getNext()'s
    // result into its own buffer, and addEvents() from a buffer into itself
    // would be self-aliasing. outMidi is shared by processBlock(), reset() and
    // turnOff(), which are never in flight at the same time.
    juce::MidiBuffer outMidi;
    juce::MidiBuffer stepMidi;
};

} // namespace fxme
