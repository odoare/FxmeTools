#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <FxmeTools/midi/Arpeggiator.h>
#include <FxmeTools/midi/PatternLanguage.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace fxme::pattern;

namespace
{
    // A host for '...' values: the text decides the value, so tests can see
    // which draws an expression was given.
    struct FakeExpressions : Expressions
    {
        std::vector<std::string> texts;
        mutable std::vector<std::uint64_t> seeds;

        int compile (std::string_view text, std::string& error) override
        {
            if (text == "bad") { error = "unknown function 'foo'"; return -1; }
            texts.emplace_back (text);
            return static_cast<int> (texts.size()) - 1;
        }
        double evaluate (int id, std::uint64_t seed) const noexcept override
        {
            seeds.push_back (seed);
            const std::string& t = texts[static_cast<std::size_t> (id)];
            if (t == "64*energy") return 0.25;
            if (t == "half") return 0.5;
            if (t == "rand") return draw (seed, 0);
            if (t == "three") return 3.0;
            return std::stod (t);
        }
    };

    Program build (const std::string& text, int syntax = 3, Expressions* ex = nullptr, double step = 0.25)
    {
        Options o;
        o.syntax = syntax;
        o.defaultStepBeats = step;
        o.expressions = ex;
        return compile (text, o);
    }

    Pass run (const Program& p, const Context& ctx = {}, Carry* carry = nullptr)
    {
        Pass pass;
        pass.reserve (p.info());
        p.realize (ctx, pass, carry);
        return pass;
    }

    Pass run (const std::string& text, int syntax = 3, const Context& ctx = {})
    {
        Program p = build (text, syntax);
        REQUIRE_MESSAGE (p.ok(), text);
        return run (p, ctx);
    }

    Context withSeed (std::uint64_t s, int passIndex = 0)
    {
        Context c;
        c.seed = s;
        c.passIndex = passIndex;
        return c;
    }

    const Note& noteOf (const Pass& p, int event, int n = 0)
    {
        return p.notes[static_cast<std::size_t> (p.events[static_cast<std::size_t> (event)].firstNote + n)];
    }
    float vel (int level) { return static_cast<float> (velocityForLevel (level)) / 127.0f; }
    const Event& ev (const Pass& p, int i) { return p.events[static_cast<std::size_t> (i)]; }
    int deg (const Pass& p, int event, int n = 0) { return noteOf (p, event, n).degree; }

    std::vector<int> degrees (const Pass& p)
    {
        std::vector<int> d;
        for (std::size_t i = 0; i < p.events.size(); ++i)
            d.push_back (p.events[i].noteCount > 0 ? deg (p, static_cast<int> (i)) : -1);
        return d;
    }

    // A seed for which draw k of the pass satisfies a condition.
    template <typename F>
    std::uint64_t seedWhere (std::uint64_t k, F cond)
    {
        for (std::uint64_t s = 1; s < 100000; ++s)
            if (cond (draw (s, k))) return s;
        FAIL ("no seed");
        return 0;
    }

    bool hasError (const Program& p, int position, const std::string& message)
    {
        for (const Diagnostic& d : p.diagnostics())
            if (! d.warning && d.position == position && d.message == message) return true;
        return false;
    }

    std::string firstError (const Program& p)
    {
        for (const Diagnostic& d : p.diagnostics())
            if (! d.warning) return std::to_string (d.position) + ": " + d.message;
        return "";
    }
}

//==============================================================================
TEST_CASE("draw and mix are reproducible and spread")
{
    CHECK (draw (1, 0) == draw (1, 0));
    CHECK (draw (1, 0) != draw (1, 1));
    CHECK (draw (1, 0) != draw (2, 0));
    double sum = 0.0;
    for (std::uint64_t k = 0; k < 10000; ++k)
    {
        const double d = draw (7, k);
        CHECK (d >= 0.0);
        CHECK (d < 1.0);
        sum += d;
    }
    CHECK (sum / 10000.0 == doctest::Approx (0.5).epsilon (0.03));
    CHECK (mix (0) != mix (1));
    CHECK (velocityForLevel (1) == 8);
    CHECK (velocityForLevel (8) == 68);
    CHECK (velocityForLevel (15) == 127);
    CHECK (velocityForLevel (0) == 1);
}

//==============================================================================
// doc/pattern-language.md section 2

TEST_CASE("section 2: 1 3 5 8")
{
    const Pass p = run ("1 3 5 8");
    REQUIRE (p.events.size() == 4);
    const int expected[] = { 0, 2, 4, 7 };
    for (int i = 0; i < 4; ++i)
    {
        CHECK (ev (p, i).onset == doctest::Approx (0.25 * i));
        CHECK (ev (p, i).length == doctest::Approx (0.25));
        CHECK (deg (p, i) == expected[i]);
        CHECK (noteOf (p, i).octave == 4);
        CHECK (noteOf (p, i).scale == ScaleKind::Chord);
        CHECK (ev (p, i).velocity < 0.0f);
    }
    CHECK (p.lengthBeats == doctest::Approx (1.0));
    CHECK (p.steps.size() == 4);
}

TEST_CASE("section 2: 1 _ 5 .")
{
    const Pass p = run ("1 _ 5 .");
    REQUIRE (p.events.size() == 2);
    CHECK (ev (p, 0).length == doctest::Approx (0.5));
    CHECK (ev (p, 1).onset == doctest::Approx (0.5));
    CHECK (ev (p, 1).length == doctest::Approx (0.25));
    CHECK (deg (p, 1) == 4);
    REQUIRE (p.steps.size() == 4);
    CHECK (p.steps[0].event == 0);
    CHECK (p.steps[1].event == 0);
    CHECK (p.steps[2].event == 1);
    CHECK (p.steps[3].event == -1);
}

TEST_CASE("section 2: x . . x . . x .")
{
    Program prog = build ("x . . x . . x .");
    const Pass p = run (prog);
    REQUIRE (p.events.size() == 3);
    CHECK (ev (p, 0).onset == doctest::Approx (0.0));
    CHECK (ev (p, 1).onset == doctest::Approx (0.75));
    CHECK (ev (p, 2).onset == doctest::Approx (1.5));
    for (int i = 0; i < 3; ++i) CHECK (ev (p, i).noteCount == 0);
    CHECK (prog.info().kind == Info::Kind::Rhythm);
}

TEST_CASE("section 2: p5 1 2 p3 3:5")
{
    Program prog = build ("p5 1 2 p3 3:5");
    std::set<int> shapes;
    for (std::uint64_t s = 0; s < 200; ++s)
    {
        const Pass p = run (prog, withSeed (s));
        const bool one = draw (s, 0) < 0.5;
        const bool three = draw (s, 1) < 0.3;
        std::vector<int> expected;
        if (one) expected.push_back (0);
        expected.push_back (1);
        expected.push_back (three ? 2 : 4);
        CHECK (degrees (p) == expected);
        CHECK (p.steps.size() == 3);
        shapes.insert ((one ? 1 : 0) + (three ? 2 : 0));
    }
    CHECK (shapes.size() == 4);
}

TEST_CASE("section 2: O+ 1 3 5 O- 1")
{
    const Pass p = run ("O+ 1 3 5 O- 1");
    REQUIRE (p.events.size() == 4);
    CHECK (noteOf (p, 0).octave == 5);
    CHECK (noteOf (p, 1).octave == 5);
    CHECK (noteOf (p, 2).octave == 5);
    CHECK (noteOf (p, 3).octave == 4);
}

TEST_CASE("section 2: v'64*energy' x . x .")
{
    FakeExpressions fx;
    Program prog = build ("v'64*energy' x . x .", 3, &fx);
    REQUIRE (prog.ok());
    const Pass p = run (prog);
    REQUIRE (p.events.size() == 2);
    CHECK (ev (p, 0).velocity == doctest::Approx (0.25f));
    CHECK (ev (p, 1).velocity < 0.0f);          // v is local: the second hit takes the host's
    CHECK (ev (p, 1).onset == doctest::Approx (0.5));
}

TEST_CASE("section 2: (m32 1 2 3 4) 5 6")
{
    const Pass p = run ("(m32 1 2 3 4) 5 6");
    REQUIRE (p.events.size() == 6);
    const double onsets[] = { 0.0, 0.125, 0.25, 0.375, 0.5, 0.75 };
    for (int i = 0; i < 6; ++i)
        CHECK (ev (p, i).onset == doctest::Approx (onsets[i]));
    CHECK (ev (p, 0).length == doctest::Approx (0.125));
    CHECK (ev (p, 5).length == doctest::Approx (0.25));
    CHECK (p.lengthBeats == doctest::Approx (1.0));
}

TEST_CASE("section 2: {135} _ {146} _")
{
    Program prog = build ("{135} _ {146} _");
    const Pass p = run (prog);
    REQUIRE (p.events.size() == 2);
    CHECK (ev (p, 0).noteCount == 3);
    CHECK (ev (p, 0).length == doctest::Approx (0.5));
    CHECK (ev (p, 1).onset == doctest::Approx (0.5));
    CHECK (deg (p, 0, 0) == 0);
    CHECK (deg (p, 0, 1) == 2);
    CHECK (deg (p, 0, 2) == 4);
    CHECK (deg (p, 1, 1) == 3);
    CHECK (deg (p, 1, 2) == 5);
    CHECK (prog.info().kind == Info::Kind::Chords);
}

TEST_CASE("section 2: e5/16 x")
{
    Program prog = build ("e5/16 x");
    const Pass p = run (prog);
    REQUIRE (p.steps.size() == 16);
    const int hits[] = { 0, 4, 7, 10, 13 };
    REQUIRE (p.events.size() == 5);
    for (int i = 0; i < 5; ++i)
    {
        CHECK (ev (p, i).onset == doctest::Approx (hits[i] * 0.25));
        CHECK (ev (p, i).length == doctest::Approx (0.25));
        CHECK (ev (p, i).noteCount == 0);
    }
    CHECK (p.lengthBeats == doctest::Approx (4.0));
    CHECK (prog.info().lengthBeats == doctest::Approx (4.0));
}

TEST_CASE("section 2: e3/8+2 (1 3 5) plays the group in turn")
{
    const Pass p = run ("e3/8+2 (1 3 5)");
    REQUIRE (p.steps.size() == 8);
    REQUIRE (p.events.size() == 3);
    CHECK (ev (p, 0).onset == doctest::Approx (0.25));
    CHECK (ev (p, 1).onset == doctest::Approx (1.0));
    CHECK (ev (p, 2).onset == doctest::Approx (1.5));
    CHECK (degrees (p) == std::vector<int> { 0, 2, 4 });
}

TEST_CASE("section 2: (1 2 3)*3 4")
{
    const Pass p = run ("(1 2 3)*3 4");
    CHECK (degrees (p) == std::vector<int> { 0, 1, 2, 0, 1, 2, 0, 1, 2, 3 });
    CHECK (p.lengthBeats == doctest::Approx (2.5));
}

TEST_CASE("section 2: 1 [3|5|7] 1 2 by pass")
{
    Program prog = build ("1 [3|5|7] 1 2");
    CHECK (degrees (run (prog, withSeed (1, 0))) == std::vector<int> { 0, 2, 0, 1 });
    CHECK (degrees (run (prog, withSeed (1, 1))) == std::vector<int> { 0, 4, 0, 1 });
    CHECK (degrees (run (prog, withSeed (1, 2))) == std::vector<int> { 0, 6, 0, 1 });
    CHECK (degrees (run (prog, withSeed (1, 3))) == std::vector<int> { 0, 2, 0, 1 });
}

TEST_CASE("section 2: key-scale block")
{
    const Pass p = run ("\"1 2 3\" 4");
    REQUIRE (p.events.size() == 4);
    CHECK (noteOf (p, 0).scale == ScaleKind::Key);
    CHECK (noteOf (p, 2).scale == ScaleKind::Key);
    CHECK (noteOf (p, 3).scale == ScaleKind::Chord);
    CHECK (deg (p, 3) == 3);
}

TEST_CASE("section 2: glide and plain numbers between quotes")
{
    Program prog = build ("~v'0.2' x . . v'0.9' x");
    REQUIRE (prog.ok());
    const Pass p = run (prog);
    REQUIRE (p.events.size() == 2);
    CHECK (ev (p, 0).glide);
    CHECK_FALSE (ev (p, 1).glide);
    CHECK (ev (p, 0).velocity == doctest::Approx (0.2f));
    CHECK (ev (p, 1).velocity == doctest::Approx (0.9f));
    CHECK (prog.info().glides);
}

TEST_CASE("section 2: comments to the end of the line")
{
    const Pass p = run ("1 2 ; a comment 3 4\n5");
    CHECK (degrees (p) == std::vector<int> { 0, 1, 4 });
}

//==============================================================================
// Syntax 2 commands

TEST_CASE("syntax 2: 0 and = repeat the last degree, + - cycle within chordSize")
{
    CHECK (degrees (run ("3 0 = 0", 2)) == std::vector<int> { 2, 2, 2, 2 });
    Context c;
    c.chordSize = 4;
    CHECK (degrees (run ("1 + + + + - -", 2, c)) == std::vector<int> { 0, 1, 2, 3, 0, 3, 2 });
    CHECK (degrees (run ("0 + -", 2, c)) == std::vector<int> { 0, 1, 0 });
}

TEST_CASE("syntax 2: ? draws only among the present degrees")
{
    Context c;
    c.chordSize = 5;
    c.chordPresent = 0b10101;                       // degrees 0, 2, 4
    Program prog = build ("?", 2);
    std::set<int> seen;
    for (std::uint64_t s = 0; s < 100; ++s)
    {
        c.seed = s;
        const Pass p = run (prog, c);
        REQUIRE (p.events.size() == 1);
        const int expected[] = { 0, 2, 4 };
        CHECK (deg (p, 0) == expected[static_cast<int> (draw (s, 0) * 3)]);
        seen.insert (deg (p, 0));
    }
    CHECK (seen == std::set<int> { 0, 2, 4 });
}

TEST_CASE("syntax 2: ? updates the last degree, so + goes on from it")
{
    Context c;
    c.seed = 5;
    const Pass p = run ("? +", 2, c);
    REQUIRE (p.events.size() == 2);
    CHECK (deg (p, 1) == (deg (p, 0) + 1) % 7);
}

TEST_CASE("syntax 2: _ . # b")
{
    const Pass p = run ("1 _ _ . _ #2 b3", 2);
    REQUIRE (p.events.size() == 3);
    CHECK (ev (p, 0).length == doctest::Approx (0.75));      // held twice
    CHECK (p.steps[4].event == -1);                         // a hold after a rest holds nothing
    CHECK (noteOf (p, 1).accidental == 1);
    CHECK (noteOf (p, 2).accidental == -1);
    CHECK (ev (p, 1).onset == doctest::Approx (1.25));
}

TEST_CASE("syntax 2: octave and velocity prefixes")
{
    Program prog = build ("o3 1 2 O5 3 o+ 4 5 O- 6 o0 7 O? 1", 2);
    const Pass p = run (prog, withSeed (3));
    REQUIRE (p.events.size() == 8);
    CHECK (noteOf (p, 0).octave == 3);
    CHECK (noteOf (p, 1).octave == 4);
    CHECK (noteOf (p, 2).octave == 5);
    CHECK (noteOf (p, 3).octave == 6);
    CHECK (noteOf (p, 4).octave == 5);
    CHECK (noteOf (p, 5).octave == 4);
    CHECK (noteOf (p, 6).octave == 0);
    CHECK (noteOf (p, 7).octave >= 0);
    CHECK (noteOf (p, 7).octave <= 7);

    const Pass v = run ("v9 1 2 V8 3 vF 4 5 V+ 6 V- 7", 2);
    REQUIRE (v.events.size() == 7);
    CHECK (ev (v, 0).velocity == doctest::Approx (vel (9)));
    CHECK (ev (v, 1).velocity < 0.0f);
    CHECK (ev (v, 2).velocity == doctest::Approx (vel (8)));
    CHECK (ev (v, 3).velocity == doctest::Approx (vel (15)));
    CHECK (ev (v, 4).velocity == doctest::Approx (vel (8)));
    CHECK (ev (v, 5).velocity == doctest::Approx (vel (9)));
    CHECK (ev (v, 6).velocity == doctest::Approx (vel (8)));
}

TEST_CASE("syntax 2: v+ v- step from the host velocity when the text sets none")
{
    Context c;
    c.hostVelocity = 96.0f / 127.0f;                  // level 11
    const Pass p = run ("v+ 1 v- 2 1", 2, c);
    REQUIRE (p.events.size() == 3);
    CHECK (ev (p, 0).velocity == doctest::Approx (vel (12)));
    CHECK (ev (p, 1).velocity == doctest::Approx (vel (10)));
    CHECK (ev (p, 2).velocity < 0.0f);
}

TEST_CASE("syntax 2: random octave and velocity draw reproducibly")
{
    Program prog = build ("o? 1 v? 2", 2);
    std::set<int> octs;
    for (std::uint64_t s = 0; s < 200; ++s)
    {
        const Pass a = run (prog, withSeed (s));
        const Pass b = run (prog, withSeed (s));
        CHECK (noteOf (a, 0).octave == noteOf (b, 0).octave);
        CHECK (ev (a, 1).velocity == ev (b, 1).velocity);
        CHECK (noteOf (a, 0).octave == std::min (7, 1 + static_cast<int> (draw (s, 0) * 9)));
        octs.insert (noteOf (a, 0).octave);
        const int level = 1 + static_cast<int> (draw (s, 1) * 15);
        CHECK (ev (a, 1).velocity == doctest::Approx (vel (level)));
    }
    CHECK (octs.size() >= 6);

    // Syntax 3 draws octaves from 0 to 7.
    Program p3 = build ("o? 1");
    std::set<int> o3;
    for (std::uint64_t s = 0; s < 300; ++s)
        o3.insert (noteOf (run (p3, withSeed (s)), 0).octave);
    CHECK (*o3.begin() == 0);
    CHECK (*o3.rbegin() == 7);
}

TEST_CASE("syntax 2: scopes restore O and V")
{
    const Pass p = run ("(O+ V8 1) 2 (O+ O+ 3 (V9 4) 5) 6", 2);
    REQUIRE (p.events.size() == 6);
    CHECK (noteOf (p, 0).octave == 5);
    CHECK (ev (p, 0).velocity == doctest::Approx (vel (8)));
    CHECK (noteOf (p, 1).octave == 4);
    CHECK (ev (p, 1).velocity < 0.0f);
    CHECK (noteOf (p, 2).octave == 6);
    CHECK (ev (p, 3).velocity == doctest::Approx (vel (9)));
    CHECK (ev (p, 4).velocity < 0.0f);
    CHECK (noteOf (p, 4).octave == 6);
    CHECK (noteOf (p, 5).octave == 4);
}

TEST_CASE("syntax 2: an odd number of quotes keeps toggling, across passes through Carry")
{
    Program prog = build ("1 \"2 3", 2);
    Carry carry;
    const Pass a = run (prog, {}, &carry);
    REQUIRE (a.events.size() == 3);
    CHECK (noteOf (a, 0).scale == ScaleKind::Chord);
    CHECK (noteOf (a, 1).scale == ScaleKind::Key);
    CHECK (carry.inKeyScale);
    const Pass b = run (prog, {}, &carry);
    CHECK (noteOf (b, 0).scale == ScaleKind::Key);                 // the block is still open
    CHECK (noteOf (b, 1).scale == ScaleKind::Chord);               // the quote closes it
    CHECK (prog.ok());
}

TEST_CASE("syntax 2: p on a step, every fallback kind")
{
    const std::uint64_t fail = seedWhere (0, [] (double d) { return d >= 0.1; });    // p1 fails
    const std::uint64_t pass = seedWhere (0, [] (double d) { return d < 0.1; });     // p1 succeeds

    // no fallback: a rest
    CHECK (degrees (run (build ("p1 3 2", 2), withSeed (fail))) == std::vector<int> { 1 });
    CHECK (degrees (run (build ("p1 3 2", 2), withSeed (pass))) == std::vector<int> { 2, 1 });

    // :_ holds what sounds
    {
        const Pass p = run (build ("1 p1 3:_", 2), withSeed (fail));
        REQUIRE (p.events.size() == 1);
        CHECK (ev (p, 0).length == doctest::Approx (0.5));
    }
    // :. is a rest
    {
        const Pass p = run (build ("1 p1 3:.", 2), withSeed (fail));
        REQUIRE (p.events.size() == 1);
        CHECK (ev (p, 0).length == doctest::Approx (0.25));
        CHECK (p.steps[1].event == -1);
    }
    // :5 plays another degree
    CHECK (degrees (run (build ("p1 3:5", 2), withSeed (fail))) == std::vector<int> { 4 });
    CHECK (degrees (run (build ("p1 3:5", 2), withSeed (pass))) == std::vector<int> { 2 });
    // :? plays a drawn degree (the next draw)
    {
        const Pass p = run (build ("p1 3:?", 2), withSeed (fail));
        REQUIRE (p.events.size() == 1);
        CHECK (deg (p, 0) == static_cast<int> (draw (fail, 1) * 7));
    }
    // :(group) plays the group
    CHECK (degrees (run (build ("p1 3:(4 5) 1", 2), withSeed (fail))) == std::vector<int> { 3, 4, 0 });
    CHECK (degrees (run (build ("p1 3:(4 5) 1", 2), withSeed (pass))) == std::vector<int> { 2, 0 });
    // the step's local prefixes still apply to the fallback
    {
        const Pass p = run (build ("p1 o+ 3:5", 2), withSeed (fail));
        CHECK (noteOf (p, 0).octave == 5);
    }
}

TEST_CASE("syntax 2: forced rests turn _ into . and . into _")
{
    const std::uint64_t fail = seedWhere (0, [] (double d) { return d >= 0.1; });
    {
        const Pass p = run (build ("1 p1 _ 2", 2), withSeed (fail));      // _ becomes a rest
        REQUIRE (p.events.size() == 2);
        CHECK (ev (p, 0).length == doctest::Approx (0.25));
        CHECK (p.steps[1].event == -1);
    }
    {
        const Pass p = run (build ("1 p1 . 2", 2), withSeed (fail));      // . becomes a hold
        REQUIRE (p.events.size() == 2);
        CHECK (ev (p, 0).length == doctest::Approx (0.5));
        CHECK (p.steps[1].event == 0);
    }
}

TEST_CASE("syntax 2: group probability, with and without a fallback")
{
    const std::uint64_t fail = seedWhere (0, [] (double d) { return d >= 0.5; });
    const std::uint64_t pass = seedWhere (0, [] (double d) { return d < 0.5; });

    Program both = build ("p5 (1 2):(3 4 5) 6", 2);
    CHECK (degrees (run (both, withSeed (pass))) == std::vector<int> { 0, 1, 5 });
    CHECK (degrees (run (both, withSeed (fail))) == std::vector<int> { 2, 3, 4, 5 });
    CHECK_FALSE (both.info().fixedLength);

    // no fallback: the group is skipped and the next step becomes a rest (TeAr)
    Program lone = build ("p5 (1 2) 3 4", 2);
    CHECK (degrees (run (lone, withSeed (pass))) == std::vector<int> { 0, 1, 2, 3 });
    const Pass f = run (lone, withSeed (fail));
    CHECK (degrees (f) == std::vector<int> { 3 });
    CHECK (f.steps.size() == 2);
    CHECK (f.steps[0].event == -1);
    CHECK_FALSE (lone.info().fixedLength);
}

TEST_CASE("syntax 2: Carry makes O+ drift across passes; syntax 3 resets")
{
    Program p2 = build ("O+ 1 3", 2);
    Carry carry;
    CHECK (noteOf (run (p2, {}, &carry), 0).octave == 5);
    CHECK (noteOf (run (p2, {}, &carry), 0).octave == 6);
    CHECK (carry.octave == 6);

    Program p3 = build ("O+ 1 3", 3);
    Carry c3;
    CHECK (noteOf (run (p3, {}, &c3), 0).octave == 5);
    CHECK (noteOf (run (p3, {}, &c3), 0).octave == 5);
}

TEST_CASE("syntax 2: trailing prefixes reach the first step of the next pass")
{
    Program prog = build ("1 2 o+", 2);
    Carry carry;
    const Pass a = run (prog, {}, &carry);
    CHECK (noteOf (a, 0).octave == 4);
    const Pass b = run (prog, {}, &carry);
    CHECK (noteOf (b, 0).octave == 5);                 // the local o+ waited
    CHECK (noteOf (b, 1).octave == 4);
    // in syntax 3 they are a warning and ignored
    Program p3 = build ("1 2 o+", 3);
    CHECK (p3.ok());
    REQUIRE (p3.diagnostics().size() == 1);
    CHECK (p3.diagnostics()[0].warning);
    CHECK (p3.diagnostics()[0].position == 4);
}

TEST_CASE("a hold at the start of a pass is tied to the previous pass's sound")
{
    const Pass p = run ("_ 1 . _ 2");
    REQUIRE (p.steps.size() == 5);
    CHECK (p.steps[0].tied);
    CHECK (p.steps[0].event == -1);
    CHECK_FALSE (p.steps[3].tied);                     // after a rest it holds nothing
    CHECK (p.steps[3].event == -1);
    CHECK_FALSE (p.steps[1].tied);
}

TEST_CASE("syntax 2 skips unknown characters, gives syntax 3's signs no meaning")
{
    Program prog = build ("1 w x 2 { 3 } [ 4 | 5 ] * ;", 2);
    CHECK (prog.ok());
    CHECK (prog.diagnostics().empty());
    CHECK (degrees (run (prog)) == std::vector<int> { 0, 1, 2, 3, 4 });
}

//==============================================================================
// Syntax 3

TEST_CASE("chords: prefixes outside and inside, holds, + after a chord")
{
    {
        const Pass p = run ("o+{135}");
        REQUIRE (p.events[0].noteCount == 3);
        for (int i = 0; i < 3; ++i) CHECK (noteOf (p, 0, i).octave == 5);
    }
    {
        const Pass p = run ("v9{135}");
        CHECK (ev (p, 0).velocity == doctest::Approx (vel (9)));
    }
    {
        const Pass p = run ("{1 o+5 #3}");
        REQUIRE (p.events[0].noteCount == 3);
        CHECK (noteOf (p, 0, 0).octave == 4);
        CHECK (noteOf (p, 0, 1).octave == 5);
        CHECK (noteOf (p, 0, 1).degree == 4);
        CHECK (noteOf (p, 0, 2).accidental == 1);
        CHECK (noteOf (p, 0, 2).octave == 4);
    }
    {
        const Pass p = run ("{35} + {53} -");          // + and - go from the chord's first note
        REQUIRE (p.events.size() == 4);
        CHECK (deg (p, 1) == 3);
        CHECK (deg (p, 3) == 3);
    }
    {
        const Pass p = run ("{135} _ _ 1");
        CHECK (ev (p, 0).length == doctest::Approx (0.75));
    }
    {
        const Pass p = run ("{1 + +}");                // inside, + goes on from the previous note
        CHECK (noteOf (p, 0, 0).degree == 0);
        CHECK (noteOf (p, 0, 1).degree == 1);
        CHECK (noteOf (p, 0, 2).degree == 2);
    }
}

TEST_CASE("m scopes and triplets, exact onsets")
{
    {
        const Pass p = run ("m8 1 2 (m16 3 4) 5");
        const double onsets[] = { 0.0, 0.5, 1.0, 1.25, 1.5 };
        REQUIRE (p.events.size() == 5);
        for (int i = 0; i < 5; ++i) CHECK (ev (p, i).onset == doctest::Approx (onsets[i]));
        CHECK (p.lengthBeats == doctest::Approx (2.0));
    }
    {
        const Pass p = run ("m8t 1 2 3 m4t 4 5 m1 6 m2 7 m64 8");
        REQUIRE (p.events.size() == 8);
        CHECK (ev (p, 1).onset == doctest::Approx (1.0 / 3.0));
        CHECK (ev (p, 3).onset == doctest::Approx (1.0));
        CHECK (ev (p, 4).onset == doctest::Approx (1.0 + 2.0 / 3.0));
        CHECK (ev (p, 5).onset == doctest::Approx (1.0 + 4.0 / 3.0));
        CHECK (ev (p, 6).onset == doctest::Approx (1.0 + 4.0 / 3.0 + 4.0));
        CHECK (ev (p, 7).onset == doctest::Approx (1.0 + 4.0 / 3.0 + 6.0));
        CHECK (ev (p, 7).length == doctest::Approx (0.0625));
    }
    {
        Program prog = build ("m16t 1 2", 3, nullptr, 0.5);
        CHECK (prog.info().shortestStepBeats == doctest::Approx (1.0 / 6.0));
        CHECK (prog.info().lengthBeats == doctest::Approx (1.0 / 3.0));
    }
}

TEST_CASE("repeats and variations")
{
    CHECK (degrees (run ("(1 2)*2 (3)*2")) == std::vector<int> { 0, 1, 0, 1, 2, 2 });
    // a scope is restored at each repetition
    const Pass p = run ("(O+ 1)*3 2");
    for (int i = 0; i < 3; ++i) CHECK (noteOf (p, i).octave == 5);
    CHECK (noteOf (p, 3).octave == 4);
    // an empty part, several steps in a part
    Program prog = build ("[1|] 2");
    CHECK (degrees (run (prog, withSeed (0, 0))) == std::vector<int> { 0, 1 });
    CHECK (degrees (run (prog, withSeed (0, 1))) == std::vector<int> { 1 });
    Program multi = build ("[1 2|3] 4");
    CHECK (degrees (run (multi, withSeed (0, 0))) == std::vector<int> { 0, 1, 3 });
    CHECK (degrees (run (multi, withSeed (0, 1))) == std::vector<int> { 2, 3 });
    // parts of different lengths: a warning, a pass that varies
    CHECK (multi.ok());
    CHECK_FALSE (multi.info().fixedLength);
    bool warned = false;
    for (const Diagnostic& d : multi.diagnostics()) warned = warned || d.warning;
    CHECK (warned);
}

TEST_CASE("gates")
{
    {
        const Pass p = run ("g5 1 2 gx 3");
        CHECK (ev (p, 0).length == doctest::Approx (0.125));
        CHECK (ev (p, 1).length == doctest::Approx (0.25));
        CHECK (ev (p, 2).length == doctest::Approx (0.25));
    }
    {
        const Pass p = run ("(G5 1 2) 3");
        CHECK (ev (p, 0).length == doctest::Approx (0.125));
        CHECK (ev (p, 1).length == doctest::Approx (0.125));
        CHECK (ev (p, 2).length == doctest::Approx (0.25));
    }
    {
        const Pass p = run ("g5 1 _ 2");                // the gate applies to the whole held length
        CHECK (ev (p, 0).length == doctest::Approx (0.25));
        CHECK (ev (p, 1).onset == doctest::Approx (0.5));
    }
}

TEST_CASE("generators: ranges drawn reproducibly, binary, gates, glides on hits")
{
    // e3-5/16: the number of hits is drawn
    Program prog = build ("e3-5/16 x");
    std::set<std::size_t> counts;
    for (std::uint64_t s = 0; s < 50; ++s)
    {
        const Pass a = run (prog, withSeed (s));
        const Pass b = run (prog, withSeed (s));
        CHECK (a.events.size() == b.events.size());
        CHECK (a.events.size() == static_cast<std::size_t> (3 + static_cast<int> (draw (s, 0) * 3)));
        CHECK (a.steps.size() == 16);
        counts.insert (a.events.size());
    }
    CHECK (counts == std::set<std::size_t> { 3, 4, 5 });

    // the rotation range, drawn after the hits
    Program rot = build ("e3/8+0-6 x");
    for (std::uint64_t s = 0; s < 20; ++s)
    {
        const int r = static_cast<int> (draw (s, 0) * 7);
        const Pass p = run (rot, withSeed (s));
        std::vector<double> onsets;
        for (int i = 0; i < 8; ++i)
            if (((i + r) * 3) % 8 < 3) onsets.push_back (i * 0.25);
        REQUIRE (p.events.size() == onsets.size());
        for (std::size_t i = 0; i < onsets.size(); ++i) CHECK (ev (p, static_cast<int> (i)).onset == doctest::Approx (onsets[i]));
    }

    // n8:32-251 x
    Program bin = build ("n8:32-251 x");
    for (std::uint64_t s = 0; s < 30; ++s)
    {
        const int value = 32 + static_cast<int> (draw (s, 0) * 220);
        const Pass p = run (bin, withSeed (s));
        CHECK (p.steps.size() == 8);
        std::vector<double> onsets;
        for (int i = 0; i < 8; ++i)
            if ((value >> (7 - i)) & 1) onsets.push_back (i * 0.25);
        REQUIRE (p.events.size() == onsets.size());
        for (std::size_t i = 0; i < onsets.size(); ++i) CHECK (ev (p, static_cast<int> (i)).onset == doctest::Approx (onsets[i]));
    }

    // a fixed binary number: no draw
    {
        const Pass p = run ("n4:10 x");                  // 1010
        REQUIRE (p.events.size() == 2);
        CHECK (ev (p, 0).onset == doctest::Approx (0.0));
        CHECK (ev (p, 1).onset == doctest::Approx (0.5));
    }

    // prefixes before a generator apply to every hit
    {
        const Pass p = run ("V8 o+e5/16 3");
        REQUIRE (p.events.size() == 5);
        for (int i = 0; i < 5; ++i)
        {
            CHECK (noteOf (p, i).octave == 5);
            CHECK (noteOf (p, i).degree == 2);
            CHECK (ev (p, i).velocity == doctest::Approx (vel (8)));
        }
    }
    // a chord on every hit, and a probability rolled for each hit
    {
        const Pass p = run ("e2/4 {135}");
        REQUIRE (p.events.size() == 2);
        CHECK (p.events[0].noteCount == 3);
    }
    {
        Program pr = build ("p5 e4/4 1");
        std::set<std::size_t> sizes;
        for (std::uint64_t s = 0; s < 100; ++s)
        {
            const Pass p = run (pr, withSeed (s));
            std::size_t expected = 0;
            for (std::uint64_t k = 0; k < 4; ++k) expected += draw (s, k) < 0.5 ? 1 : 0;
            CHECK (p.events.size() == expected);
            CHECK (p.steps.size() == 4);
            sizes.insert (p.events.size());
        }
        CHECK (sizes.size() == 5);
    }
}

TEST_CASE("syntax 3: group probability keeps the length")
{
    const std::uint64_t fail = seedWhere (0, [] (double d) { return d >= 0.5; });
    const std::uint64_t pass = seedWhere (0, [] (double d) { return d < 0.5; });
    Program lone = build ("p5 (1 2) 3");
    CHECK (lone.info().fixedLength);
    CHECK (degrees (run (lone, withSeed (pass))) == std::vector<int> { 0, 1, 2 });
    const Pass f = run (lone, withSeed (fail));
    CHECK (degrees (f) == std::vector<int> { 2 });
    CHECK (f.steps.size() == 3);
    CHECK (f.lengthBeats == doctest::Approx (0.75));
    CHECK (f.steps[2].onset == doctest::Approx (0.5));

    Program both = build ("p5 (1 2):(3 4) 5");
    CHECK (degrees (run (both, withSeed (pass))) == std::vector<int> { 0, 1, 4 });
    CHECK (degrees (run (both, withSeed (fail))) == std::vector<int> { 2, 3, 4 });

    // a step with a group of the same length
    Program step = build ("p5 1:(m8 2)");
    CHECK_FALSE (step.ok());                             // (m8 2) lasts 0.5, the step 0.25
    Program step2 = build ("p5 1:(2)");
    CHECK (step2.ok());
}

TEST_CASE("syntax 3: expressions through the host, one draw each")
{
    FakeExpressions fx2;
    Program p2 = build ("v'half' x o'three' 1 p'half' 2 g'half' 3", 3, &fx2);
    REQUIRE (p2.ok());
    const Pass p = run (p2, withSeed (11));
    // evaluate calls, in order: v (k=0), o (k=1), p (k=2), then the roll takes k=3, g (k=4)
    REQUIRE (fx2.seeds.size() == 4);
    CHECK (fx2.seeds[0] == mix (11 ^ mix (0)));
    CHECK (fx2.seeds[1] == mix (11 ^ mix (1)));
    CHECK (fx2.seeds[2] == mix (11 ^ mix (2)));
    CHECK (fx2.seeds[3] == mix (11 ^ mix (4)));
    CHECK (ev (p, 0).velocity == doctest::Approx (0.5f));
    CHECK (noteOf (p, 1).octave == 3);
    const bool two = draw (11, 3) < 0.5;
    CHECK (p.events.size() == (two ? 4u : 3u));
}

TEST_CASE("let lines are skipped; after the first step they are an error")
{
    Program ok = build ("let amp = /oto/agent/x\nlet lift = /oto/ambience/lift\nv'half' x", 3, nullptr);
    CHECK_FALSE (ok.ok());                               // no host: the expression is refused, not the let
    CHECK (firstError (ok).find ("expressions need a host") != std::string::npos);
    FakeExpressions fx;
    Program with = build ("let amp = /oto/agent/x\n; note\nlet lift = /oto/ambience/lift\nv'half' x", 3, &fx);
    CHECK (with.ok());
    CHECK (run (with).events.size() == 1);
    Program late = build ("1 2\nlet a = /x");
    CHECK (hasError (late, 4, "let after the first step"));
    CHECK (build ("1 2 let").ok() == false);             // not at a line start: 'l' is unknown
}

//==============================================================================
// Draws

TEST_CASE("draws: same seed, same pass; different seeds differ; frequencies")
{
    Program prog = build ("p5 1 p5 2 p5 3 p5 4 ? ? ? ?");
    const Pass a = run (prog, withSeed (42));
    const Pass b = run (prog, withSeed (42));
    CHECK (degrees (a) == degrees (b));
    bool differs = false;
    for (std::uint64_t s = 0; s < 20; ++s)
        differs = differs || degrees (run (prog, withSeed (s))) != degrees (a);
    CHECK (differs);

    Program one = build ("p3 x");
    int hits = 0;
    const int n = 4000;
    for (int s = 0; s < n; ++s)
        hits += static_cast<int> (run (one, withSeed (static_cast<std::uint64_t> (s))).events.size());
    CHECK (static_cast<double> (hits) / n == doctest::Approx (0.3).epsilon (0.1));
}

//==============================================================================
// Errors

TEST_CASE("errors: one per message of section 11, with its position")
{
    CHECK (hasError (build ("1 w 2"), 2, "unknown character 'w'"));
    CHECK (hasError (build ("1 o"), 2, "prefix 'o' without a value"));
    CHECK (hasError (build ("o 1"), 0, "prefix 'o' without a value"));
    CHECK (hasError (build ("1 O9 2"), 3, "a value must be 0-7 here"));
    CHECK (hasError (build ("1 (2 3"), 2, "missing ')'"));
    CHECK (hasError (build ("1 2) 3"), 3, "')' without '('"));
    CHECK (hasError (build ("1 \"2 3"), 2, "unbalanced '\"'"));
    CHECK (hasError (build ("1 {2 3"), 2, "missing '}'"));
    CHECK (hasError (build ("{1 x}"), 3, "'x' or a rest inside a chord"));
    CHECK (hasError (build ("{1 .}"), 3, "'x' or a rest inside a chord"));
    CHECK (hasError (build ("{1 _}"), 3, "'x' or a rest inside a chord"));
    CHECK (hasError (build ("1 [2|3"), 2, "missing ']'"));
    CHECK (hasError (build ("1*4"), 1, "'*' needs a group before it"));
    CHECK (hasError (build ("(1)*1"), 4, "a repeat must be 2-64"));
    CHECK (hasError (build ("(1)*65"), 4, "a repeat must be 2-64"));
    CHECK (hasError (build ("(1)*"), 3, "a repeat must be 2-64"));
    CHECK (hasError (build ("1 p5 (1 2):(3)"), 2, "a probability's branches must last the same"));
    CHECK (hasError (build ("e5/0 x"), 0, "bad generator"));
    CHECK (hasError (build ("e5 x"), 0, "bad generator"));
    CHECK (hasError (build ("n40 x"), 0, "bad generator"));
    CHECK (hasError (build ("n4:20 x"), 0, "bad generator"));
    CHECK (hasError (build ("e5/16"), 0, "a generator needs what its hits play"));
    CHECK (hasError (build ("e5/16 ."), 0, "a generator needs what its hits play"));
    CHECK (hasError (build ("m3 1"), 0, "bad note value after 'm'"));
    CHECK (hasError (build ("1 m 2"), 2, "bad note value after 'm'"));
    CHECK (hasError (build ("v'foo' x"), 1, "in 'foo': expressions need a host"));
    CHECK (hasError (build ("1 2\nlet a = /x"), 4, "let after the first step"));

    FakeExpressions fx;
    CHECK (hasError (build ("1 v'bad' 2", 3, &fx), 3, "in 'bad': unknown function 'foo'"));
    CHECK (hasError (build ("v'half", 3, &fx), 1, "missing closing quote"));
}

TEST_CASE("an invalid program realizes an empty pass")
{
    Program prog = build ("1 w 2");
    CHECK_FALSE (prog.ok());
    Pass p;
    p.events.push_back (Event {});
    prog.realize ({}, p);
    CHECK (p.events.empty());
    CHECK (p.steps.empty());
    CHECK (p.lengthBeats == 0.0);

    Program empty = build ("");
    CHECK (empty.ok());
    CHECK (run (empty).events.empty());
    Program blank = build ("  ; just a comment\n ");
    CHECK (blank.ok());
}

//==============================================================================
// Info and source ranges

TEST_CASE("Info: kind, glides, length, shortest step, bounds")
{
    {
        Program p = build ("x . x x");
        CHECK (p.info().kind == Info::Kind::Rhythm);
        CHECK (p.info().fixedLength);
        CHECK (p.info().lengthBeats == doctest::Approx (1.0));
        CHECK (p.info().shortestStepBeats == doctest::Approx (0.25));
        CHECK_FALSE (p.info().glides);
        CHECK (p.info().maxStepsPerPass == 4);
    }
    {
        Program p = build ("1 3 _ (m32 5 6)");
        CHECK (p.info().kind == Info::Kind::Melody);
        CHECK (p.info().shortestStepBeats == doctest::Approx (0.125));
        CHECK (p.info().lengthBeats == doctest::Approx (1.0));
    }
    {
        Program p = build ("1 {135} ~2");
        CHECK (p.info().kind == Info::Kind::Chords);
        CHECK (p.info().glides);
        CHECK (p.info().maxNotesPerPass == 5);
        CHECK (p.info().maxEventsPerPass == 3);
    }
    {
        Program p = build ("{1} 2");                     // one note: a melody
        CHECK (p.info().kind == Info::Kind::Melody);
    }
    {
        Program p = build ("(e3/8 x)*4");
        CHECK (p.info().maxStepsPerPass == 32);
        CHECK (p.info().maxEventsPerPass == 12);
        CHECK (p.info().lengthBeats == doctest::Approx (8.0));
    }
    {
        Program p = build ("x");
        CHECK (p.info().lengthBeats == doctest::Approx (0.25));
    }
}

TEST_CASE("Step source ranges include the prefixes")
{
    const std::string text = "1 o+2 (v9 3) x";
    Program prog = build (text.substr (0, 12));
    const Pass p = run (prog);
    REQUIRE (p.steps.size() == 3);
    CHECK (p.steps[0].sourceStart == 0);
    CHECK (p.steps[0].sourceEnd == 1);
    CHECK (p.steps[1].sourceStart == 2);
    CHECK (p.steps[1].sourceEnd == 5);
    CHECK (p.steps[2].sourceStart == 7);
    CHECK (p.steps[2].sourceEnd == 11);
}

TEST_CASE("realize allocates nothing once the pass is reserved")
{
    const char* texts[] = {
        "(e3-7/16+0-6 x)*4 (1 [2|3] {135})*3",
        "n16:0-65535 (1 3 5) e4-8/32 {135} p5 (1 2):(3 4) (O+ 1 2)*8",
        "p5 e8/16 x (m32 1 2 3 4)*16",
    };
    for (const char* t : texts)
    {
        Program prog = build (t);
        REQUIRE_MESSAGE (prog.ok(), t);
        Pass pass;
        pass.reserve (prog.info());
        const auto ec = pass.events.capacity(), nc = pass.notes.capacity(), sc = pass.steps.capacity();
        const auto* ed = pass.events.data();
        for (std::uint64_t s = 0; s < 500; ++s)
        {
            Context c;
            c.seed = s;
            c.passIndex = static_cast<int> (s);
            prog.realize (c, pass);
            CHECK (pass.events.size() <= ec);
            CHECK (pass.steps.size() <= sc);
        }
        CHECK (pass.events.capacity() == ec);
        CHECK (pass.notes.capacity() == nc);
        CHECK (pass.steps.capacity() == sc);
        CHECK (pass.events.data() == ed);
    }
}

//==============================================================================
// Equivalence with fxme::Arpeggiator (TeAr's engine) on syntax 2

namespace
{
    const std::vector<int> kChordScale { 0, 2, 4, 5, 7, 9, 11 };       // major
    const std::vector<int> kKeyScale   { 0, 2, 3, 5, 7, 8, 10 };       // minor

    struct Tok
    {
        int kind = 0;           // 0 nothing, 1 note-off only, 2 note-on
        int pitch = -1;
        int velocity = -1;
        bool operator== (const Tok& o) const { return kind == o.kind && pitch == o.pitch && velocity == o.velocity; }
    };

    std::string show (const std::vector<Tok>& v)
    {
        std::string s;
        for (const Tok& t : v)
            s += t.kind == 2 ? std::to_string (t.pitch) + "/" + std::to_string (t.velocity) + " "
               : t.kind == 1 ? "off " : "- ";
        return s;
    }

    std::vector<Tok> viaTeAr (const std::string& pattern, int steps)
    {
        fxme::Arpeggiator arp;
        fxme::MidiTools::Chord chord ("");
        chord.setDegreesDirect (kChordScale);
        fxme::MidiTools::Chord key ("");
        key.setDegreesDirect (kKeyScale);
        arp.setChord (chord);
        arp.setRootChord (key);
        arp.setPattern (pattern);
        arp.prepareToPlay (1000.0);
        arp.setTempo (150.0);
        arp.setSubdivision (4);
        arp.setChordMethod (2);
        std::vector<Tok> out;
        for (int i = 0; i < steps; ++i)
        {
            Tok t;
            auto buf = arp.processBlock (i == 0 ? 1 : 100);
            for (const auto& e : buf)
            {
                if (e.noteOn) { t.kind = 2; t.pitch = e.note; t.velocity = e.velocity; }
                else if (t.kind == 0) t.kind = 1;
            }
            out.push_back (t);
        }
        return out;
    }

    std::vector<Tok> viaCompiler (const std::string& pattern, int steps)
    {
        Program prog = build (pattern, 2);
        REQUIRE_MESSAGE (prog.ok(), pattern);
        Carry carry;
        std::vector<Tok> out;
        bool sounding = false;
        int passIndex = 0;
        while (static_cast<int> (out.size()) < steps)
        {
            Context c;
            c.passIndex = passIndex++;
            const Pass p = run (prog, c, &carry);
            REQUIRE_MESSAGE (! p.steps.empty(), pattern);
            for (std::size_t i = 0; i < p.steps.size() && static_cast<int> (out.size()) < steps; ++i)
            {
                Tok t;
                const int e = p.steps[i].event;
                if (e >= 0 && ev (p, e).firstStep == static_cast<int> (i))
                {
                    const Note& n = noteOf (p, e);
                    const auto& scale = n.scale == ScaleKind::Key ? kKeyScale : kChordScale;
                    t.kind = 2;
                    t.pitch = scale[static_cast<std::size_t> (n.degree % 7)] + 12 * n.octave + n.accidental;
                    const float v = ev (p, e).velocity;
                    t.velocity = v < 0.0f ? 96 : static_cast<int> (std::lround (v * 127.0f));
                    sounding = true;
                }
                else if (e >= 0 || p.steps[i].tied)
                    t.kind = 0;                                     // a hold: TeAr sends nothing
                else
                {
                    t.kind = sounding ? 1 : 0;
                    sounding = false;
                }
                out.push_back (t);
            }
        }
        return out;
    }
}

TEST_CASE("syntax 2 plays exactly as fxme::Arpeggiator does, step by step")
{
    const char* patterns[] = {
        "1 2 3 4 5 6 7",
        "1 3 5 8",
        "1 _ 5 .",
        "1 . _ 3",
        "1 + + - 5 -",
        "1 = = 3 =",
        "0 1 0 3 0",
        "o+1 2 o-3 4",
        "O+ 1 2",
        "1 2 O+",
        "O+ 1 O- 2",
        "(O+ 1 2) 3 4",
        "(O+ 1 (O+ 2 3) 4) 5",
        "V8 1 2 (VF 3 4) 5",
        "v9 1 2 vF3 4",
        "V+ 1 V+ 2 V- 3",
        "v+ 1 v+ 2 V3 v- 4",
        "#1 b2 3 #b4",
        "\"1 2 3\" 4 5",
        "1 \"2 3 4 5",
        "\"1 2\" \"3 4\"",
        "o+ (1 2) 3",
        "(1 2 o+) 3 4",
        "vF0 1 0",
        "o3 1 O2 2 o+ 3",
        "1 2 3 O+ O+ 4",
        "8 9 A B C D E F",
        "(((1 2) 3) 4) 5",
        "O- 1 O- 2 O- 3 O- 4 O- 5",
        "O+ O+ O+ 1 2",
        "_ 1 _ . _ 2",
        "  1   2  ",
        "1 w 2 x 3",
        "1 ) 2",
        "1 2 o+ #",
        "V8 1 2 V+ ",
        "\"O+ 1 2\" 3 \"4\"",
    };
    for (const char* pattern : patterns)
    {
        const auto expected = viaTeAr (pattern, 64);
        const auto actual = viaCompiler (pattern, 64);
        CHECK_MESSAGE (actual == expected, "pattern: ", std::string (pattern), "\n  TeAr:     ", show (expected), "\n  compiler: ", show (actual));
    }
}
