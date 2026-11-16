#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <FxmeTools/midi/MelodyMaker.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace fxme;

namespace
{
    /** Four bars of 4/4 in C major: C, F, G, C, one per bar. */
    melody::Context progression()
    {
        melody::Context c;
        c.lengthBeats = 16.0;
        const std::vector<float> ionian { 0, 2, 4, 5, 7, 9, 11 }, lydian { 0, 2, 4, 6, 7, 9, 11 }, mixolydian { 0, 2, 4, 5, 7, 9, 10 };
        c.chords = { { 0.0, ionian, { 0, 2, 4 }, 0.0f }, { 4.0, lydian, { 0, 2, 4 }, 5.0f },
                     { 8.0, mixolydian, { 0, 2, 4, 6 }, 7.0f }, { 12.0, ionian, { 0, 2, 4 }, 0.0f } };
        return c;
    }

    const melody::Chord& chordAt (const melody::Context& c, double beat)
    {
        const melody::Chord* found = &c.chords.front();
        for (const auto& ch : c.chords)
            if (ch.start <= beat + 1e-9)
                found = &ch;
        return *found;
    }

    struct Played { double onset; float pitch; int degree; bool tone; float velocity; bool key; };

    /** The melody compiled and realized bar by bar, as the conductor plays it; its pitches through the context. */
    std::vector<Played> play (const melody::Melody& m, const melody::Context& c)
    {
        const auto program = pattern::compile (m.byBar());
        for (const auto& d : program.diagnostics())
            FAIL (d.message << " at " << d.position << " in: " << m.byBar());
        REQUIRE (program.ok());
        std::vector<Played> out;
        pattern::Pass pass;
        pass.reserve (program.info());
        for (int bar = 0; bar < static_cast<int> (m.bars.size()); ++bar)
        {
            pattern::Context pc;
            pc.passIndex = bar;
            program.realize (pc, pass);
            CHECK (pass.lengthBeats == doctest::Approx (c.barBeats));
            for (const auto& e : pass.events)
            {
                REQUIRE (e.noteCount == 1);
                const auto& n = pass.notes[static_cast<std::size_t> (e.firstNote)];
                const double onset = bar * c.barBeats + e.onset;
                const auto& ch = chordAt (c, onset);
                const float octave = 12.0f * static_cast<float> (n.octave - 4) + static_cast<float> (n.accidental);
                if (n.scale == pattern::ScaleKind::Key)
                {
                    REQUIRE (n.degree < static_cast<int> (c.key.size()));
                    out.push_back ({ onset, c.key[static_cast<std::size_t> (n.degree)] + octave, n.degree, false, e.velocity, true });
                    continue;
                }
                REQUIRE (n.degree < static_cast<int> (ch.scale.size()));
                bool tone = false;
                for (int t : ch.tones)
                    tone = tone || t == n.degree;
                out.push_back ({ onset, ch.root + ch.scale[static_cast<std::size_t> (n.degree)] + octave,
                                 n.degree, tone && n.accidental == 0, e.velocity, false });
            }
        }
        return out;
    }

    const melody::Maker& phrase() { return *melody::find ("phrase"); }
}

TEST_CASE ("melody: the makers are found by id")
{
    REQUIRE (melody::find ("phrase") != nullptr);
    CHECK (melody::find ("nothing") == nullptr);
    CHECK (melody::all().front() == melody::find ("phrase"));
    CHECK (phrase().parameter ("density") != nullptr);
    CHECK (phrase().value ({ { "density", 7.0f } }, "density") == 1.0f);   // within its range
    CHECK (phrase().value ({}, "span") == doctest::Approx (1.2f));
}

TEST_CASE ("melody: the text compiles, bar by bar, for many seeds and settings")
{
    const auto c = progression();
    for (std::uint64_t seed = 1; seed <= 40; ++seed)
    {
        melody::Values v { { "density", static_cast<float> (seed % 5) / 4.0f }, { "leaps", static_cast<float> (seed % 3) / 2.0f },
                           { "grid", static_cast<float> (seed % 2) }, { "chromatic", seed % 4 == 0 ? 1.0f : 0.0f },
                           { "consonance", seed % 4 == 0 ? 0.0f : 0.7f },
                           { "legato", 0.5f }, { "slide", 0.3f }, { "form", static_cast<float> (seed % 5) } };
        const auto m = phrase().generate (v, c, seed);
        REQUIRE (m.bars.size() == 4);
        const auto notes = play (m, c);
        CHECK (notes.size() >= 4);
        CHECK (pattern::compile (m.flat()).ok());
        CHECK (pattern::compile (m.flat()).info().lengthBeats == doctest::Approx (16.0));
    }
}

TEST_CASE ("melody: the same seed gives the same text, another seed another")
{
    const auto c = progression();
    const auto a = phrase().generate ({}, c, 42), b = phrase().generate ({}, c, 42), other = phrase().generate ({}, c, 43);
    CHECK (a.byBar() == b.byBar());
    CHECK (a.byBar() != other.byBar());
}

TEST_CASE ("melody: the range is respected")
{
    const auto c = progression();
    for (std::uint64_t seed = 1; seed <= 20; ++seed)
        for (float span : { 0.5f, 1.2f, 2.5f })
            for (const auto& n : play (phrase().generate ({ { "span", span }, { "leaps", 1.0f } }, c, seed), c))
                CHECK (std::abs (n.pitch - 7.0f) <= span * 6.0f + 2.01f);   // the centre, half the span, an approach note's step
}

TEST_CASE ("melody: chord tones on the strong beats at full consonance")
{
    const auto c = progression();
    int strong = 0, tones = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed)
        for (const auto& n : play (phrase().generate ({ { "consonance", 1.0f }, { "colour", 0.0f } }, c, seed), c))
            if (std::abs (n.onset - std::round (n.onset)) < 1e-9)
            {
                ++strong;
                tones += n.tone ? 1 : 0;
            }
    REQUIRE (strong > 50);
    CHECK (tones == strong);
}

TEST_CASE ("melody: leaps and density do what they say")
{
    const auto c = progression();
    auto meanInterval = [&] (float leaps)
    {
        double sum = 0.0;
        int count = 0;
        for (std::uint64_t seed = 1; seed <= 20; ++seed)
        {
            const auto notes = play (phrase().generate ({ { "leaps", leaps }, { "consonance", 0.3f }, { "span", 2.0f } }, c, seed), c);
            for (std::size_t i = 1; i < notes.size(); ++i, ++count)
                sum += std::abs (notes[i].pitch - notes[i - 1].pitch);
        }
        return sum / count;
    };
    CHECK (meanInterval (0.0f) < 3.0);
    CHECK (meanInterval (1.0f) > meanInterval (0.0f) + 1.0);

    auto count = [&] (float density)
    {
        std::size_t n = 0;
        for (std::uint64_t seed = 1; seed <= 10; ++seed)
            n += play (phrase().generate ({ { "density", density } }, c, seed), c).size();
        return n;
    };
    CHECK (count (0.0f) < count (0.5f));
    CHECK (count (0.5f) < count (1.0f));
    CHECK (count (0.0f) <= 10 * 8);    // about a note per bar
}

TEST_CASE ("melody: a repeated unit keeps its onsets; the cycle ends on a long chord root")
{
    const auto c = progression();
    int sameRhythm = 0, roots = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed)
    {
        const auto m = phrase().generate ({ { "repetition", 1.0f }, { "form", 3.0f } }, c, seed);   // aaba
        auto rhythm = [] (std::string bar)
        {
            for (std::size_t at; (at = bar.find_first_of ("vV")) != std::string::npos;)
                bar.erase (at, 2);   // velocities: not the rhythm
            std::string r;
            for (char ch : bar)
                r += ch == ' ' ? ' ' : ch == '.' || ch == '_' ? '.' : ch == 'o' || ch == '+' || ch == '-' || ch == '#' || ch == 'b' ? '\0' : 'n';
            r.erase (std::remove (r.begin(), r.end(), '\0'), r.end());
            return r;
        };
        sameRhythm += rhythm (m.bars[0]) == rhythm (m.bars[1]) ? 1 : 0;
        const auto notes = play (m, c);
        roots += notes.back().degree == 0 ? 1 : 0;
        CHECK (notes.back().onset <= 14.0 + 1e-9);   // nothing after the middle of the last bar
    }
    CHECK (sameRhythm == 20);
    CHECK (roots >= 12);
}

TEST_CASE ("melody: odd contexts: no chord, one bar, 7/8, a 5-note scale, a long cycle")
{
    melody::Context empty;
    CHECK (pattern::compile (phrase().generate ({}, empty, 1).byBar()).ok());

    melody::Context one;
    one.lengthBeats = 4.0;
    const auto single = phrase().generate ({}, one, 1);
    CHECK (single.bars.size() == 1);
    CHECK (single.byBar().find ('[') == std::string::npos);

    melody::Context seven;
    seven.barBeats = 3.5;
    seven.lengthBeats = 7.0;
    seven.chords = { { 0.0, { 0, 2, 4, 7, 9 }, { 0, 2, 3 }, 2.0f } };
    const auto m = phrase().generate ({ { "density", 0.8f } }, seven, 3);
    CHECK (m.bars.size() == 2);
    for (const auto& n : play (m, seven))
        CHECK (n.degree < 5);

    melody::Context longCycle;
    longCycle.lengthBeats = 64.0;
    const auto sixteen = phrase().generate ({ { "bars", 4.0f } }, longCycle, 5);   // phrases of 8 bars
    CHECK (sixteen.bars.size() == 16);
    CHECK (pattern::compile (sixteen.byBar()).ok());
}

TEST_CASE ("melody: velocities follow the metre and the phrase; dynamics 0 writes none")
{
    const auto c = progression();
    double strong = 0.0, weak = 0.0;
    int strongCount = 0, weakCount = 0;
    std::set<float> levels;
    for (std::uint64_t seed = 1; seed <= 20; ++seed)
    {
        const auto m = phrase().generate ({ { "dynamics", 1.0f }, { "density", 0.8f } }, c, seed);
        CHECK (m.bars[0].rfind ("V", 0) == 0);
        for (const auto& n : play (m, c))
        {
            REQUIRE (n.velocity > 0.0f);
            levels.insert (n.velocity);
            const bool onBeat = std::abs (n.onset - std::round (n.onset)) < 1e-9;
            (onBeat ? strong : weak) += n.velocity;
            (onBeat ? strongCount : weakCount) += 1;
        }
    }
    CHECK (levels.size() >= 4);
    CHECK (strong / strongCount > weak / weakCount + 0.05);

    for (std::uint64_t seed = 1; seed <= 10; ++seed)
    {
        const auto m = phrase().generate ({ { "dynamics", 0.0f } }, c, seed);
        CHECK (m.byBar().find ('V') == std::string::npos);
        CHECK (m.byBar().find ('v') == std::string::npos);
        for (const auto& n : play (m, c))
            CHECK (n.velocity < 0.0f);
    }
}

TEST_CASE ("melody: passing tones in the key scale, between quotes, never on the chord's notes")
{
    // A borrowed bVII (Bb in C): its chord scale has Bb, the key scale B.
    auto c = progression();
    c.chords[2] = { 8.0, { 0, 2, 4, 5, 7, 9, 10 }, { 0, 2, 4 }, 10.0f };   // Bb mixolydian from Bb
    int keyNotes = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed)
    {
        const auto m = phrase().generate ({ { "passing", 1.0f }, { "consonance", 0.3f } }, c, seed);
        CHECK (m.byBar().find ('"') != std::string::npos);
        for (const auto& n : play (m, c))
            if (n.key)
            {
                ++keyNotes;
                const int pc = static_cast<int> (std::lround (n.pitch)) % 12;
                CHECK (pc >= 0);
                CHECK (std::set<int> { 0, 2, 4, 5, 7, 9, 11 }.count ((pc + 12) % 12) == 1);   // C major's notes
            }
    }
    CHECK (keyNotes > 20);

    // Without `passing`, no quote.
    for (std::uint64_t seed = 1; seed <= 10; ++seed)
        CHECK (phrase().generate ({ { "consonance", 0.3f } }, c, seed).byBar().find ('"') == std::string::npos);
}
