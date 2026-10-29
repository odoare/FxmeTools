/*
  ------------------------------------------------------------------------------
    Arpeggiator.h

    Text-pattern MIDI arpeggiator engine. Moved here from the CppMusicTools
    repository, now living in the `fxme` namespace.

    Framework-free: patterns are std::string, the output is a list of
    ArpMidiEvent (a sample offset, a channel, a note, a velocity) that the host
    turns into whatever MIDI type it uses.

    The engine does not read the pattern's text itself. It is a player of the
    pattern language's passes (PatternLanguage.h): the text is compiled to a
    pattern::Program, the program realizes one pass at a time (timed events with
    notes as scale degrees), and the engine maps the degrees to MIDI notes
    through its chords and plays the events on its own clock. Random choices
    ('?', 'pN', 'v?', 'o?') are draws of the pass's seed: setSeed() makes them
    reproducible.

    Two syntaxes: 2 is TeAr's (the default, so existing presets play exactly as
    before), 3 is its superset (chords, local note values, generators, repeats,
    variations by pass, gates, expressions). See setPatternSyntax().

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/midi/MidiTools.h>
#include <FxmeTools/midi/PatternLanguage.h>
#include <FxmeTools/util/ArrayView.h>
#include <FxmeTools/util/Math.h>
#include <FxmeTools/util/Random.h>
#include <FxmeTools/util/StringRef.h>
#include <FxmeTools/util/StringUtils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fxme
{

/** One MIDI event of an arpeggiator, at a sample offset in the block. */
struct ArpMidiEvent
{
    int sample = 0;          ///< offset in the block
    int channel = 1;         ///< 1..16
    int note = 0;            ///< 0..127
    int velocity = 0;        ///< 1..127 for a note-on, 0 for a note-off
    bool noteOn = false;
};

/**
    A base class for creating MIDI arpeggiators.

    This class takes a Chord, an octave, and a pattern string to generate
    a sequence of MIDI notes. processBlock() plays the pattern and returns MIDI
    messages for note-on and note-off events.

    Degrees and velocity levels are written as a single uppercase hexadecimal
    character ('0'-'9' then 'A'-'F') since syntax version 2. Octave (0-7) and
    probability (0-9) stay decimal. See migratePatternV1toV2() for what changed.

    The pattern string consists of characters that define the arpeggio's behavior at each step
    (syntax 2; syntax 3 is written in Otomatotamoto's doc/pattern-language.md):
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

    Explicit degrees beyond the chord's size ('8' on a seven-note scale):
    by default they wrap into the same octave; with setOctaveWrap (true) they
    continue upward, '8' being the first degree an octave higher. Only explicit
    degrees can exceed the size: '+', '-' and '?' stay cyclic within the chord.

    Time. A pass is played in beats: an event starts at its onset and stops at
    onset + length, so the notes of a chord sound together and are released
    together, and a gate (syntax 3) releases a note inside its step. The engine
    counts samples from the sample rate and the tempo (samplesPerBeat =
    sampleRate * 60 / tempo). A step is the subdivision unless the text says
    otherwise (`m` in syntax 3). Glides (`~`) and hits (`x`, no note) play no
    note here.

    Real time. processBlock(), reset(), turnOff(), syncToPlayHead() and
    setProgram() do not allocate once prepareToPlay() was called and the pass
    was reserved for the program (reservePass(), or by setPattern() /
    setProgram() beforehand). setPattern() compiles: not for the audio thread.
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
        v = fxme::jlimit (0, 15, v);
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
        return fxme::jlimit (1, 127, fxme::roundToInt (
            (float) level * 127.0f / (float) maxVelocityLevel));
    }

    /** Nearest level to a MIDI velocity: the inverse of velocityForLevel, used
        by the relative commands ('v+', 'V-') to step from whatever velocity is
        currently in effect. Floors at 1 so a run of 'V-' cannot silence the
        pattern outright. */
    static int levelForVelocity (int velocity) noexcept
    {
        return fxme::jlimit (1, maxVelocityLevel, fxme::roundToInt (
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
    static std::string migratePatternV1toV2 (StringRef oldPattern)
    {
        // Old level -> nearest new level, indexed 0-9, via the velocity each
        // produced: round(min(127, n * 16) * 15 / 127). Level 0 means "no
        // velocity change" in both versions, and old 8 and 9 both clamped to
        // 127, so both land on F.
        static const char* const velocityMap = "024689BDFF";

        std::string result;
        result.reserve (oldPattern.size());

        const int oldLength = (int) oldPattern.size();
        int i = 0;
        while (i < oldLength)
        {
            const char c = oldPattern[(std::size_t) (i)];

            if (c == 'v' || c == 'V')
            {
                result += c;
                if (i + 1 < oldLength)
                {
                    const char arg = oldPattern[(std::size_t) (i + 1)];
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
                if (i + 1 < oldLength)
                    result += oldPattern[(std::size_t) (i + 1)];
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
        : chord(MidiTools::Chord("CM"))
    {
        initialise ("012");
    }

    /**
        Constructs an Arpeggiator.
        @param initialChord The chord to be arpeggiated.
        @param arpPattern The string pattern defining the arpeggio.
        @param baseOctave The starting MIDI octave.
    */
    Arpeggiator(const MidiTools::Chord& initialChord, StringRef arpPattern, int initialOctave)
        : chord(initialChord), baseOctave(initialOctave)
    {
        initialise (arpPattern);
    }

    virtual ~Arpeggiator() = default;

    /**
        Call this before playback to set the sample rate.
        @param rate The host's sample rate.
    */
    void prepareToPlay(double rate)
    {
        sampleRate = rate;
        updateSamplesPerBeat();

        // Grow the output vector here, once, so that the audio thread never
        // has to. clear() keeps the capacity, so from here on the
        // clear-and-refill cycle allocates nothing at all.
        //
        // 512 events is well beyond what one block of the fastest subdivision
        // can produce; if some pathological block ever exceeds it, the vector
        // grows once and then stays grown.
        outEvents.reserve (512);
    }

    //=========================================================================
    // processBlock(), reset() and turnOff() all return a view over one
    // vector owned by this engine, rather than a fresh container by value.
    // Returning by value would mean a heap allocation on the audio thread
    // every time a step fired, which is what this avoids.
    //
    // The lifetime rule that comes with it: the returned view is only valid
    // until the next call to any of the three. Consume it straight away, which
    // is what the natural call shape already does:
    //
    //     for (const auto& e : arp.processBlock (n, ch))
    //         send (e);
    //
    // Copy the events out if they must outlive the next call. One engine per
    // voice, as the callers already do, means the three never overlap.
    //=========================================================================

    /**
        Generates MIDI events for the current block of audio samples.
        @param numSamples The number of samples in the current audio block.
        @return A view over this engine's output events, in time order (at one
                sample, note-offs before note-ons), holding any events
                generated during this block. Valid until the next call to
                processBlock(), reset() or turnOff().
    */
    ArrayView<ArpMidiEvent> processBlock(int numSamples, int midiChannel = 1)
    {
        outEvents.clear();
        if (sampleRate <= 0.0 || samplesPerBeat <= 0.0 || program == nullptr || ! program->ok())
            return outEvents;
        if (midiChannel < 1 || midiChannel > 16) midiChannel = 1;

        int time = 0;
        while (time < numSamples)
        {
            // Everything due at this sample: note-offs first, then the step. A
            // step that is late by more than its own length fires again, at
            // the same sample, up to a bound.
            for (int guard = 0; guard < 64; ++guard)
            {
                const bool stepDue = samplesUntilNextStep <= timeTolerance;
                // A new pass first: its tied first steps keep the notes that reach the boundary
                // from being released.
                if (stepDue && (! haveCurrentPass || nextStep >= (int) currentPass.steps.size()))
                    startPass();
                bool did = releaseDueNotes (time);
                if (stepDue)
                {
                    fireStep (time, midiChannel);
                    did = true;
                }
                if (! did)
                    break;
            }

            // Sleep until the next thing: the step boundary or a note-off.
            double wait = samplesUntilNextStep;
            for (int i = 0; i < soundingCount; ++i)
                wait = std::min (wait, samplesUntilNextStep + sounding[(std::size_t) i].endRel * samplesPerBeat);

            // Always advance, even if the wait rounds to nothing.
            const int advance = fxme::jmin (numSamples - time, fxme::jmax (1, (int) std::ceil (wait - timeTolerance)));
            time += advance;
            samplesUntilNextStep -= advance;
        }
        return outEvents;
    }

    /** Returns the number of samples remaining until the next step boundary. */
    double getSamplesUntilNextNote() const
    {
        return samplesUntilNextStep;
    }

    /** Sets the number of samples remaining until the next step boundary. */
    void setSamplesUntilNextNote(double samples)
    {
        samplesUntilNextStep = samples;
    }

    // --- Setters for properties ---
    void setChord(const MidiTools::Chord& newChord)
    {
        chord = newChord;
    }

    /**
        Sets the pattern from its text and plays it from its start: the text is
        compiled with the engine's syntax (setPatternSyntax()) and a step the
        length of the subdivision (setSubdivision()). Not for the audio thread
        (it allocates).

        A text that does not compile (syntax 3 reports errors) leaves the
        previous program playing; getDiagnostics() says why. getPattern()
        returns the new text either way, so that an editor shows what was typed.
    */
    void setPattern(StringRef newPattern)
    {
        pattern.assign (newPattern.data(), newPattern.length());
        compilePattern();
    }

    /**
        Plays a program compiled elsewhere (the pattern language's compile()),
        from its start. For hosts that compile on another thread and swap on
        the audio thread: nothing here allocates or frees when the pass was
        reserved for the program (reservePass()).

        The program carries its syntax and its step (defaultStepBeats), so the
        engine takes both from it: setSubdivision() and setPatternSyntax() no
        longer apply to it. Its text is what getPattern() returns.

        @return The program that was playing. It is moved out rather than
                released here: the host keeps it and lets it go on its own
                thread, so that no program is ever freed by the audio thread.
                (Ignoring the result frees it here, which is harmless unless
                this was its last owner.)
    */
    std::shared_ptr<const pattern::Program> setProgram (std::shared_ptr<const pattern::Program> newProgram)
    {
        std::shared_ptr<const pattern::Program> previous = std::move (program);
        program = std::move (newProgram);
        programFromText = false;
        if (program != nullptr)
        {
            patternSyntax = program->options().syntax < 3 ? 2 : 3;
            reservePass (program->info());
        }
        restartProgram();
        return previous;
    }

    /** The program being played (null before any pattern). */
    std::shared_ptr<const pattern::Program> getProgram() const { return program; }

    /** Makes room for the passes of a program with this Info, so that a later swap
        allocates nothing. Not for the audio thread. setProgram() and setPattern()
        do it for their own program. */
    void reservePass (const pattern::Info& info)
    {
        currentPass.reserve (info);
    }

    /** The compiler's diagnostics for the last text given to setPattern() (errors and
        warnings with their character positions), or for the program given to setProgram(). */
    const std::vector<pattern::Diagnostic>& getDiagnostics() const
    {
        return (programFromText || program == nullptr) ? diagnostics : program->diagnostics();
    }

    /** The syntax setPattern() compiles with: 2 (TeAr's, the default) or 3 (the superset). Changing
        it compiles the current text again and restarts the pattern. */
    void setPatternSyntax (int syntax)
    {
        const int s = syntax < 3 ? 2 : 3;
        if (s == patternSyntax)
            return;
        patternSyntax = s;
        if (programFromText)
            compilePattern();
    }

    int getPatternSyntax() const noexcept { return patternSyntax; }

    /** Values between single quotes in syntax 3 ('64*energy') are evaluated by the host. Given to
        setPattern()'s compiler; it must outlive the programs. */
    void setExpressions (pattern::Expressions* e) noexcept { expressions = e; }

    void setOctave(int newOctave) { carry.octave = fxme::jlimit(0, 7, newOctave); }
    void setPlayNoteOffMode(StringRef mode) { playNoteOff = mode.str(); }

    /** Seeds the draws behind every random choice of the pattern ('?',
        'pN', 'v?', 'o?', generators), so that a host can make them
        reproducible: the same seed and the same calls give the same events.
        Pass k draws from a seed made of this one and k, and the count of
        passes starts again: a host that reseeds at a cycle's start gets
        identical passes. Without a call, the engine seeds itself randomly
        (and keeps going on reset(): each restart draws differently). */
    void setSeed (std::int64_t seed) noexcept
    {
        rng.setSeed (seed);
        baseSeed = static_cast<std::uint64_t> (seed);
        seedCounter = 0;
        passIndex = 0;
    }

    /** When on, an explicit degree beyond the chord's size continues upward
        ('8' on a seven-note scale is its first degree an octave higher) instead
        of wrapping into the same octave. Off by default. */
    void setOctaveWrap (bool on) noexcept { octaveWrap = on; }
    void setTempo(double newTempoBPM)
    {
        tempoBPM = newTempoBPM > 0 ? newTempoBPM : 120.0;
        updateSamplesPerBeat();
    }

    /** The step's length (index 4 = 1/16, see getNoteDivisor()). A program compiled from a text
        by setPattern() is compiled again with it, from its start; a program given to setProgram()
        keeps its own. */
    void setSubdivision(int subdivisionIndex)
    {
        subdivision = subdivisionIndex;
        updateSamplesPerBeat();
        if (programFromText && program != nullptr && std::abs (program->options().defaultStepBeats - 1.0 / getNoteDivisor()) > 1.0e-12)
            compilePattern();
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
        newBaseOctave = fxme::jlimit(0, 7, newBaseOctave);

        int diff = newBaseOctave - baseOctave;
        baseOctave = newBaseOctave;
        // Adjust the current octave by the difference to maintain relative shifts
        if (carry.octave >= 0)
            carry.octave = fxme::jlimit(0, 7, carry.octave + diff);
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
        pattern's 'V' if one is in effect here (syntax 2), else the played velocity. */
    int effectiveVelocity() const noexcept
    {
        return (carry.velocity >= 0.0f) ? fxme::jlimit (1, 127, (int) std::lround (carry.velocity * 127.0f))
                                        : playedVelocity;
    }


    /**
        Generates a Euclidean pattern string.
        @param hits Number of notes.
        @param steps Total length of the sequence.
    */
    /** Both pattern generators are static: they are pure functions of their
        arguments (makeRandomPattern also of the global RNG) and read no member
        state, so they need no instance.

        This is not cosmetic. Callers used to reach them through an
        Arpeggiator reference, and at least one held that reference in a lambda
        that outlived the call, which made it a dangling-reference hazard for
        no benefit. Being static removes the reason to hold a reference at all.

        Calling a static member through an instance stays legal, so existing
        `arp.makeRandomPattern()` call sites keep compiling unchanged. */
    static std::string makeEuclidianPattern(int hits, int steps, int rotation)
    {
        auto bools = MidiTools::euclidianRythm(hits, steps, rotation);
        std::string s;
        for (bool b : bools)
            s += (b ? "1 " : ". ");
        return trim(s);
    }

    /**
        Generates a random pattern string without applying it.
        Rules:
        - It shouldn't start with a "_"
        - Balanced global relative modifiers (O+, V+, O-, V-) to prevent drift.
        - Note values between 1 and 9.
    */
    /** Static for the same reason as makeEuclidianPattern above. Draws from `r`
        (the shared generator by default; pass a seeded Random for a
        reproducible pattern). */
    static std::string makeRandomPattern (Random& rng = systemRandom())
    {
        int length = rng.nextInt(13) + 4; // Random length between 4 and 16

        struct Step {
            std::string prefixes;
            std::string note;
        };
        std::vector<Step> steps;

        for (int i = 0; i < length; ++i)
        {
            Step s;
            int r = rng.nextInt(100);

            if (i == 0)
            {
                // First step: ensure it's not a sustain ('_') or rest ('.') for a strong start
                if (r < 60) s.note = std::to_string(rng.nextInt(5) + 1); // Start with 1-5
                else if (r < 80) s.note = "+";
                else s.note = "?";
            }
            else
            {
                if (r < 40) s.note = std::to_string(rng.nextInt(9) + 1); // 1-9
                else if (r < 55) s.note = "_";
                else if (r < 65) s.note = ".";
                else if (r < 75) s.note = "+";
                else if (r < 85) s.note = "-";
                else if (r < 90) s.note = "?";
                else s.note = "=";
            }
            steps.push_back(s);
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
                else s.prefixes += "o" + std::to_string(rng.nextInt(3) + 3); // o3-o5
            }
            
            // Local Pitch
            if (rng.nextFloat() < 0.1f)
                s.prefixes += (rng.nextBool() ? "#" : "b");

            // Local Velocity: an accent, so the top half of the range. Levels
            // are hex since syntax v2, so v9-vF is what v5-v8 used to be.
            if (rng.nextFloat() < 0.1f)
            {
                s.prefixes += 'v';
                s.prefixes += valueChar(rng.nextInt(7) + 9); // v9-vF
            }
        }

        // Add Global Modifiers (Balanced)
        auto addBalancedGlobal = [&](const char* plus, const char* minus) {
            int idx1 = rng.nextInt(length);
            int idx2 = rng.nextInt(length);
            while (idx1 == idx2) idx2 = rng.nextInt(length);
            steps[(std::size_t) idx1].prefixes += plus;
            steps[(std::size_t) idx2].prefixes += minus;
        };

        if (rng.nextFloat() < 0.3f) addBalancedGlobal("O+", "O-");
        if (rng.nextFloat() < 0.3f) addBalancedGlobal("V+", "V-");

        std::string newPattern;
        for (const auto& s : steps)
            newPattern += s.prefixes + s.note + " ";

        return trim(newPattern);
    }

    void randomize()
    {
        setPattern(makeRandomPattern());
    }

    /** Returns the current pattern string. */
    const std::string& getPattern() const
    {
        return text();
    }

    /** Returns a const reference to the currently active chord. */
    const MidiTools::Chord& getChord() const
    {
        return chord;
    }

    /** Returns the index of the step that played last. Syntax 2: its number among the text's
        musical steps (what getPatternIndexForStep() and getStepForPatternIndex() count, so a step
        inside a probability fallback group reports the group's); syntax 3: its index in the pass's
        steps. */
    int getCurrentStepIndex() const
    {
        if (shownStep < 0)
            return 0;
        return patternSyntax < 3 ? getStepForPatternIndex (shownSourceStart) : shownStep;
    }

    /** The characters [start, end) of the text of the step that played last (prefixes
        included), for highlighting it; {-1, -1} before any step. */
    std::pair<int, int> getCurrentStepSource() const
    {
        if (shownStep < 0)
            return { -1, -1 };
        return { shownSourceStart, shownSourceEnd };
    }

    /** Returns the last MIDI note number that was played and is still sounding; -1 if none. */
    int getLastPlayedNote() const
    {
        return lastPlayedMidiNote;
    }

    /** Monotonic count of note-ons emitted since construction. Poll it from a
        GUI timer and compare with the previous reading to detect that this
        arpeggiator fired: a change means at least one note started since the
        last poll. Wrapping after 2^32 notes is harmless, since only
        inequality is meaningful. Safe to call from any thread. */
    std::uint32_t getNoteOnCount() const noexcept
    {
        return noteOnCounter.get();
    }

private:
    /**
        Advances `i` past exactly one token of the text and reports whether
        that token was a musical step (syntax 2's reading of the text, behind
        numSteps() and its inverses).

        The step queries below are the same walk asked different questions, so
        they share this one definition. They used to carry a copy each, and the
        copies had drifted: one of them counted '"' as a step while the others
        treated it as a block marker, which made the playing-step highlight
        slide out of position in any pattern using root-relative blocks.
    */
    bool advanceOneToken(int& i) const
    {
        const std::string& p = text();
        const char command = p[static_cast<std::size_t> (i)];
        const int length = (int) p.length();

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
            if (i < length && p[static_cast<std::size_t> (i)] == '(')
            {
                int depth = 1; i++;
                while (i < length && depth > 0)
                {
                    if (p[static_cast<std::size_t> (i)] == '(') ++depth;
                    else if (p[static_cast<std::size_t> (i)] == ')') --depth;
                    i++;
                }
            }
            else if (i < length)
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
    /** The number of musical steps. Syntax 2: counted in the text (a probability group counts its
        success branch, whatever pass is playing). Syntax 3: the steps of the pass being played,
        or before any pass the most a pass can have. */
    int numSteps() const
    {
        if (patternSyntax >= 3)
        {
            if (haveCurrentPass)
                return (int) currentPass.steps.size();
            return program != nullptr ? program->info().maxStepsPerPass : 0;
        }

        const std::string& p = text();
        int steps = 0;
        int i = 0;
        while (i < (int) p.length())
            if (advanceOneToken(i))
                ++steps;
        return steps;
    }

    /** Given a step index (0, 1, 2...), find the corresponding character index
        in the pattern string. The index returned is the start of the whole
        step, prefixes and block markers included, so that highlighting a step
        highlights its modifiers too. Syntax 3: the start of the step in the
        pass being played (0 before one). */
    int getPatternIndexForStep(int stepIndex) const
    {
        if (patternSyntax >= 3)
        {
            if (haveCurrentPass && stepIndex >= 0 && stepIndex < (int) currentPass.steps.size())
                return currentPass.steps[static_cast<std::size_t> (stepIndex)].sourceStart;
            return 0;
        }

        const std::string& p = text();
        if (p.empty())
            return 0;

        int currentStepCount = 0;
        int i = 0;
        while (i < (int) p.length())
        {
            if (currentStepCount == stepIndex)
                return i; // Found the start of the desired step

            if (advanceOneToken(i))
                ++currentStepCount;
        }
        return 0; // Fallback if stepIndex is out of bounds
    }

    /** Given a character index in the pattern string, find the corresponding
        musical step index (syntax 2's count). Exact inverse of getPatternIndexForStep() there. */
    int getStepForPatternIndex(int patternIndex) const
    {
        const std::string& p = text();
        if (p.empty() || patternIndex < 0)
            return 0;

        int stepCount = 0;
        int i = 0;
        while (i < (int) p.length() && i < patternIndex)
            if (advanceOneToken(i))
                ++stepCount;

        return stepCount;
    }

    /**
        True if every pass through a syntax-2 text lasts the same number of steps.

        Probability groups break that: `p5 (1 2):(3 4 5)` runs two steps or
        three depending on the roll, and a bare `p5 (1 2)` runs two steps or
        one, so there is no fixed length to anchor a song position against.
        Single-step probability (`p5 1`, `p5 1:2`) is always one step and does
        not count. See hasFixedLength() for the program being played.
    */
    static bool patternHasFixedLength (StringRef p)
    {
        for (int i = 0; i < (int) p.length(); ++i)
        {
            // A group fallback, whichever branch it belongs to.
            if (p[(std::size_t) (i)] == ':' && i + 1 < (int) p.length() && p[(std::size_t) (i + 1)] == '(')
                return false;

            // A probability opening a group rather than tagging one step.
            if (p[(std::size_t) (i)] == 'p')
            {
                int j = i + 2;   // skip 'p' and its digit
                while (j < (int) p.length() && p[(std::size_t) (j)] == ' ')
                    ++j;
                if (j < (int) p.length() && p[(std::size_t) (j)] == '(')
                    return false;
            }
        }
        return true;
    }

    /** True if every pass of the program being played lasts the same time, so that a song
        position can anchor it (syncToPlayHead()). */
    bool hasFixedLength() const
    {
        return program == nullptr || program->info().fixedLength;
    }

    /** Returns the total duration of one full pattern loop in PPQ (syntax 2: from the text's
        steps; syntax 3: the program's fixed length). */
    double ppqDuration() const
    {
        if (patternSyntax >= 3)
            return program != nullptr ? program->info().lengthBeats : 0.0;

        const int steps = numSteps();
        if (steps == 0)
            return 0.0;

        // The duration of one step in PPQ is 1.0 / notesPerQuarter.
        return steps / getNoteDivisor();
    }

    /**
        Synchronizes the arpeggiator's internal clock to the host's transport position.
        This should be called on every process block while the host is playing.
        @param ppqPosition The host's position in quarter notes; negative means unknown.
    */
    void syncToPlayHead(double ppqPosition)
    {
        if (samplesPerBeat <= 0.0 || ppqPosition < 0.0 || program == nullptr || ! program->ok())
            return;

        const pattern::Info& info = program->info();
        const double grid = info.shortestStepBeats > 0.0 ? info.shortestStepBeats : 1.0 / getNoteDivisor();

        if (! info.fixedLength || info.lengthBeats <= 0.0)
        {
            // No single length to anchor a position against (see patternHasFixedLength()):
            // the step grid is placed, the pass is left where it is.
            const double raw = ppqPosition / grid;
            const double nearest = std::round (raw);
            const double steps = (std::abs (raw - nearest) < 1.0e-6) ? nearest : raw;
            samplesUntilNextStep = (std::ceil (steps) - steps) * grid * samplesPerBeat;
            return;
        }

        if (! haveCurrentPass)
            startPass();
        const int count = (int) currentPass.steps.size();
        if (count == 0)
            return;

        // Where the position sits in the pass, and which step boundary comes next. Aligning the
        // grid alone is not enough: it keeps steps on the beat but lets the pattern sit at any
        // rotation against the bar, so where a pattern started would depend on how many steps
        // had been consumed since the plugin loaded. Anchoring the phase to the song makes the
        // pass start on every multiple of its length, always.
        const double length = info.lengthBeats;
        const double eps = 1.0e-6 * grid;
        double pos = std::fmod (ppqPosition, length);
        if (pos < eps) pos = 0.0;
        else if (length - pos < eps) pos = length;

        int target = count;
        for (int i = 0; i < count; ++i)
            if (currentPass.steps[(std::size_t) i].onset >= pos - eps) { target = i; break; }

        const double boundary = target < count ? currentPass.steps[(std::size_t) target].onset : length;
        double wait = boundary - pos;
        if (wait < eps) wait = 0.0;
        samplesUntilNextStep = wait * samplesPerBeat;

        // The pass boundary is both the end of a pass and the start of the next one.
        const bool atEdge = pos == 0.0 || pos == length;
        const bool inPlace = target == nextStep || (atEdge && (nextStep == 0 || nextStep == count));

        // Only on genuine drift. In the steady state the next step is already the right one.
        if (! inPlace)
        {
            jumpToStep (target);
            // Jumping abandons whatever the passes before were holding in scope.
            carry = pattern::Carry {};
        }
    }

    /** Resets the arpeggiator's position to the beginning of the pattern.
        @param ppqPosition The host's position in quarter notes when the
                transport has just started, or a negative value for none.
        @return A view over this engine's output events, holding a note-off
                for every note left ringing. Valid until the next call to
                processBlock(), reset() or turnOff(). */
    ArrayView<ArpMidiEvent> reset(int midiChannel = 1, double ppqPosition = -1.0)
    {
        // Kept in the signature for source compatibility, but unused: the
        // note-offs below go out on the channel each note was played on.
        (void) midiChannel;

        outEvents.clear();
        releaseAllNotes();
        restartPlayback();

        // If host position is provided (i.e., transport just started), sync to it.
        // Unlike syncToPlayHead this lands on the step the position sits inside
        // rather than the next boundary, because it fires immediately.
        if (ppqPosition >= 0.0 && program != nullptr && program->ok())
        {
            const pattern::Info& info = program->info();
            if (info.fixedLength && info.lengthBeats > 0.0)
            {
                startPass();
                const double eps = 1.0e-6 * (info.shortestStepBeats > 0.0 ? info.shortestStepBeats : 1.0 / getNoteDivisor());
                const double pos = std::fmod (ppqPosition, info.lengthBeats);
                int inside = 0;
                for (int i = 0; i < (int) currentPass.steps.size(); ++i)
                    if (currentPass.steps[(std::size_t) i].onset <= pos + eps)
                        inside = i;
                jumpToStep (inside);
                samplesUntilNextStep = 0;   // Trigger immediate evaluation for the current position
            }
        }

        return outEvents;
    }

    /** Releases every note that sounds and resets the state.
        @return A view over this engine's output events. Valid until the
                next call to processBlock(), reset() or turnOff(). */
    ArrayView<ArpMidiEvent> turnOff(int midiChannel = 1)
    {
        (void) midiChannel;   // see reset() above

        outEvents.clear();
        releaseAllNotes();
        // Also reset pattern position and other state variables for a clean start next time
        // (the step grid keeps going).
        restartProgram();
        return outEvents;
    }

protected:
    // Root-relative mode: inside " " blocks in single-note mode (chordMethod==2),
    // notes are resolved against rootChord (degree-0 of the scale) rather than
    // the chord built from the pressed MIDI note's degree.
    MidiTools::Chord rootChord { "" };

    /**
        Finds a valid semitone for a degree index in `activeChord`, handling absent notes.
        For "Notes played" mode, it returns a semitone (0-11).
        For "Chord played as is" mode, it returns a full MIDI note number (0-127).
    */
    int getNoteForDegree(const MidiTools::Chord& activeChord, int degreeIndex) const
    {
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

            // With octave wrap, an explicit degree past the end continues one
            // octave up per lap of the chord instead of folding back.
            int octaveShift = 0;
            if (!fxme::isPositiveAndBelow(degreeIndex, degrees.size()))
            {
                if (octaveWrap && degreeIndex >= degrees.size())
                    octaveShift = 12 * (degreeIndex / degrees.size());
                degreeIndex %= degrees.size();
            }

            const int note = resolveScaleDegree (activeChord, degreeIndex);
            return (note != -1) ? note + octaveShift : note;
        }
    }

    /** The scale-mode half of getNoteForDegree, for an index already inside the chord. */
    int resolveScaleDegree (const MidiTools::Chord& activeChord, int degreeIndex) const
    {
        const auto& degrees = activeChord.getDegrees();
        {
            if (activeChord.getName() == "Custom")
            {
                const auto playedNotes = activeChord.getSortedSet();
                const int numPlayedNotes = static_cast<int> (playedNotes.size());
                if (numPlayedNotes > 0)
                    return playedNotes[static_cast<std::size_t> (degreeIndex % numPlayedNotes)];
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

    MidiTools::Chord chord;
    std::string pattern;             ///< the text given to setPattern()

    int baseOctave = 4;
    std::string playNoteOff = "Next"; // "Off", "Next", "Previous"
    int chordMethod = 0; // 0: Notes played, 1: Chord played as is, 2: Single note

    // Velocity comes from two independent sources. `playedVelocity` tracks the
    // incoming note-on and is what a pattern that says nothing about velocity
    // will use. The pattern's own 'V' lives in `carry` (syntax 2), so the two
    // never overwrite each other and a ')' can restore the override to "unset"
    // and fall back to the player again.
    int playedVelocity = 96;    // until a note has been played

    // The generator of the engine's random seed, and what setSeed() reaches.
    Random rng;

    // See setOctaveWrap().
    bool octaveWrap = false;

    int lastPlayedMidiNote = -1;

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
        std::uint32_t get() const noexcept { return value.load (std::memory_order_relaxed); }

        std::atomic<std::uint32_t> value { 0 };
    };

    NoteOnCounter noteOnCounter;

private:
    //==========================================================================
    // The player. `currentPass` is the realized pass being played, `nextStep` the
    // step that starts at the next boundary (== the number of steps when the pass is over:
    // the boundary then starts the next pass). `samplesUntilNextStep` counts down to that
    // boundary. A sounding note remembers when it ends in beats *after that boundary*
    // (negative: before it), so it needs no correction when the pass changes.

    static constexpr int maxSounding = 32;            ///< notes that sound at once (chords, ties)
    static constexpr double timeTolerance = 1.0e-6;   ///< samples; beats for the ends of notes

    struct Sounding { int note = 0; int channel = 1; double endRel = 0.0; };

    void initialise (StringRef text)
    {
        baseSeed = pattern::mix (static_cast<std::uint64_t> (rng.getSeed()));
        outEvents.reserve (512);
        pattern.assign (text.data(), text.length());
        compilePattern();
    }

    const std::string& text() const noexcept
    {
        return (programFromText || program == nullptr) ? pattern : program->text();
    }

    /** Compiles `pattern` with the engine's syntax and step; an invalid text keeps the previous program. */
    void compilePattern()
    {
        pattern::Options options;
        options.syntax = patternSyntax;
        options.defaultStepBeats = 1.0 / getNoteDivisor();
        options.expressions = expressions;
        auto compiled = std::make_shared<pattern::Program> (pattern::compile (pattern, options));
        diagnostics = compiled->diagnostics();
        if (! compiled->ok() && program != nullptr)
            return;
        reservePass (compiled->info());
        program = std::move (compiled);
        programFromText = true;
        restartProgram();
    }

    /** A new program starts from its first pass, with the state of a fresh start (the notes
        that sound keep their ends). */
    void restartProgram()
    {
        carry = pattern::Carry {};
        passIndex = 0;
        haveCurrentPass = false;
        nextStep = 0;
        nextEvent = 0;
    }

    /** Back to the start for good: the notes were released by the caller. */
    void restartPlayback()
    {
        restartProgram();
        samplesUntilNextStep = 0;
    }

    void jumpToStep (int step)
    {
        nextStep = step;
        nextEvent = 0;
        const int n = (int) currentPass.events.size();
        while (nextEvent < n && currentPass.events[(std::size_t) nextEvent].firstStep < step)
            ++nextEvent;
    }

    //--------------------------------------------------------------------------
    pattern::Context makeContext() const
    {
        pattern::Context c;
        c.seed = pattern::mix (baseSeed ^ pattern::mix (seedCounter));
        c.passIndex = passIndex;
        c.baseOctave = baseOctave;
        c.hostVelocity = (float) playedVelocity / 127.0f;

        const auto& degrees = chord.getDegrees();
        int size = degrees.size();
        std::uint32_t present = 0;
        if (chordMethod == 1)
        {
            // The notes themselves are the degrees ('+' and '-' keep cycling in the chord's
            // degree slots, as they always did).
            const int raw = chord.getRawNotes().size();
            size = fxme::jmax (size, raw);
            for (int d = 0; d < fxme::jmin (raw, 32); ++d)
                present |= 1u << d;
        }
        else
        {
            for (int d = 0; d < fxme::jmin (size, 32); ++d)
                if (degrees[d] != -1)
                    present |= 1u << d;
        }
        c.chordSize = size;
        c.chordPresent = present;
        c.keySize = (chordMethod == 2 ? rootChord : chord).getDegrees().size();
        return c;
    }

    /** Realizes the next pass and starts at its first step. At a pass boundary, a first step
        that is a tie (a hold with nothing to hold in its own pass) keeps the notes that reach
        the boundary sounding. */
    void startPass()
    {
        const bool continuing = haveCurrentPass;
        program->realize (makeContext(), currentPass, &carry);
        ++passIndex;
        ++seedCounter;
        haveCurrentPass = true;
        nextStep = 0;
        nextEvent = 0;

        if (! continuing)
            return;
        double tied = 0.0;
        for (const pattern::Step& s : currentPass.steps)
        {
            if (! s.tied)
                break;
            tied += s.duration;
        }
        if (tied > 0.0)
            for (int i = 0; i < soundingCount; ++i)
                if (sounding[(std::size_t) i].endRel >= -timeTolerance)
                    sounding[(std::size_t) i].endRel += tied;
    }

    /** The step boundary has come (and its pass is ready): start its events and set the next boundary. */
    void fireStep (int time, int midiChannel)
    {
        if (currentPass.steps.empty())
        {
            samplesUntilNextStep += samplesPerBeat;   // nothing to play: look again in a beat
            return;
        }

        const int s = nextStep++;
        const pattern::Step& step = currentPass.steps[(std::size_t) s];
        shownStep = s;
        shownSourceStart = step.sourceStart;
        shownSourceEnd = step.sourceEnd;

        const int numEvents = (int) currentPass.events.size();
        while (nextEvent < numEvents && currentPass.events[(std::size_t) nextEvent].firstStep < s)
            ++nextEvent;
        while (nextEvent < numEvents && currentPass.events[(std::size_t) nextEvent].firstStep == s)
            startEvent (currentPass.events[(std::size_t) nextEvent++], time, midiChannel);

        const double duration = std::max (step.duration, 1.0e-9);
        samplesUntilNextStep += duration * samplesPerBeat;
        for (int i = 0; i < soundingCount; ++i)
            sounding[(std::size_t) i].endRel -= duration;
    }

    void startEvent (const pattern::Event& e, int time, int midiChannel)
    {
        if (e.length <= 0.0)
            return;
        for (int k = 0; k < e.noteCount; ++k)
        {
            const int midi = midiNoteFor (currentPass.notes[(std::size_t) (e.firstNote + k)]);
            // A note outside 0..127 is not emitted, and is not remembered either:
            // there would be no note-off to send for it.
            if (midi < 0 || midi > 127 || soundingCount >= maxSounding)
                continue;
            bool again = false;
            for (int i = 0; i < soundingCount; ++i)
                again = again || (sounding[(std::size_t) i].note == midi && sounding[(std::size_t) i].channel == midiChannel);
            if (again)
                continue;

            const int velocity = e.velocity < 0.0f ? playedVelocity
                                                   : fxme::jlimit (1, 127, (int) std::lround (e.velocity * 127.0f));
            outEvents.push_back ({ time, midiChannel, midi, velocity, true });
            sounding[(std::size_t) soundingCount++] = { midi, midiChannel, e.length };
            lastPlayedMidiNote = midi;
            noteOnCounter.bump();
        }
    }

    /** The MIDI note of a pattern note: its degree through the chord (or the root chord, for the
        key scale in single-note mode), placed in its octave; -1: none. */
    int midiNoteFor (const pattern::Note& n) const
    {
        const bool key = n.scale == pattern::ScaleKind::Key;
        const MidiTools::Chord& active = (key && chordMethod == 2) ? rootChord : chord;
        const int base = getNoteForDegree (active, n.degree);
        if (base < 0)
            return -1;
        // For "Notes played" (0) and "Single note" (2) modes, the base is a semitone (0-11)
        // that needs to be placed in an absolute octave.
        // For "Chord played as is" (1) mode, the base is a full MIDI note that needs a relative offset.
        const int placed = (chordMethod == 1) ? base + (n.octave - baseOctave) * 12
                                              : base + n.octave * 12;
        return placed + n.accidental;
    }

    /** Note-offs for the notes whose end has come. @return true if there was any. */
    bool releaseDueNotes (int time)
    {
        bool any = false;
        for (int i = 0; i < soundingCount;)
        {
            const Sounding s = sounding[(std::size_t) i];
            if (samplesUntilNextStep + s.endRel * samplesPerBeat <= timeTolerance)
            {
                release (i, time);
                any = true;
            }
            else
                ++i;
        }
        return any;
    }

    void release (int index, int time)
    {
        const Sounding s = sounding[(std::size_t) index];
        outEvents.push_back ({ time, s.channel, s.note, 0, false });
        for (int j = index + 1; j < soundingCount; ++j)
            sounding[(std::size_t) (j - 1)] = sounding[(std::size_t) j];
        --soundingCount;
        if (lastPlayedMidiNote == s.note)
            lastPlayedMidiNote = -1;
    }

    void releaseAllNotes()
    {
        while (soundingCount > 0)
            release (0, 0);
        lastPlayedMidiNote = -1;
    }

    //--------------------------------------------------------------------------
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

    void updateSamplesPerBeat()
    {
        if (sampleRate > 0 && tempoBPM > 0)
            samplesPerBeat = sampleRate * 60.0 / tempoBPM;
    }

    double sampleRate = 0.0;
    double tempoBPM = 120.0;
    int subdivision = 4; // Default to 1/16
    double samplesPerBeat = 0.0;
    double samplesUntilNextStep = 0.0;

    // The program and what plays it
    std::shared_ptr<const pattern::Program> program;
    bool programFromText = true;              ///< compiled by setPattern(), so compiled again when the syntax or the step changes
    int patternSyntax = 2;
    pattern::Expressions* expressions = nullptr;
    std::vector<pattern::Diagnostic> diagnostics;   ///< of the last text given to setPattern()

    pattern::Pass currentPass;
    pattern::Carry carry;
    bool haveCurrentPass = false;
    int nextStep = 0, nextEvent = 0;
    int passIndex = 0;                        ///< chooses the parts of [a|b]; restarts with the pattern
    std::uint64_t baseSeed = 0, seedCounter = 0;   ///< pass k draws from mix (baseSeed ^ mix (k))

    std::array<Sounding, maxSounding> sounding {};
    int soundingCount = 0;

    // What GUI threads read about the step that played last (plain ints, as before).
    int shownStep = -1, shownSourceStart = 0, shownSourceEnd = 0;

    // Output buffer, reused across calls so that nothing allocates on the
    // audio thread. Sized in prepareToPlay(); see the note above processBlock()
    // for the lifetime rule this places on callers. Shared by processBlock(),
    // reset() and turnOff(), which are never in flight at the same time.
    std::vector<ArpMidiEvent> outEvents;
};

} // namespace fxme
