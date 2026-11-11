/*
  ------------------------------------------------------------------------------
    MelodyMaker.h

    Melody makers: algorithms that write a melody as pattern-language text
    (PatternLanguage.h; the grammar is Otomatotamoto's doc/pattern-language.md,
    the design doc/melody-generator.md).

    A maker gets a Context (the chords of one cycle: their dates, their scales,
    their chord tones), a set of parameter values and a seed, and returns a
    Melody: one text per bar, in chord-scale degrees. Written in degrees, a
    melody follows whatever chord sounds when it is played; written against the
    context, its chord tones fall on the strong beats and its line is led
    across the chord changes.

    - Maker: one algorithm, with the parameters it declares (hosts build their
      knobs and their scenario keywords from them).
    - find(), all(): the makers compiled in. The first one is "phrase".
    - Melody::byBar(): `m8 [bar 1 | bar 2 | …]`, one bar per pass, for a host
      that restarts the pattern at each bar and counts its passes from the
      cycle's start (Otomatotamoto's conductor). Melody::flat(): one long pass.

    Draws come from the seed alone (pattern::draw): the same seed, values and
    context give the same text. The arithmetic is floating point, so two
    machines may differ in a rare tie: a melody has one owner, who publishes
    its text.

    Header-only, C++17, no dependency. Not real-time: generate() allocates.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "PatternLanguage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fxme::melody
{

//==============================================================================
/** One chord of the cycle a melody is written for. */
struct Chord
{
    double start = 0.0;                                   ///< beats from the cycle's start
    std::vector<float> scale { 0, 2, 4, 5, 7, 9, 11 };    ///< the chord scale from the chord's root: semitones, ascending in [0, 12)
    std::vector<int> tones { 0, 2, 4 };                   ///< the chord's notes, as 0-based degrees of that scale; the root first
    float root = 0.0f;                                    ///< the chord's root, in semitones above the key's root
};

/** What a maker knows of the music. No chord: one major triad for the whole length. */
struct Context
{
    std::vector<Chord> chords;       ///< in time order, the first at 0
    double lengthBeats = 16.0;       ///< the cycle
    double barBeats = 4.0;
};

/** A parameter a maker declares. With `choices`, its value is the index of one of them. */
struct ParameterInfo
{
    std::string id, label;
    float min = 0.0f, max = 1.0f, def = 0.5f;
    bool advanced = false;           ///< folded in a window
    std::vector<std::string> choices;
};

using Values = std::map<std::string, float>;   ///< by parameter id; a missing one takes its default

/** A melody, bar by bar. Every bar has the same number of steps. */
struct Melody
{
    std::string step = "m8";           ///< the step duration every bar is written in
    std::vector<std::string> bars;     ///< pattern-language text, without the step duration

    std::string byBar() const
    {
        if (bars.size() < 2)
            return step + " " + (bars.empty() ? std::string (".") : bars[0]);
        std::string t = step + " [";
        for (std::size_t i = 0; i < bars.size(); ++i)
            t += (i ? " | " : "") + bars[i];
        return t + "]";
    }

    std::string flat() const
    {
        std::string t = step;
        for (const auto& b : bars)
            t += " " + b;
        return t;
    }
};

//==============================================================================
/** One algorithm. */
class Maker
{
public:
    virtual ~Maker() = default;
    virtual const char* id() const noexcept = 0;
    virtual const std::vector<ParameterInfo>& parameters() const = 0;
    virtual Melody generate (const Values& values, const Context& context, std::uint64_t seed) const = 0;

    const ParameterInfo* parameter (std::string_view pid) const
    {
        for (const auto& p : parameters())
            if (p.id == pid)
                return &p;
        return nullptr;
    }

    /** The value of a declared parameter: the given one within its range, else its default. */
    float value (const Values& values, std::string_view pid) const
    {
        const auto* p = parameter (pid);
        if (p == nullptr)
            return 0.0f;
        const auto it = values.find (std::string (pid));
        return it == values.end() ? p->def : std::min (p->max, std::max (p->min, it->second));
    }
};

/** The makers compiled in; the first is the default. */
const std::vector<const Maker*>& all();
const Maker* find (std::string_view id);

//==============================================================================
// Shared by the makers: abstract notes on a grid of steps, and their writer.

/** A note of a melody before it is text. */
struct Note
{
    int step = 0, length = 1;        ///< in grid steps from the cycle's start; a note stays inside its bar
    int degree = 0;                  ///< 0-based, in the scale of the chord at its onset
    int octave = 0;                  ///< -1, 0, +1 around the player's octave
    int accidental = 0;              ///< -1, 0, +1 semitone
    float pitch = 0.0f;              ///< semitones above the key's root at the player's octave: what the line is led by
    enum class Link { None, Legato, Glide };
    Link link = Link::None;          ///< into the next note (which then starts where this one ends)
};

/** The notes as text, one string per bar: degrees with `o+` / `o-`, `#` / `b`, holds, rests, `^`, `~`. */
inline std::vector<std::string> writeBars (const std::vector<Note>& notes, int barSteps, int barCount)
{
    const int total = barSteps * barCount;
    std::vector<std::string> tokens (static_cast<std::size_t> (total), ".");
    for (const auto& n : notes)
    {
        if (n.step < 0 || n.step >= total)
            continue;
        std::string t = n.octave > 0 ? "o+" : n.octave < 0 ? "o-" : "";
        t += n.accidental > 0 ? "#" : n.accidental < 0 ? "b" : "";
        t += "123456789ABCDEF"[std::min (14, std::max (0, n.degree))];
        const int barEnd = (n.step / barSteps + 1) * barSteps;
        const int end = std::min (barEnd, n.step + std::max (1, n.length));
        tokens[static_cast<std::size_t> (n.step)] = t;
        for (int s = n.step + 1; s < end; ++s)
            tokens[static_cast<std::size_t> (s)] = "_";
        if (n.link != Note::Link::None && end < barEnd)
            tokens[static_cast<std::size_t> (end - 1)] += n.link == Note::Link::Glide ? "~" : "^";
    }

    std::vector<std::string> bars (static_cast<std::size_t> (barCount));
    for (int s = 0; s < total; ++s)
    {
        auto& bar = bars[static_cast<std::size_t> (s / barSteps)];
        bar += (bar.empty() ? "" : " ") + tokens[static_cast<std::size_t> (s)];
    }
    return bars;
}

//==============================================================================
/** The first maker, "phrase" (doc/melody-generator.md §3).

    Form first: the cycle is cut into phrases, a phrase into units (a bar or
    two) that are new, a repeat of an earlier one or a sequence of it, the last
    one a cadence. Then a rhythm per unit, from metric weights. Then pitches,
    note by note: a category (chord tone on strong beats, any scale tone
    elsewhere), and among its candidates a weighted draw: range, proximity to
    the previous note, step inertia, turning back after a leap, the contour,
    and, in a repeated unit, the note it repeats. Then approach notes, written
    backwards from the chord tones they lead to. Several candidates are made
    and the best kept by a score. */
class PhraseMaker final : public Maker
{
public:
    const char* id() const noexcept override { return "phrase"; }

    const std::vector<ParameterInfo>& parameters() const override
    {
        static const std::vector<ParameterInfo> list {
            { "density", "Density", 0.0f, 1.0f, 0.5f, false, {} },
            { "span", "Span", 0.5f, 2.5f, 1.2f, false, {} },
            { "leaps", "Leaps", 0.0f, 1.0f, 0.3f, false, {} },
            { "consonance", "Consonance", 0.0f, 1.0f, 0.7f, false, {} },
            { "repetition", "Repetition", 0.0f, 1.0f, 0.6f, false, {} },
            { "register", "Register", -12.0f, 12.0f, 0.0f, false, {} },
            { "syncopation", "Syncopation", 0.0f, 1.0f, 0.15f, true, {} },
            { "colour", "Colour", 0.0f, 1.0f, 0.2f, true, {} },
            { "chromatic", "Chromatic", 0.0f, 1.0f, 0.0f, true, {} },
            { "legato", "Legato", 0.0f, 1.0f, 0.0f, true, {} },
            { "slide", "Slide", 0.0f, 1.0f, 0.0f, true, {} },
            { "contour", "Contour", 0.0f, 4.0f, 0.0f, true, { "arch", "rise", "fall", "wave", "free" } },
            { "form", "Form", 0.0f, 4.0f, 0.0f, true, { "auto", "sentence", "period", "aaba", "free" } },
            { "bars", "Phrase", 0.0f, 4.0f, 0.0f, true, { "auto", "1", "2", "4", "8" } },
            { "grid", "Grid", 0.0f, 1.0f, 0.0f, true, { "8", "16" } },
        };
        return list;
    }

    Melody generate (const Values& values, const Context& context, std::uint64_t seed) const override
    {
        const Setup s = setup (values, context);
        std::vector<Note> best;
        double bestScore = -1e30;
        for (std::uint64_t i = 0; i < 12; ++i)
        {
            auto notes = candidate (s, pattern::mix (seed + i * 0x9e3779b97f4a7c15ULL));
            const double sc = score (s, notes);
            if (sc > bestScore)
            {
                bestScore = sc;
                best = std::move (notes);
            }
        }
        Melody m;
        m.step = s.sixteenths ? "m16" : "m8";
        m.bars = writeBars (best, s.barSteps, s.barCount);
        return m;
    }

private:
    struct Rng
    {
        std::uint64_t seed, k = 0;
        double next() noexcept { return pattern::draw (seed, k++); }
        bool chance (double p) noexcept { return next() < p; }
    };

    struct Setup
    {
        std::vector<Chord> chords;
        bool sixteenths = false;
        double stepBeats = 0.5;
        int barSteps = 8, barCount = 4, phraseBars = 4, unitBars = 1, unitsPerPhrase = 4;
        float density, span, leaps, consonance, repetition, centre, syncopation, colour, chromatic, legato, slide;
        int contour = 0, form = 0;
    };

    /** What a unit is made from. */
    struct Role
    {
        int source = -1;       ///< the unit of the same phrase it repeats; -1: new
        bool sequence = false; ///< repeated a step away
    };

    struct Slot   ///< an onset before it has a pitch
    {
        int step = 0, length = 1, unit = 0, indexInUnit = 0, phrase = 0;
        float weight = 0.0f;
        bool lastOfPhrase = false, firstOfPhrase = false;
    };

    static double sq (double x) noexcept { return x * x; }

    Setup setup (const Values& v, const Context& c) const
    {
        Setup s;
        s.chords = c.chords;
        if (s.chords.empty())
            s.chords.push_back ({});
        for (auto& ch : s.chords)
        {
            if (ch.scale.empty())
                ch.scale = { 0.0f };
            if (ch.tones.empty())
                ch.tones = { 0 };
            ch.root = ch.root - 12.0f * std::floor (ch.root / 12.0f);
        }
        s.sixteenths = value (v, "grid") >= 0.5f;
        s.stepBeats = s.sixteenths ? 0.25 : 0.5;
        const double barBeats = c.barBeats > 0.0 ? c.barBeats : 4.0;
        s.barSteps = std::max (1, static_cast<int> (std::lround (barBeats / s.stepBeats)));
        s.barCount = std::min (64, std::max (1, static_cast<int> (std::ceil (c.lengthBeats / barBeats - 1e-6))));

        static const int phraseChoices[] { 0, 1, 2, 4, 8 };
        const int asked = phraseChoices[std::min (4, std::max (0, static_cast<int> (std::lround (value (v, "bars")))))];
        s.phraseBars = std::min (s.barCount, asked > 0 ? asked : 4);
        s.unitBars = s.phraseBars >= 8 ? 2 : 1;
        s.unitsPerPhrase = (s.phraseBars + s.unitBars - 1) / s.unitBars;

        s.density = value (v, "density");
        s.span = value (v, "span");
        s.leaps = value (v, "leaps");
        s.consonance = value (v, "consonance");
        s.repetition = value (v, "repetition");
        s.centre = 7.0f + value (v, "register");
        s.syncopation = value (v, "syncopation");
        s.colour = value (v, "colour");
        s.chromatic = value (v, "chromatic");
        s.legato = value (v, "legato");
        s.slide = value (v, "slide");
        s.contour = static_cast<int> (std::lround (value (v, "contour")));
        s.form = static_cast<int> (std::lround (value (v, "form")));
        return s;
    }

    /** How strong a step of the bar is: the downbeat, the half bar, the beats, their halves, the rest. */
    static float metricWeight (const Setup& s, int stepInBar) noexcept
    {
        if (stepInBar == 0)
            return 1.0f;
        if (s.barSteps % 2 == 0 && stepInBar == s.barSteps / 2)
            return 0.8f;
        const double beat = stepInBar * s.stepBeats;
        if (std::abs (beat - std::round (beat)) < 1e-9)
            return 0.6f;
        if (std::abs (beat * 2.0 - std::round (beat * 2.0)) < 1e-9)
            return 0.35f;
        return 0.15f;
    }

    static const Chord& chordAt (const Setup& s, int step) noexcept
    {
        const double beat = step * s.stepBeats;
        const Chord* c = &s.chords.front();
        for (const auto& ch : s.chords)
            if (ch.start <= beat + 1e-9)
                c = &ch;
        return *c;
    }

    static bool isTone (const Chord& c, int degree) noexcept
    {
        return std::find (c.tones.begin(), c.tones.end(), degree) != c.tones.end();
    }

    std::vector<Role> roles (const Setup& s, Rng& rng) const
    {
        int form = s.form;
        if (form == 0)
            form = 1 + std::min (2, static_cast<int> (rng.next() * 3.0));
        const int u = s.unitsPerPhrase;
        std::vector<Role> r (static_cast<std::size_t> (u));
        if (form == 4 || u < 2)
            return r;
        auto repeat = [&] (int unit, int source, bool sequence)
        {
            if (unit < u && unit > source)
                r[static_cast<std::size_t> (unit)] = { source, sequence };
        };
        if (form == 1)        // sentence: an idea, its repeat or sequence, a continuation, a cadence
            repeat (1, 0, true);
        else if (form == 2)   // period: a b a b'
            repeat (u >= 4 ? 2 : 1, 0, false);
        else                  // aaba
        {
            repeat (1, 0, false);
            repeat (3, 0, false);
        }
        return r;
    }

    /** The onsets of one bar: a count from the density, places from the metric weights. */
    std::vector<bool> barRhythm (const Setup& s, Rng& rng) const
    {
        const int n = s.barSteps;
        std::vector<double> w (static_cast<std::size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            const double m = metricWeight (s, i);
            w[static_cast<std::size_t> (i)] = sq (std::max (0.03, m + s.syncopation * 0.7 * (1.0 - 2.0 * m)));
        }
        const double fill = (0.12 + 0.88 * s.density) * (s.sixteenths ? 0.7 : 1.0);
        int count = std::min (n, std::max (1, static_cast<int> (std::lround (n * fill + (rng.next() - 0.5)))));
        std::vector<bool> on (static_cast<std::size_t> (n), false);
        while (count-- > 0)
        {
            double sum = 0.0;
            for (double x : w)
                sum += x;
            if (sum <= 0.0)
                break;
            double at = rng.next() * sum;
            int pick = n - 1;
            for (int i = 0; i < n; ++i)
            {
                at -= w[static_cast<std::size_t> (i)];
                if (at < 0.0)
                {
                    pick = i;
                    break;
                }
            }
            on[static_cast<std::size_t> (pick)] = true;
            w[static_cast<std::size_t> (pick)] = 0.0;
        }
        return on;
    }

    float contourAt (const Setup& s, double t) const noexcept
    {
        const double a = s.span * 4.0, pi = 3.14159265358979323846;
        switch (s.contour)
        {
            case 0: return static_cast<float> (a * (std::sin (pi * t) - 0.5));
            case 1: return static_cast<float> (a * (t - 0.5));
            case 2: return static_cast<float> (a * (0.5 - t));
            case 3: return static_cast<float> (a * 0.5 * std::sin (2.0 * pi * t));
            default: return 0.0f;
        }
    }

    std::vector<Note> candidate (const Setup& s, std::uint64_t seed) const
    {
        Rng rng { seed };
        const auto unitRoles = roles (s, rng);
        const int unitSteps = s.unitBars * s.barSteps, phraseSteps = s.phraseBars * s.barSteps;
        const int total = s.barCount * s.barSteps;
        const int phrases = (s.barCount + s.phraseBars - 1) / s.phraseBars;
        const float shift = rng.chance (0.5) ? 2.0f : -2.0f;   // a sequence: about a step away

        //--- rhythm, unit by unit (kept by unit: a repeat copies it)
        std::vector<std::vector<bool>> rhythm;   // [phrase * unitsPerPhrase + unit]
        std::vector<int> pitchSource;            // the unit whose pitches it follows, -1: none
        std::vector<float> pitchShift;
        for (int p = 0; p < phrases; ++p)
            for (int u = 0; u < s.unitsPerPhrase; ++u)
            {
                const int self = p * s.unitsPerPhrase + u;
                const bool last = u == s.unitsPerPhrase - 1;
                int source = -1;
                bool sequence = false;
                const auto& role = unitRoles[static_cast<std::size_t> (u)];
                if (p > 0 && ! last && rng.chance (0.3 + 0.7 * s.repetition))
                    source = u;   // a later phrase answers the first: the same units, a new ending
                else if (role.source >= 0 && rng.chance (0.25 + 0.75 * s.repetition))
                {
                    source = p * s.unitsPerPhrase + role.source;
                    sequence = role.sequence;
                }

                std::vector<bool> on;
                if (source >= 0)
                    on = rhythm[static_cast<std::size_t> (source)];
                else if (self > 0 && rng.chance (0.5 * s.repetition))
                    on = rhythm[0];   // new pitches on the first unit's rhythm
                else
                    for (int b = 0; b < s.unitBars; ++b)
                    {
                        const auto bar = barRhythm (s, rng);
                        on.insert (on.end(), bar.begin(), bar.end());
                    }
                if (last)
                {
                    // A cadence: nothing after the middle of the phrase's last bar, so its last note is long.
                    const int from = (s.unitBars - 1) * s.barSteps;
                    bool any = false;
                    for (int i = from; i < unitSteps; ++i)
                    {
                        if (i - from > s.barSteps / 2)
                            on[static_cast<std::size_t> (i)] = false;
                        any = any || on[static_cast<std::size_t> (i)];
                    }
                    if (! any)
                        on[static_cast<std::size_t> (from)] = true;
                }
                rhythm.push_back (std::move (on));
                pitchSource.push_back (source);
                pitchShift.push_back (sequence ? shift : 0.0f);
            }

        //--- slots: onsets with their lengths
        std::vector<Slot> slots;
        for (int p = 0; p < phrases; ++p)
        {
            const std::size_t firstOfThisPhrase = slots.size();
            for (int u = 0; u < s.unitsPerPhrase; ++u)
            {
                const int unit = p * s.unitsPerPhrase + u, start = p * phraseSteps + u * unitSteps;
                int index = 0;
                for (int i = 0; i < unitSteps; ++i)
                    if (rhythm[static_cast<std::size_t> (unit)][static_cast<std::size_t> (i)] && start + i < total
                        && i + u * unitSteps < phraseSteps)
                    {
                        Slot sl;
                        sl.step = start + i;
                        sl.unit = unit;
                        sl.indexInUnit = index++;
                        sl.phrase = p;
                        sl.weight = metricWeight (s, sl.step % s.barSteps);
                        slots.push_back (sl);
                    }
            }
            if (slots.size() > firstOfThisPhrase)
            {
                slots[firstOfThisPhrase].firstOfPhrase = true;
                slots.back().lastOfPhrase = true;
            }
        }
        const int twoBeats = std::max (1, static_cast<int> (std::lround (2.0 / s.stepBeats)));
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            auto& sl = slots[i];
            const int next = i + 1 < slots.size() ? slots[i + 1].step : total;
            const int barEnd = (sl.step / s.barSteps + 1) * s.barSteps;
            const int room = std::max (1, std::min (next, barEnd) - sl.step);
            sl.length = std::min (room, sl.lastOfPhrase ? twoBeats + twoBeats / 2 : twoBeats);
            if (! sl.lastOfPhrase && room >= 2 && rng.chance (0.15))
                sl.length = std::max (1, room / 2);   // a short note, then a breath
        }

        //--- pitches
        struct Cand { int degree, octave; float pitch; bool tone; };
        std::vector<Note> notes;
        std::vector<Cand> cands;
        std::vector<double> weights;
        std::vector<std::vector<float>> unitPitches (rhythm.size());
        const double halfRange = s.span * 6.0;
        bool hasPrev = false;
        float prev = 0.0f, prevInterval = 0.0f;

        for (const auto& sl : slots)
        {
            const Chord& ch = chordAt (s, sl.step);
            const int size = std::min (15, static_cast<int> (ch.scale.size()));
            const bool strong = sl.weight >= 0.6f;
            const bool finalPhrase = sl.phrase == phrases - 1;
            const double t = static_cast<double> (sl.step - sl.phrase * phraseSteps) / std::max (1, phraseSteps);
            const double centre = s.centre + contourAt (s, t);

            bool wantTone = sl.firstOfPhrase || sl.lastOfPhrase
                            || rng.chance (strong ? 0.4 + 0.6 * s.consonance : 0.1 + 0.5 * s.consonance);
            if (wantTone && strong && ! sl.firstOfPhrase && ! sl.lastOfPhrase && rng.chance (0.4 * s.colour))
                wantTone = false;   // a tension on a strong beat

            const int src = pitchSource[static_cast<std::size_t> (sl.unit)];
            const bool follows = src >= 0 && sl.indexInUnit < static_cast<int> (unitPitches[static_cast<std::size_t> (src)].size());
            const float target = follows ? unitPitches[static_cast<std::size_t> (src)][static_cast<std::size_t> (sl.indexInUnit)]
                                               + pitchShift[static_cast<std::size_t> (sl.unit)]
                                         : 0.0f;

            cands.clear();
            weights.clear();
            double sum = 0.0;
            for (int k = -1; k <= 1; ++k)
                for (int d = 0; d < size; ++d)
                {
                    Cand c { d, k, ch.root + ch.scale[static_cast<std::size_t> (d)] + 12.0f * static_cast<float> (k), isTone (ch, d) };
                    double w = 1.0 / (1.0 + sq ((c.pitch - centre) / (halfRange * 0.6)));
                    if (std::abs (c.pitch - s.centre) > halfRange + 0.01)
                        w = 0.0;
                    if (wantTone && ! c.tone)
                        w = 0.0;
                    if (! wantTone && c.tone)
                        w *= 0.25;
                    if (hasPrev && ! sl.firstOfPhrase)
                    {
                        const double i = c.pitch - prev;
                        w *= sq (1.0 / (1.0 + sq (i / (1.2 + 5.0 * s.leaps))));
                        if (std::abs (i) < 0.01)
                            w *= 0.3;
                        const bool same = i * prevInterval > 0.0;
                        if (std::abs (prevInterval) > 0.01 && std::abs (prevInterval) <= 2.5 && same && std::abs (i) <= 2.5)
                            w *= 1.6;   // a step goes on by step
                        if (std::abs (prevInterval) > 4.5)
                            w *= same ? 0.25 : std::abs (i) <= 2.5 && std::abs (i) > 0.01 ? 3.0 : 1.0;   // a leap turns back by step
                    }
                    if (follows)
                        w *= 0.05 + 1.0 / (1.0 + sq ((c.pitch - target) / 1.2));
                    if (sl.lastOfPhrase && d == ch.tones.front())
                        w *= finalPhrase ? 4.0 : 0.3;   // closed at the cycle's end, open before
                    cands.push_back (c);
                    weights.push_back (w);
                    sum += w;
                }

            std::size_t pick = 0;
            if (sum <= 0.0)
            {
                // Nothing fits (a narrow range): the chord tone nearest the centre.
                double bestDistance = 1e30;
                for (std::size_t i = 0; i < cands.size(); ++i)
                    if (cands[i].tone && std::abs (cands[i].pitch - s.centre) < bestDistance)
                    {
                        bestDistance = std::abs (cands[i].pitch - s.centre);
                        pick = i;
                    }
            }
            else
            {
                double at = rng.next() * sum;
                pick = cands.size() - 1;
                for (std::size_t i = 0; i < cands.size(); ++i)
                {
                    at -= weights[i];
                    if (at < 0.0)
                    {
                        pick = i;
                        break;
                    }
                }
            }

            const Cand& c = cands[pick];
            Note n;
            n.step = sl.step;
            n.length = sl.length;
            n.degree = c.degree;
            n.octave = c.octave;
            n.pitch = c.pitch;
            if (hasPrev)
                prevInterval = c.pitch - prev;
            prev = c.pitch;
            hasPrev = true;
            unitPitches[static_cast<std::size_t> (sl.unit)].push_back (c.pitch);
            notes.push_back (n);
        }

        //--- approach notes, backwards: a weak note before a strong chord tone becomes its neighbour
        for (std::size_t i = notes.size(); i-- > 1;)
        {
            if (i + 1 >= notes.size())
                continue;
            const Slot& a = slots[i];
            const Slot& b = slots[i + 1];
            const Note& to = notes[i + 1];
            if (a.weight >= 0.6f || b.weight < 0.6f || a.lastOfPhrase || a.firstOfPhrase
                || ! isTone (chordAt (s, b.step), to.degree) || to.accidental != 0)
                continue;
            if (! rng.chance ((1.0 - s.consonance) * 0.5 + s.chromatic * 0.5))
                continue;
            const Chord& ch = chordAt (s, a.step);
            const int size = std::min (15, static_cast<int> (ch.scale.size()));
            const bool chromatic = rng.chance (s.chromatic);
            const float from = notes[i - 1].pitch;
            Note n = notes[i];
            double bestCost = 1e30;
            for (int k = -1; k <= 1; ++k)
                for (int d = 0; d < size; ++d)
                    for (int acc = chromatic ? -1 : 0; acc <= (chromatic ? 1 : 0); ++acc)
                    {
                        const float p = ch.root + ch.scale[static_cast<std::size_t> (d)] + 12.0f * static_cast<float> (k) + static_cast<float> (acc);
                        const double gap = std::abs (p - to.pitch);
                        if (gap < 0.01 || gap > (chromatic ? 1.01 : 2.5) || (chromatic && std::abs (gap - 1.0) > 0.01))
                            continue;
                        // From the side the line comes from, and without an accidental when one is not needed.
                        const double cost = ((p - to.pitch) * (from - to.pitch) >= 0.0 ? 0.0 : 1.0) + std::abs (acc) * 0.5 + gap * 0.1;
                        if (cost < bestCost)
                        {
                            bestCost = cost;
                            n.degree = d;
                            n.octave = k;
                            n.accidental = acc;
                            n.pitch = p;
                        }
                    }
            if (bestCost < 1e29)
                notes[i] = n;
        }

        //--- links: a note that ends where the next one starts may slur or slide into it
        if (s.legato > 0.0f || s.slide > 0.0f)
            for (std::size_t i = 0; i + 1 < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.step + n.length != notes[i + 1].step || notes[i + 1].step % s.barSteps == 0
                    || std::abs (notes[i + 1].pitch - n.pitch) < 0.01)
                    continue;
                if (rng.chance (s.slide))
                    n.link = Note::Link::Glide;
                else if (rng.chance (s.legato))
                    n.link = Note::Link::Legato;
            }
        return notes;
    }

    /** Higher is better: the line moves as `leaps` asks, repeats little, resolves its leaps, uses its range, ends on the root. */
    double score (const Setup& s, const std::vector<Note>& notes) const
    {
        if (notes.size() < 2)
            return 0.0;
        double sumAbs = 0.0, lo = notes[0].pitch, hi = notes[0].pitch;
        int repeats = 0, unresolved = 0;
        std::vector<float> distinct;
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            lo = std::min<double> (lo, notes[i].pitch);
            hi = std::max<double> (hi, notes[i].pitch);
            if (std::find (distinct.begin(), distinct.end(), notes[i].pitch) == distinct.end())
                distinct.push_back (notes[i].pitch);
            if (i == 0)
                continue;
            const double d = notes[i].pitch - notes[i - 1].pitch;
            sumAbs += std::abs (d);
            repeats += std::abs (d) < 0.01 ? 1 : 0;
            if (i >= 2)
            {
                const double before = notes[i - 1].pitch - notes[i - 2].pitch;
                unresolved += std::abs (before) > 4.5 && before * d > 0.0 ? 1 : 0;
            }
        }
        const double n = static_cast<double> (notes.size() - 1);
        double sc = 0.0;
        sc -= 0.5 * std::abs (sumAbs / n - (1.5 + 4.0 * s.leaps));
        sc -= 4.0 * std::max (0.0, repeats / n - 0.2);
        sc -= 0.5 * unresolved;
        sc -= std::abs ((hi - lo) - s.span * 12.0 * 0.7) / 12.0;
        if (notes.size() >= 6 && distinct.size() < 3)
            sc -= 2.0;
        const auto& last = notes.back();
        if (last.degree == chordAt (s, last.step).tones.front() && last.accidental == 0)
            sc += 1.0;
        return sc;
    }
};

//==============================================================================
inline const std::vector<const Maker*>& all()
{
    static const PhraseMaker phrase;
    static const std::vector<const Maker*> list { &phrase };
    return list;
}

inline const Maker* find (std::string_view id)
{
    for (const auto* m : all())
        if (id == m->id())
            return m;
    return nullptr;
}

} // namespace fxme::melody
