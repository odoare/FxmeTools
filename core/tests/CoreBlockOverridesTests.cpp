/*
  ------------------------------------------------------------------------------
    CoreBlockOverridesTests.cpp

    The block override language (midi/BlockOverrides.h) with a language of its
    own, as a host other than Mango would define it: keys with and without
    symbols, variables with and without voice digits, names that prefix one
    another. And StringSequencer's settable step limit. Mango keeps its own,
    more exhaustive tests of its language on top of this one.

    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/midi/BlockOverrides.h>
#include <FxmeTools/midi/StringSequencer.h>

#include <cmath>
#include <cstdio>

namespace
{
    int failures = 0;
    int checks   = 0;

    void check (bool condition, const char* what, int line)
    {
        ++checks;
        if (! condition)
        {
            ++failures;
            std::printf ("  FAIL (line %d): %s\n", line, what);
        }
    }

    #define CHECK(cond)              check ((cond), #cond, __LINE__)
    #define CHECK_CLOSE(a, b, tol)   check (std::fabs ((double) (a) - (double) (b)) <= (tol), #a " ~= " #b, __LINE__)

    // A small language: three keys, `mode` taking symbols; four variables,
    // `bar` and `barline` prefixing one another, `freq` with up to 3 voices.
    const fxme::OverrideSymbol modes[] = { { "up", 0.0f }, { "down", 1.0f } };
    const fxme::OverrideKey keys[] = { { "dur" }, { "f0" }, { "mode", modes, 2 } };
    const fxme::OverrideVariable variables[] = { { "bar" }, { "barline" }, { "freq", 3 }, { "beat" } };
    const fxme::OverrideLanguage language { keys, 3, variables, 4 };

    enum Key { Dur, F0, Mode };

    /** Variable values: bar 2 s, barline 7, freq 100 Hz x voice (voice 0: 50), beat 0.5 s. */
    struct Source
    {
        float variableValue (int variable, int voice) const
        {
            switch (variable)
            {
                case 0:  return 2.0f;
                case 1:  return 7.0f;
                case 2:  return voice == 0 ? 50.0f : 100.0f * (float) voice;
                default: return 0.5f;
            }
        }
    };
}

int main()
{
    std::printf ("FxmeTools core block-override tests\n");
    const Source src;

    // Empty and whitespace: a valid empty set.
    CHECK (fxme::parseOverrides ("", language).has_value());
    CHECK (fxme::parseOverrides (" \t\n ", language)->count == 0);

    // Numbers, seconds, note values.
    auto a = fxme::parseOverrides ("dur=0.125 f0=440", language);
    CHECK (a && a->count == 2);
    CHECK (a->find (Dur)->kind == fxme::OverrideExpr::Const);
    CHECK_CLOSE (a->find (F0)->eval (src), 440.0, 1e-6);
    CHECK (a->find (Mode) == nullptr);

    auto s = fxme::parseOverrides ("dur=1.5s", language);
    CHECK (s && s->find (Dur)->kind == fxme::OverrideExpr::SecondsConst);
    CHECK_CLOSE (s->find (Dur)->eval (src), 1.5, 1e-6);

    auto n = fxme::parseOverrides ("dur=t8*2", language);
    CHECK (n && n->find (Dur)->kind == fxme::OverrideExpr::Const);
    CHECK_CLOSE (n->find (Dur)->value, 2.0 * 0.125 * 2.0 / 3.0, 1e-6);
    CHECK (! fxme::parseOverrides ("dur=s5", language));

    // Variables: scaled, divided, prefixed by a number; the longest name wins.
    auto v = fxme::parseOverrides ("dur=bar/4 f0=2*freq2", language);
    CHECK (v && v->find (Dur)->kind == fxme::OverrideExpr::Variable);
    CHECK_CLOSE (v->find (Dur)->eval (src), 0.5, 1e-6);
    CHECK (v->find (F0)->voice == 2);
    CHECK_CLOSE (v->find (F0)->eval (src), 400.0, 1e-4);

    auto l = fxme::parseOverrides ("f0=barline", language);
    CHECK (l && l->find (F0)->variable == 1);
    CHECK_CLOSE (l->find (F0)->eval (src), 7.0, 1e-6);

    CHECK_CLOSE (fxme::parseOverrides ("f0=freq", language)->find (F0)->eval (src), 50.0, 1e-6);
    CHECK (! fxme::parseOverrides ("f0=freq4", language));     // past maxVoice: not clamped
    CHECK (! fxme::parseOverrides ("dur=bar2", language));     // `bar` takes no digit
    CHECK (! fxme::parseOverrides ("dur=bars", language));
    CHECK (! fxme::parseOverrides ("dur=bar/0", language));

    // Symbols, only on the key that has them.
    auto m = fxme::parseOverrides ("mode=down", language);
    CHECK (m && m->find (Mode)->value == 1.0f);
    CHECK (! fxme::parseOverrides ("f0=down", language));

    // All or nothing; unknown key; too many assignments.
    CHECK (! fxme::parseOverrides ("dur=0.25 f0=oops", language));
    CHECK (! fxme::parseOverrides ("q=1", language));
    std::string many;
    for (int i = 0; i < fxme::ParsedOverrides::kMaxEntries + 1; ++i)
        many += "f0=1 ";
    CHECK (! fxme::parseOverrides (many, language));

    // The sequencer's window: 64 steps at most by default, more when asked.
    fxme::StringSequencer seq;
    seq.setNumSteps (200);
    CHECK (seq.getNumSteps() == 64);
    seq.setMaxSteps (256);
    seq.setNumSteps (200);
    CHECK (seq.getNumSteps() == 200);
    CHECK (seq.addBlock (150, 10) >= 0);
    seq.setMaxSteps (128);   // the window follows; the block past it goes dormant
    CHECK (seq.getNumSteps() == 128 && seq.dormantCount() == 1);

    std::printf ("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
