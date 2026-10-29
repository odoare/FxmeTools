/*
  ------------------------------------------------------------------------------
    PatternLanguage.h

    The pattern language: TeAr's text patterns (syntax 2) and their superset,
    syntax 3 (chords, local note values, generators, repeats, variations by
    pass, gates, glides, expressions, comments). One compiler for every host:
    TeAr (fxme::Arpeggiator plays its passes) and Otomatotamoto (the
    conductor's bank, its step sequencer, its arpeggiator, its players).

    The grammar is written in Otomatotamoto's doc/pattern-language.md, which
    is the reference: this header only says how to call it.

    - compile(): text -> Program, with errors and their positions. Allocates.
    - Program::realize(): one pass -> timed events (onsets and lengths in
      beats, velocities 0..1, notes as scale degrees). Draws come from a seed
      the host gives (the k-th draw of a pass is a hash of the seed and k), so
      a pass replays identically. Writes into a Pass whose storage the host
      reserved (reserve()), so that it can run on an audio thread.

    No MIDI, no harmony, no clock here: the host maps degrees to notes through
    its own scales and plays the events on its own time.

    Header-only, C++17, no dependency.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fxme::pattern
{

//==============================================================================
// Values between single quotes ('64*energy') are the host's business: a
// pattern hands their text to the host when it compiles, and asks the host
// for their value when it realizes a pass. Without a host evaluator, only a
// plain number is accepted between quotes.

class Expressions
{
public:
    virtual ~Expressions() = default;

    /** Compiles an expression's text; returns an id >= 0, or -1 with `error` set. Not real-time. */
    virtual int compile (std::string_view text, std::string& error) = 0;

    /** The value of expression `id`, with the draws of `seed` (rand, chance). Real-time safe. */
    virtual double evaluate (int id, std::uint64_t seed) const noexcept = 0;
};

//==============================================================================
struct Options
{
    int syntax = 3;                    ///< 2: TeAr's syntax 2 (unknown characters skipped, no reset per pass, free probability lengths); 3: this one
    double defaultStepBeats = 0.25;    ///< the step's duration when the text has no `m` (the host's subdivision)
    Expressions* expressions = nullptr;   ///< for '…' values; kept by the program (must outlive it)
};

struct Diagnostic
{
    int position = 0;                  ///< character index in the text
    std::string message;               ///< doc/pattern-language.md §11
    bool warning = false;              ///< a warning does not stop the pattern from compiling
};

/** What a pattern holds (doc/pattern-language.md §12). */
struct Info
{
    enum class Kind { Rhythm, Melody, Chords };
    Kind kind = Kind::Rhythm;
    bool glides = false;               ///< it has `~`
    bool fixedLength = true;           ///< every pass lasts the same (false: syntax-2 probability groups of different lengths)
    double lengthBeats = 0.0;          ///< a pass's length when fixed
    double shortestStepBeats = 0.0;
    int maxEventsPerPass = 0;          ///< for Pass::reserve()
    int maxNotesPerPass = 0;
    int maxStepsPerPass = 0;
};

//==============================================================================
enum class ScaleKind { Chord, Key };   ///< digits; digits between double quotes

/** One note of an event, as written: the host maps it through its scale. */
struct Note
{
    ScaleKind scale = ScaleKind::Chord;
    int degree = 0;                    ///< 0-based ('1' is 0), may exceed the scale's size (the host wraps or adds octaves)
    int octave = 4;                    ///< absolute octave in effect (0..7), from Context::baseOctave and the o/O prefixes
    int accidental = 0;                ///< semitones, -1 (b) or +1 (#)
};

/** A sounding event: a note, a chord, or a hit (`x`, no note). */
struct Event
{
    double onset = 0.0;                ///< beats from the pass's start
    double length = 0.0;               ///< beats it sounds (its steps, holds included, times its gate)
    float velocity = -1.0f;            ///< 0..1; < 0: the host's (the text set none)
    bool glide = false;                ///< `~`
    int firstNote = 0, noteCount = 0;  ///< in Pass::notes; 0 notes: a hit
    int firstStep = 0;                 ///< the step (in Pass::steps) it starts on
};

/** A step of the pass as the text wrote it, sounding or not: for views (the playing step highlighted). */
struct Step
{
    double onset = 0.0, duration = 0.0;   ///< beats
    int sourceStart = 0, sourceEnd = 0;   ///< characters of the text: [start, end), prefixes included
    int event = -1;                       ///< the event it starts or holds; -1: a rest
    bool tied = false;                    ///< a hold with nothing before it in this pass to hold: it extends the sound the previous pass left (a rest if none)
};

/** One pass, realized. reserve() once with the program's Info, then realize() into it as often as needed. */
struct Pass
{
    std::vector<Event> events;
    std::vector<Note> notes;
    std::vector<Step> steps;
    double lengthBeats = 0.0;

    void reserve (const Info& i)
    {
        events.reserve (static_cast<std::size_t> (i.maxEventsPerPass));
        notes.reserve (static_cast<std::size_t> (i.maxNotesPerPass));
        steps.reserve (static_cast<std::size_t> (i.maxStepsPerPass));
    }
    void clear() noexcept { events.clear(); notes.clear(); steps.clear(); lengthBeats = 0.0; }
};

/** What the host says when it asks for a pass. */
struct Context
{
    std::uint64_t seed = 0;            ///< the pass's draws (doc/pattern-language.md §9.3)
    int passIndex = 0;                 ///< 0, 1, 2…: chooses the parts of [a|b|c]
    int chordSize = 7;                 ///< degrees of the chord scale: `+` `-` cycle within them, `?` draws among them
    int keySize = 7;                   ///< the same for the key scale (between double quotes)
    std::uint32_t chordPresent = ~0u;  ///< bit d: degree d of the chord scale is present (`?` draws only those); all by default
    int baseOctave = 4;                ///< the octave a pass starts at (TeAr: from the pressed note)
    float hostVelocity = 96.0f / 127.0f;   ///< what `v+` `v-` `V+` `V-` step from when the text set no velocity (TeAr: the played note's)
};

/** What carries over from one pass to the next in syntax 2 (syntax 3 resets it at each pass). */
struct Carry
{
    int octave = -1;                   ///< -1: baseOctave
    float velocity = -1.0f;            ///< the global V in effect; < 0: none
    int lastDegree = 0;
    bool inKeyScale = false;

    // Syntax 2 only. Local prefixes written after the last step ("1 2 o+") wait
    // for the next step, which is the next pass's first (TeAr wraps around).
    // The program stores them here at the end of a pass and reads them back at
    // the start of the next. Hosts leave these alone.
    int pendingOctave = -1;            ///< a local `o` waiting; -1: none
    float pendingVelocity = -1.0f;     ///< a local `v` waiting; < 0: none
    int pendingAccidental = 0;         ///< a `#` or `b` waiting
    bool pendingForcedRest = false;    ///< a failed `p` roll waiting
};

//==============================================================================
class Program
{
public:
    Program();
    ~Program();
    Program (Program&&) noexcept;
    Program& operator= (Program&&) noexcept;

    /** True if it compiled (warnings allowed). An empty text compiles to an empty program. */
    bool ok() const noexcept;
    const std::vector<Diagnostic>& diagnostics() const noexcept;
    const Info& info() const noexcept;
    const std::string& text() const noexcept;

    /** One pass into `out` (cleared first). `carry`: syntax 2 reads and updates it; syntax 3 ignores it.
        Real-time safe when `out` was reserved with info(). A program that is not ok() gives an empty pass. */
    void realize (const Context& context, Pass& out, Carry* carry = nullptr) const noexcept;

private:
    friend Program compile (std::string_view text, const Options& options);
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Compiles a pattern's text (doc/pattern-language.md). Not real-time. */
Program compile (std::string_view text, const Options& options = {});

//==============================================================================
// Helpers the hosts share.

/** The k-th draw of a seed, in [0, 1): what every draw of a pass uses. */
double draw (std::uint64_t seed, std::uint64_t k) noexcept;

/** A seed from several keys (splitmix64), for hosts building one. */
std::uint64_t mix (std::uint64_t x) noexcept;

/** TeAr's velocity levels (syntax 2): level 1..15 -> MIDI velocity 8..127. */
int velocityForLevel (int level) noexcept;

} // namespace fxme::pattern

#include "PatternLanguageImpl.h"
