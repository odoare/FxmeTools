#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <FxmeTools/midi/Arpeggiator.h>
#include "ArpeggiatorGolden.h"
#include "ArpeggiatorGoldenRecord.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

// The engine moved from CppMusicTools into FxmeTools; keep the test body
// written against the original (un-nested) names.
using fxme::Arpeggiator;
namespace MidiTools = fxme::MidiTools;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
//
// Timing rationale: sampleRate=1000, tempo=150 BPM, subdivision=4 (1/16)
//   samplesPerNote = 1000 * (60/150) / 4 = 100 samples
//
// First tick  → processBlock(1)  : fires immediately (samplesUntilNextNote==0),
//                                  leaves samplesUntilNextNote=99
// Each next tick → processBlock(100): fires at sample 99 inside the block,
//                                  leaves samplesUntilNextNote=99 again
//
// Every call therefore produces exactly one MIDI step. ✓

namespace
{
    // Expected MIDI note numbers for CM chord at baseOctave=4, chordMethod=0.
    // noteToPlay = semitone + octave*12 = semitone + 48.
    constexpr int C4 = 48;   // pattern '1' → degree 0 (semitone  0)
    constexpr int E4 = 52;   // pattern '2' → degree 1 (semitone  4)
    constexpr int G4 = 55;   // pattern '3' → degree 2 (semitone  7)
    constexpr int B4 = 59;   // pattern '4' → degree 3 (semitone 11, CM7 only)

    constexpr int DEFAULT_VEL = 96;
    constexpr int REST = -1;

    // Configures an Arpeggiator for deterministic, sample-accurate testing.
    void setup(Arpeggiator& arp,
               const std::string& pattern,
               const std::string& chordName = "CM")
    {
        arp.setChord(MidiTools::Chord(chordName));
        arp.setPattern(pattern);
        arp.prepareToPlay(1000.0);
        arp.setTempo(150.0);
        arp.setSubdivision(4);  // 1/16 → 100 samples/note
        arp.setChordMethod(0);  // "Notes played" mode
    }

    struct NoteEvent { int pitch = REST; int velocity = REST; };

    // Returns the first NoteOn in one processed step, or {REST, REST} if none.
    NoteEvent tick(Arpeggiator& arp, bool isFirst = false)
    {
        auto buf = arp.processBlock(isFirst ? 1 : 100);
        for (const auto& e : buf)
            if (e.noteOn)
                return { e.note, e.velocity };
        return {};
    }

    // Collects `n` consecutive steps and returns their NoteOn pitches.
    // REST (-1) is used for rest and sustain steps.
    std::vector<int> collectPitches(Arpeggiator& arp, int n)
    {
        std::vector<int> pitches;
        pitches.reserve(n);
        for (int i = 0; i < n; ++i)
            pitches.push_back(tick(arp, i == 0).pitch);
        return pitches;
    }
}

// ---------------------------------------------------------------------------
// 1. numSteps() — pattern length counting
// ---------------------------------------------------------------------------
TEST_CASE("numSteps basic patterns")
{
    Arpeggiator arp;

    arp.setPattern("1 2 3");
    CHECK(arp.numSteps() == 3);

    arp.setPattern("1 . _ + -");
    CHECK(arp.numSteps() == 5);

    arp.setPattern("= ?");
    CHECK(arp.numSteps() == 2);
}

TEST_CASE("numSteps with probability prefixes")
{
    Arpeggiator arp;

    // Single-step probability: pN counts as the one note step
    arp.setPattern("p5 1");
    CHECK(arp.numSteps() == 1);

    // Single-step with fallback: still 1 step
    arp.setPattern("p5 1:2");
    CHECK(arp.numSteps() == 1);

    // Group probability: the group contains 2 note steps
    arp.setPattern("p5 (1 2):(3 4)");
    CHECK(arp.numSteps() == 2);

    // Group probability without space before '('
    arp.setPattern("p5(12):(34)");
    CHECK(arp.numSteps() == 2);
}

// ---------------------------------------------------------------------------
// 2. Basic note sequences
// ---------------------------------------------------------------------------
TEST_CASE("Pattern '1 2 3' cycles through CM triad")
{
    Arpeggiator arp;
    setup(arp, "1 2 3");

    auto notes = collectPitches(arp, 6);  // two full cycles
    CHECK(notes == std::vector<int>{ C4, E4, G4, C4, E4, G4 });
}

TEST_CASE("Pattern '1 2 3 4' plays all four degrees of CM7")
{
    Arpeggiator arp;
    setup(arp, "1 2 3 4", "CM7");

    auto notes = collectPitches(arp, 4);
    CHECK(notes == std::vector<int>{ C4, E4, G4, B4 });
}

// ---------------------------------------------------------------------------
// 3. Rest (.) and sustain (_)
// ---------------------------------------------------------------------------
TEST_CASE("Rest '.' produces no note-on")
{
    Arpeggiator arp;
    setup(arp, "1 . 3");

    auto notes = collectPitches(arp, 3);
    CHECK(notes[0] == C4);
    CHECK(notes[1] == REST);
    CHECK(notes[2] == G4);
}

TEST_CASE("Sustain '_' produces no note-on")
{
    Arpeggiator arp;
    setup(arp, "1 _ 3");

    auto notes = collectPitches(arp, 3);
    CHECK(notes[0] == C4);
    CHECK(notes[1] == REST);  // sustain returns no note-on
    CHECK(notes[2] == G4);
}

// ---------------------------------------------------------------------------
// 4. Repeat and relative navigation
// ---------------------------------------------------------------------------
TEST_CASE("'=' repeats the last degree")
{
    Arpeggiator arp;
    setup(arp, "2 = =");

    auto notes = collectPitches(arp, 3);
    CHECK(notes == std::vector<int>{ E4, E4, E4 });
}

TEST_CASE("'+' steps forward through CM7 degrees")
{
    Arpeggiator arp;
    setup(arp, "1 + + +", "CM7");

    auto notes = collectPitches(arp, 4);
    CHECK(notes == std::vector<int>{ C4, E4, G4, B4 });
}

TEST_CASE("'-' steps backward")
{
    Arpeggiator arp;
    setup(arp, "3 -", "CM7");

    auto notes = collectPitches(arp, 2);
    CHECK(notes[0] == G4);
    CHECK(notes[1] == E4);
}

// ---------------------------------------------------------------------------
// 5. Random '?'
// ---------------------------------------------------------------------------
TEST_CASE("'?' always plays a valid CM chord note")
{
    Arpeggiator arp;
    setup(arp, "?");

    const std::set<int> validNotes{ C4, E4, G4 };
    for (int i = 0; i < 30; ++i)
    {
        int note = tick(arp, i == 0).pitch;
        CHECK(validNotes.count(note) == 1);
    }
}

// ---------------------------------------------------------------------------
// 6. Octave modifiers
// ---------------------------------------------------------------------------
TEST_CASE("'o+' raises pitch by one octave for the tagged note")
{
    Arpeggiator arp;
    setup(arp, "o+1 1");

    auto notes = collectPitches(arp, 2);
    CHECK(notes[0] == C4 + 12);  // one octave up
    CHECK(notes[1] == C4);       // back to normal
}

TEST_CASE("'o-' lowers pitch by one octave for the tagged note")
{
    Arpeggiator arp;
    setup(arp, "o-1 1");

    auto notes = collectPitches(arp, 2);
    CHECK(notes[0] == C4 - 12);
    CHECK(notes[1] == C4);
}

// ---------------------------------------------------------------------------
// 7. Sharp / flat modifiers
// ---------------------------------------------------------------------------
TEST_CASE("'#' raises pitch by one semitone")
{
    Arpeggiator arp;
    setup(arp, "#1 1");

    auto notes = collectPitches(arp, 2);
    CHECK(notes[0] == C4 + 1);
    CHECK(notes[1] == C4);
}

TEST_CASE("'b' lowers pitch by one semitone")
{
    Arpeggiator arp;
    setup(arp, "b2 2");

    auto notes = collectPitches(arp, 2);
    CHECK(notes[0] == E4 - 1);
    CHECK(notes[1] == E4);
}

// ---------------------------------------------------------------------------
// 8. Velocity modifiers
// ---------------------------------------------------------------------------
// Velocity levels are hexadecimal since syntax v2: 1-F spread over the full
// MIDI range, so 'F' is the maximum rather than '8'.
TEST_CASE("'vF' sets local note velocity to 127")
{
    Arpeggiator arp;
    setup(arp, "vF 1 1");

    auto e0 = tick(arp, true);
    auto e1 = tick(arp);

    CHECK(e0.pitch    == C4);
    CHECK(e0.velocity == 127);
    CHECK(e1.pitch    == C4);
    CHECK(e1.velocity == DEFAULT_VEL);  // local modifier expired
}

TEST_CASE("'V8' sets global velocity to mid range for subsequent notes")
{
    Arpeggiator arp;
    setup(arp, "V8 1 1 1");

    const int expected = Arpeggiator::velocityForLevel(8);   // 8 * 127 / 15
    CHECK(expected == 68);

    auto e0 = tick(arp, true);  // 'V8' consumed as prefix
    auto e1 = tick(arp);
    auto e2 = tick(arp);

    CHECK(e0.velocity == expected);
    CHECK(e1.velocity == expected);
    CHECK(e2.velocity == expected);
}

TEST_CASE("velocity levels span the full range monotonically")
{
    CHECK(Arpeggiator::velocityForLevel(1)  == 8);
    CHECK(Arpeggiator::velocityForLevel(15) == 127);

    for (int level = 2; level <= Arpeggiator::maxVelocityLevel; ++level)
        CHECK(Arpeggiator::velocityForLevel(level) > Arpeggiator::velocityForLevel(level - 1));
}

TEST_CASE("levelForVelocity inverts velocityForLevel")
{
    for (int level = 1; level <= Arpeggiator::maxVelocityLevel; ++level)
        CHECK(Arpeggiator::levelForVelocity(Arpeggiator::velocityForLevel(level)) == level);
}

TEST_CASE("'V+' and 'V-' step the global velocity by one level")
{
    using A = Arpeggiator;

    Arpeggiator arp;
    setup(arp, "V8 1 V+ 1 V+ 1 V- 1");

    auto e0 = tick(arp, true);   // 'V8' consumed as prefix
    auto e1 = tick(arp);         // 'V+' -> level 9
    auto e2 = tick(arp);         // 'V+' -> level 10
    auto e3 = tick(arp);         // 'V-' -> level 9

    CHECK(e0.velocity == A::velocityForLevel(8));
    CHECK(e1.velocity == A::velocityForLevel(9));
    CHECK(e2.velocity == A::velocityForLevel(10));
    CHECK(e3.velocity == A::velocityForLevel(9));
}

TEST_CASE("'v+' and 'v-' affect only the next note")
{
    using A = Arpeggiator;

    Arpeggiator arp;
    setup(arp, "V8 1 v+ 1 1");

    auto e0 = tick(arp, true);   // global level 8
    auto e1 = tick(arp);         // local step up
    auto e2 = tick(arp);         // back to the global level

    CHECK(e0.velocity == A::velocityForLevel(8));
    CHECK(e1.velocity == A::velocityForLevel(9));
    CHECK(e2.velocity == A::velocityForLevel(8));
}

TEST_CASE("global velocity saturates instead of wrapping or silencing")
{
    using A = Arpeggiator;

    Arpeggiator arp;
    setup(arp, "VF 1 V+ 1");

    auto e0 = tick(arp, true);
    auto e1 = tick(arp);

    CHECK(e0.velocity == 127);
    CHECK(e1.velocity == 127);   // already at the top

    Arpeggiator quiet;
    setup(quiet, "V1 1 V- 1");

    auto q0 = tick(quiet, true);
    auto q1 = tick(quiet);

    CHECK(q0.velocity == A::velocityForLevel(1));
    CHECK(q1.velocity == A::velocityForLevel(1));   // floors at 1, never 0
    CHECK(q1.velocity > 0);
}

// ---------------------------------------------------------------------------
// 8d. Played velocity vs. pattern velocity
// ---------------------------------------------------------------------------
// A pattern that says nothing about velocity follows the note the player is
// holding. A 'V' overrides that for as long as it is in scope, and neither
// source overwrites the other.
TEST_CASE("a pattern with no velocity command follows the played velocity")
{
    using A = Arpeggiator;

    Arpeggiator arp;
    setup(arp, "1 1");
    arp.setPlayedVelocityFromMidi(40);

    CHECK(tick(arp, true).velocity == A::velocityForLevel(A::levelForVelocity(40)));
    CHECK(tick(arp).velocity       == A::velocityForLevel(A::levelForVelocity(40)));
}

TEST_CASE("played velocity lands on the 15-level alphabet, not 8 coarse steps")
{
    using A = Arpeggiator;

    Arpeggiator arp;
    setup(arp, "1");
    arp.setPlayedVelocityFromMidi(100);

    // round(100 * 15/127) = 12, and velocityForLevel(12) = 102.
    CHECK(arp.effectiveVelocity() == A::velocityForLevel(12));
    CHECK(arp.effectiveVelocity() == 102);
    // The pre-fix mapping was ceil(100/16) = 7 levels of 16, i.e. 112.
    CHECK(arp.effectiveVelocity() != 112);

    // Every level of the alphabet is reachable from some incoming velocity,
    // which was not true of the 8-step mapping.
    for (int level = 1; level <= A::maxVelocityLevel; ++level)
    {
        Arpeggiator a;
        setup(a, "1");
        a.setPlayedVelocityFromMidi(A::velocityForLevel(level));
        CHECK(a.effectiveVelocity() == A::velocityForLevel(level));
    }
}

TEST_CASE("a played note does not overwrite a 'V' the pattern set")
{
    using A = Arpeggiator;

    Arpeggiator arp;
    setup(arp, "V4 1 1");
    arp.setPlayedVelocityFromMidi(127);

    CHECK(tick(arp, true).velocity == A::velocityForLevel(4));

    // Playing another note mid-pattern must not undo the 'V'.
    arp.setPlayedVelocityFromMidi(20);
    CHECK(tick(arp).velocity == A::velocityForLevel(4));
}

TEST_CASE("closing a block hands velocity back to the played note")
{
    using A = Arpeggiator;

    Arpeggiator arp;
    setup(arp, "(V4 1) 1");
    arp.setPlayedVelocityFromMidi(127);

    // Inside the block the 'V' rules; outside it there is no 'V' in effect,
    // so the played velocity applies again rather than the block's leftover.
    CHECK(tick(arp, true).velocity == A::velocityForLevel(4));
    CHECK(tick(arp).velocity       == 127);
}

TEST_CASE("reset drops the pattern's 'V' but keeps the played velocity")
{
    using A = Arpeggiator;

    Arpeggiator arp;
    setup(arp, "V4 1 1");
    arp.setPlayedVelocityFromMidi(127);

    CHECK(tick(arp, true).velocity == A::velocityForLevel(4));

    arp.reset();
    CHECK(arp.effectiveVelocity() == 127);
}

// ---------------------------------------------------------------------------
// 8e. Transport sync: the pattern is anchored to the song, not to its history
// ---------------------------------------------------------------------------
namespace
{
    // A host position in PPQ. The test setup runs 1/16 steps, so one step is
    // 0.25 PPQ and an eight-step pattern spans 2.0 PPQ (a half note).
    double hostAt(double ppq)
    {
        return ppq;
    }

    // The pitch of the first note the arpeggiator emits once synced to `ppq`.
    int firstNoteAt(Arpeggiator& arp, double ppq)
    {
        arp.syncToPlayHead(hostAt(ppq));
        return tick(arp).pitch;
    }
}

TEST_CASE("[REGRESSION] the pattern lands on the step the song position asks for")
{
    // 8 steps of 1/16. Song step 4 (ppq 1.0, beat 2) must play pattern step 4.
    // Before the fix the engine kept the 1/16 grid but always resumed at
    // whatever step it had reached, so this played C4 (its own step 0).
    Arpeggiator arp;
    setup(arp, "1 2 3 4 1 2 3 4", "CM7");

    CHECK(firstNoteAt(arp, 1.0) == C4);   // song step 4 → pattern step 4 → '1'

    Arpeggiator other;
    setup(other, "1 2 3 4 1 2 3 4", "CM7");
    CHECK(firstNoteAt(other, 0.25) == E4);  // song step 1 → '2'

    Arpeggiator third;
    setup(third, "1 2 3 4 1 2 3 4", "CM7");
    CHECK(firstNoteAt(third, 1.5) == G4);   // song step 6 → '3'
}

TEST_CASE("[REGRESSION] pattern phase does not depend on how much was played before")
{
    // The bug reported from Bitwig: a pattern resumed where the previous chord
    // left it, so the same bar position produced a different step depending on
    // history. Run one engine for an odd number of steps first; both must
    // still agree once the host position is known.
    Arpeggiator fresh, used;
    setup(fresh, "1 2 3 4 1 2 3 4", "CM7");
    setup(used,  "1 2 3 4 1 2 3 4", "CM7");

    for (int i = 0; i < 3; ++i)     // leave `used` three steps out of phase
        tick(used, i == 0);

    CHECK(firstNoteAt(fresh, 2.0) == firstNoteAt(used, 2.0));
    CHECK(firstNoteAt(fresh, 2.0) == C4);   // ppq 2.0 = song step 8 = one full pattern
}

TEST_CASE("[REGRESSION] a fixed-length pattern repeats at the same bar phase")
{
    // Two full patterns apart must give the same step, and the pattern must
    // come back to its first step every 2.0 PPQ.
    Arpeggiator arp;
    setup(arp, "1 2 3 4 1 2 3 4", "CM7");

    for (double bar = 0.0; bar < 8.0; bar += 2.0)
        CHECK(firstNoteAt(arp, bar) == C4);

    for (double bar = 0.0; bar < 8.0; bar += 2.0)
        CHECK(firstNoteAt(arp, bar + 0.75) == B4);   // song step 3 → '4'
}

TEST_CASE("host position jitter does not push the pattern a step late")
{
    // Hosts do not report exact binary fractions. A position a hair below a
    // step boundary must resolve to that boundary, not to the step after it.
    Arpeggiator arp;
    setup(arp, "1 2 3 4 1 2 3 4", "CM7");

    arp.syncToPlayHead(hostAt(1.0 - 1e-12));
    // Snapped onto the boundary: fire now, not a whole step from now.
    CHECK(arp.getSamplesUntilNextNote() == doctest::Approx(0.0));
    CHECK(tick(arp).pitch == C4);                 // song step 4, not step 5

    // A position genuinely between two steps still waits out the remainder.
    Arpeggiator mid;
    setup(mid, "1 2 3 4 1 2 3 4", "CM7");
    mid.syncToPlayHead(hostAt(1.125));            // half a step past song step 4
    CHECK(mid.getSamplesUntilNextNote() == doctest::Approx(50.0));
}

TEST_CASE("patternHasFixedLength recognises the variable-length forms")
{
    using A = Arpeggiator;

    // Fixed: no probability at all, or probability tagging a single step.
    CHECK(A::patternHasFixedLength("1 2 3 4"));
    CHECK(A::patternHasFixedLength("1 (O+ 2 3) 4"));
    CHECK(A::patternHasFixedLength("p5 1"));
    CHECK(A::patternHasFixedLength("p5 1:2"));
    CHECK(A::patternHasFixedLength("\"1 2\" 3"));

    // Variable: a group whose branches can differ in length, or a group that
    // collapses to a single rest when the roll fails.
    CHECK_FALSE(A::patternHasFixedLength("p5 (1 2):(3 4 5)"));
    CHECK_FALSE(A::patternHasFixedLength("p5(12):(34)"));
    CHECK_FALSE(A::patternHasFixedLength("p5 (1 2)"));
    CHECK_FALSE(A::patternHasFixedLength("1 p5 2:(3 4)"));
}

TEST_CASE("variable-length patterns keep the step grid but are left unanchored")
{
    // Documented limitation, not a target: numSteps() reports the success
    // branch only, so there is no single length to anchor against. The sync
    // must still place the grid, and must not corrupt the group traversal.
    Arpeggiator arp;
    setup(arp, "p0 (1 2):(3 4)", "CM7");

    arp.syncToPlayHead(hostAt(1.0));
    CHECK(arp.getSamplesUntilNextNote() == doctest::Approx(0.0));

    // p0 always fails, so the fallback group plays through intact.
    CHECK(tick(arp).pitch == G4);
    CHECK(tick(arp).pitch == B4);
}

// ---------------------------------------------------------------------------
// 8b. Hexadecimal value alphabet (syntax v2)
// ---------------------------------------------------------------------------
TEST_CASE("hexValue / valueChar round-trip over the whole alphabet")
{
    for (int v = 0; v <= 15; ++v)
        CHECK(Arpeggiator::hexValue(Arpeggiator::valueChar(v)) == v);

    CHECK(Arpeggiator::hexValue('0') == 0);
    CHECK(Arpeggiator::hexValue('9') == 9);
    CHECK(Arpeggiator::hexValue('A') == 10);
    CHECK(Arpeggiator::hexValue('F') == 15);

    // Not values: 'b' is the flat command, and lowercase hex is not part of
    // the alphabet precisely so that it cannot collide with it.
    CHECK(Arpeggiator::hexValue('b') == -1);
    CHECK(Arpeggiator::hexValue('a') == -1);
    CHECK(Arpeggiator::hexValue('f') == -1);
    CHECK(Arpeggiator::hexValue('G') == -1);
    CHECK(Arpeggiator::hexValue('.') == -1);
}

// ---------------------------------------------------------------------------
// 8a. Step counting: the three traversals must agree
// ---------------------------------------------------------------------------
TEST_CASE("getPatternIndexForStep and getStepForPatternIndex are inverses")
{
    Arpeggiator arp;

    const std::vector<std::string> patterns {
        "1 2 3",
        "vF 1 o3 2 p5 3",
        "1 2 (O+ 1 2 3) 1 2",
        "1 \"2 3\" 4",                 // root-relative block
        "\"1 2\" \"3 4\"",
        "p5 (1 2 3):(4 5) 1",
        "1 b2 #3 A F"
    };

    for (const auto& p : patterns)
    {
        setup(arp, p);
        const int steps = arp.numSteps();
        REQUIRE(steps > 0);

        for (int s = 0; s < steps; ++s)
            CHECK(arp.getStepForPatternIndex(arp.getPatternIndexForStep(s)) == s);
    }
}

TEST_CASE("root-relative markers are not musical steps")
{
    Arpeggiator arp;

    setup(arp, "1 \"2 3\" 4");
    CHECK(arp.numSteps() == 4);

    // The '"' before step 3 must not be counted, or every index past it slides.
    CHECK(arp.getStepForPatternIndex(arp.getPatternIndexForStep(3)) == 3);

    setup(arp, "\"1 2 3\"");
    CHECK(arp.numSteps() == 3);
}

TEST_CASE("hex degrees count as steps, lowercase 'b' stays the flat prefix")
{
    Arpeggiator arp;

    setup(arp, "1 A F");
    CHECK(arp.numSteps() == 3);

    // 'b' prefixes the note that follows, so this is two steps, not three.
    setup(arp, "1 b2");
    CHECK(arp.numSteps() == 2);

    // Uppercase 'B' is degree 11 and therefore a step of its own.
    setup(arp, "1 B 2");
    CHECK(arp.numSteps() == 3);
}

TEST_CASE("'b' still flattens while 'B' selects a degree")
{
    Arpeggiator arp;
    setup(arp, "b1 1");

    auto e0 = tick(arp, true);
    auto e1 = tick(arp);

    CHECK(e0.pitch == C4 - 1);   // flattened
    CHECK(e1.pitch == C4);       // local modifier expired
}

// ---------------------------------------------------------------------------
// 8c. Migration from syntax v1
// ---------------------------------------------------------------------------
TEST_CASE("v1 to v2 migration preserves the velocity each step produced")
{
    using A = Arpeggiator;

    // Old scale was min(127, level * 16); the new level is whichever lands
    // nearest that velocity.
    CHECK(A::migratePatternV1toV2("v8 1") == "vF 1");   // was 127, max
    CHECK(A::migratePatternV1toV2("v9 1") == "vF 1");   // also clamped to 127
    CHECK(A::migratePatternV1toV2("V4 1") == "V8 1");   // was 64
    CHECK(A::migratePatternV1toV2("v6 1") == "vB 1");   // was 96
    CHECK(A::migratePatternV1toV2("v7 1") == "vD 1");   // was 112

    for (int oldLevel = 1; oldLevel <= 9; ++oldLevel)
    {
        const std::string migrated = A::migratePatternV1toV2("v" + std::to_string(oldLevel) + " 1");
        const int newLevel = A::hexValue(migrated[1]);
        const int oldVel   = std::min(127, oldLevel * 16);

        CHECK(std::abs(A::velocityForLevel(newLevel) - oldVel) <= 5);
    }
}

TEST_CASE("v1 to v2 migration touches only velocity arguments")
{
    using A = Arpeggiator;

    // Octave and probability arguments stay decimal and must be left alone,
    // including when they are digits that would be valid velocity levels.
    CHECK(A::migratePatternV1toV2("o3 p5 1 v2 3") == "o3 p5 1 v4 3");
    CHECK(A::migratePatternV1toV2("O5 1 2 3")     == "O5 1 2 3");
    CHECK(A::migratePatternV1toV2("p5 1:2")       == "p5 1:2");

    // Degrees are unchanged: they meant the same in v1.
    CHECK(A::migratePatternV1toV2("1 2 3 4 5 6 7 8 9") == "1 2 3 4 5 6 7 8 9");

    // Non-numeric velocity arguments pass through.
    CHECK(A::migratePatternV1toV2("v? 1") == "v? 1");
    CHECK(A::migratePatternV1toV2("V+ 1") == "V+ 1");

    // Already-migrated patterns must survive a second pass unchanged, since
    // the migration is keyed off a stored version and should be idempotent
    // for anything already in the new alphabet.
    CHECK(A::migratePatternV1toV2("vF 1") == "vF 1");
    CHECK(A::migratePatternV1toV2("VB 1") == "VB 1");

    // A trailing prefix with no argument must not read past the end.
    CHECK(A::migratePatternV1toV2("1 v")  == "1 v");
}

// ---------------------------------------------------------------------------
// 9. Probability — p0 always fails
// ---------------------------------------------------------------------------
TEST_CASE("'p0 1' never produces a note (probability = 0)")
{
    Arpeggiator arp;
    setup(arp, "p0 1");

    auto notes = collectPitches(arp, 10);
    for (int n : notes)
        CHECK(n == REST);
}

TEST_CASE("'p0 1:2' always plays the fallback degree 2 (E4)")
{
    Arpeggiator arp;
    setup(arp, "p0 1:2");

    auto notes = collectPitches(arp, 8);
    for (int n : notes)
        CHECK(n == E4);
}

TEST_CASE("'p0 1:.' always produces a rest")
{
    Arpeggiator arp;
    setup(arp, "p0 1:.");

    auto notes = collectPitches(arp, 8);
    for (int n : notes)
        CHECK(n == REST);
}

TEST_CASE("'p0 1:_' always sustains (no note-on)")
{
    Arpeggiator arp;
    setup(arp, "p0 1:_");

    auto notes = collectPitches(arp, 8);
    for (int n : notes)
        CHECK(n == REST);
}

// ---------------------------------------------------------------------------
// 10. Probability — p5 statistical sanity check
// ---------------------------------------------------------------------------
TEST_CASE("'p5 1' produces notes roughly half the time")
{
    Arpeggiator arp;
    setup(arp, "p5 1");

    int hits = 0;
    const int N = 200;
    for (int i = 0; i < N; ++i)
        if (tick(arp, i == 0).pitch == C4) ++hits;

    // At 50% probability with N=200, the chance of 0 hits or 200 hits is negligible.
    CHECK(hits > 10);
    CHECK(hits < 190);
}

// ---------------------------------------------------------------------------
// 11. Scope blocks  ( )
// ---------------------------------------------------------------------------
TEST_CASE("Scope block '(o+1 2)' applies local octave only inside block")
{
    Arpeggiator arp;
    // Outside the block the global octave is unchanged.
    setup(arp, "(o+1 2) 1");

    auto notes = collectPitches(arp, 3);
    CHECK(notes[0] == C4 + 12);  // o+ applies locally
    CHECK(notes[1] == E4);       // still inside block, global octave unaffected
    CHECK(notes[2] == C4);       // outside block, octave restored
}

// ---------------------------------------------------------------------------
// BUG REGRESSION 1
// "p5 (12):(34)" — space between 'pN' and '(' was not skipped,
// causing the group to be parsed as single-step + literal scope openers,
// producing all four notes regardless of the probability roll.
//
// Fix: groupCheckPos skips spaces before testing for '('.
// ---------------------------------------------------------------------------
TEST_CASE("[REGRESSION] 'p0 (12):(34)' always plays the fallback group (34)")
{
    Arpeggiator arp;
    setup(arp, "p0 (12):(34)", "CM7");

    // p0 = always fail → always enter the fallback group (34)
    // Expected per 2-step cycle: degree 2 (G4=55) then degree 3 (B4=59)
    auto notes = collectPitches(arp, 6);

    // Every odd step (0,2,4,...) must be G4, every even-odd (1,3,5,...) B4.
    CHECK(notes[0] == G4);
    CHECK(notes[1] == B4);
    CHECK(notes[2] == G4);   // new cycle
    CHECK(notes[3] == B4);
    CHECK(notes[4] == G4);
    CHECK(notes[5] == B4);

    // Explicitly verify that E4 (degree 1, which belonged to the success group)
    // never appears — the old bug would sneak it in on the fail path.
    for (int n : notes)
        CHECK(n != E4);
}

TEST_CASE("[REGRESSION] 'p5 (12):(34)' each 2-step window uses only one group")
{
    Arpeggiator arp;
    setup(arp, "p5 (12):(34)", "CM7");

    const std::set<int> successGroup{ C4, E4 };
    const std::set<int> failGroup   { G4, B4 };

    // Run 40 full cycles (80 steps). Each consecutive pair of notes must come
    // entirely from the success group OR entirely from the fail group.
    // The old bug produced all four notes in one cycle, breaking this invariant.
    for (int cycle = 0; cycle < 40; ++cycle)
    {
        int n1 = tick(arp, cycle == 0).pitch;
        int n2 = tick(arp).pitch;

        bool bothSuccess = successGroup.count(n1) && successGroup.count(n2);
        bool bothFail    = failGroup.count(n1)    && failGroup.count(n2);
        CHECK((bothSuccess || bothFail));
    }
}

// ---------------------------------------------------------------------------
// BUG REGRESSION 2
// "p5 1:?" — '?' in the single-char fallback position was silently ignored,
// so on a failed roll the arpeggiator replayed lastPlayedDegreeIndex (always
// degree 0 = C4) instead of picking a random chord note.
//
// Fix: added 'else if (fallback == '?') { currentDegreeIndex = getRandomPresentDegree(); }'
// ---------------------------------------------------------------------------
TEST_CASE("[REGRESSION] 'p0 1:?' plays a random chord note, not always C4")
{
    Arpeggiator arp;
    setup(arp, "p0 1:?");  // p0 = always fail → always execute '?'

    // With CM (3 valid degrees: C4, E4, G4), over many iterations the buggy
    // version would return C4 every time; the fixed version returns varied notes.
    std::set<int> seenPitches;
    for (int i = 0; i < 50; ++i)
        seenPitches.insert(tick(arp, i == 0).pitch);

    // Must see more than one distinct pitch — proves '?' is randomising.
    CHECK(seenPitches.size() > 1);

    // All pitches must be valid CM chord members.
    const std::set<int> validNotes{ C4, E4, G4 };
    for (int p : seenPitches)
        CHECK(validNotes.count(p) == 1);
}

TEST_CASE("[REGRESSION] 'p5 1:?' uses '?' on fail but '1' on success")
{
    Arpeggiator arp;
    setup(arp, "p5 1:?");

    // On success the note must be C4 (degree '1').
    // On fail the note must be a random valid CM note (could also be C4 by chance,
    // but never a note outside the chord). We can only verify no invalid notes appear.
    const std::set<int> validNotes{ C4, E4, G4 };
    for (int i = 0; i < 60; ++i)
    {
        int note = tick(arp, i == 0).pitch;
        if (note != REST)
            CHECK(validNotes.count(note) == 1);
    }
}

// ---------------------------------------------------------------------------
// 12. Variable-length probability branches (characterisation)
// ---------------------------------------------------------------------------
//
// These pin down current behaviour rather than desired behaviour. When a
// probability fallback is a different length from its success branch, the
// number of steps a loop actually plays depends on the roll, and the roll
// happens lazily, at the moment playback reaches the 'p'. Two things follow,
// and both are why per-sequence position display is not possible yet:
//
//   * numSteps() measures the success branch only, always. It is a property of
//     the string, and with a variable-length pattern there is no single right
//     answer for it to give.
//   * getCurrentStepIndex() is derived from the character position by a walk
//     that skips the whole fallback, so every step inside a fallback group
//     reports the same index.
//
// Fixing this means resolving the rolls for a loop up front, at loop start, so
// the loop has a known length and a meaningful step index. Update these tests
// when that lands.

TEST_CASE("a probability fallback can change how many steps a loop plays")
{
    Arpeggiator arp;
    setup(arp, "p0 1:(1 2 3)");   // p0 never succeeds, so always the fallback

    // The string measures as one step (the success branch)...
    CHECK(arp.numSteps() == 1);

    // ...while the loop actually plays the fallback's three.
    auto notes = collectPitches(arp, 9);
    CHECK(notes == std::vector<int>{ C4, E4, G4, C4, E4, G4, C4, E4, G4 });
}

TEST_CASE("the reported step index does not advance inside a fallback group")
{
    Arpeggiator arp;
    setup(arp, "p0 1:(1 2 3)");

    std::vector<int> reported;
    for (int i = 0; i < 6; ++i)
    {
        tick(arp, i == 0);
        reported.push_back(arp.getCurrentStepIndex());
    }

    // Frozen: the walk behind it steps over the fallback as a single token.
    CHECK(reported == std::vector<int>{ 1, 1, 1, 1, 1, 1 });
}

// ---------------------------------------------------------------------------
// 11. Seeded randomness, octave wrap, note range, setDegreesDirect
// ---------------------------------------------------------------------------
namespace
{
    struct Played { int pitch; int velocity; bool on; };

    // Every event of `n` consecutive steps, flattened.
    std::vector<Played> collectEvents(Arpeggiator& arp, int n)
    {
        std::vector<Played> out;
        for (int i = 0; i < n; ++i)
            for (const auto& e : arp.processBlock(i == 0 ? 1 : 100))
                out.push_back({ e.note, e.velocity, e.noteOn });
        return out;
    }

    bool sameEvents(const std::vector<Played>& a, const std::vector<Played>& b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].pitch != b[i].pitch || a[i].velocity != b[i].velocity || a[i].on != b[i].on)
                return false;
        return true;
    }

    std::vector<Played> seededRun(std::int64_t seed)
    {
        Arpeggiator arp;
        setup(arp, "? p5 1 v?0", "CM7");
        arp.setSeed(seed);
        return collectEvents(arp, 64);
    }
}

TEST_CASE("setSeed makes the random choices reproducible")
{
    const auto a = seededRun(1234);
    const auto b = seededRun(1234);
    CHECK(a.size() > 0);
    CHECK(sameEvents(a, b));

    CHECK_FALSE(sameEvents(a, seededRun(98765)));
}

TEST_CASE("setOctaveWrap makes an explicit degree past the end continue upward")
{
    const std::vector<int> major { 0, 2, 4, 5, 7, 9, 11 };
    MidiTools::Chord scale("");
    scale.setDegreesDirect(major);

    Arpeggiator off;
    setup(off, "1 8 F");
    off.setChord(scale);
    const auto without = collectPitches(off, 3);
    CHECK(without[0] == C4);
    CHECK(without[1] == C4);          // '8' folds back onto the first degree
    CHECK(without[2] == C4);          // 'F' is degree 14: two laps, still folded

    Arpeggiator on;
    setup(on, "1 8 F 9");
    on.setChord(scale);
    on.setOctaveWrap(true);
    const auto with = collectPitches(on, 4);
    CHECK(with[0] == C4);
    CHECK(with[1] == C4 + 12);        // one octave above '1'
    CHECK(with[2] == C4 + 24);
    CHECK(with[3] == C4 + 2 + 12);    // '9' is the second degree, an octave up
}

TEST_CASE("setOctaveWrap leaves '+' cyclic within the chord")
{
    const std::vector<int> major { 0, 2, 4, 5, 7, 9, 11 };
    MidiTools::Chord scale("");
    scale.setDegreesDirect(major);

    Arpeggiator arp;
    setup(arp, "7 + +");
    arp.setChord(scale);
    arp.setOctaveWrap(true);
    const auto p = collectPitches(arp, 3);
    CHECK(p[0] == C4 + 11);
    CHECK(p[1] == C4);
    CHECK(p[2] == C4 + 2);
}

TEST_CASE("processBlock never emits a note outside 0..127")
{
    // A scale voiced far upward: at octave 7 (84) most of it is beyond 127.
    const std::vector<int> high { 5, 11, 16, 23, 30, 40, 50 };
    MidiTools::Chord chord("");
    chord.setDegreesDirect(high);

    Arpeggiator arp;
    setup(arp, "O7 ? F 1 2 3 4 5 6 7 8 9 A ? ? #F b1 o+7", "CM");
    arp.setChord(chord);
    arp.setOctaveWrap(true);
    arp.setSeed(7);

    bool sawOn = false;
    for (int i = 0; i < 200; ++i)
        for (const auto& e : arp.processBlock(i == 0 ? 1 : 100))
        {
            CHECK(e.note >= 0);
            CHECK(e.note <= 127);
            CHECK(e.channel >= 1);
            CHECK(e.channel <= 16);
            if (e.noteOn)
            {
                sawOn = true;
                CHECK(e.velocity >= 1);
                CHECK(e.velocity <= 127);
            }
        }
    CHECK(sawOn);

    // A note beyond the range is not remembered as the last played one.
    const std::vector<int> beyond { 50 };
    MidiTools::Chord chord2("");
    chord2.setDegreesDirect(beyond);
    Arpeggiator single;
    setup(single, "O7 1");
    single.setChord(chord2);
    CHECK(collectEvents(single, 4).empty());
    CHECK(single.getLastPlayedNote() == -1);
}

TEST_CASE("the note-off of a played note still goes out when the next one is out of range")
{
    const std::vector<int> mixed { 0, 50 };
    MidiTools::Chord chord("");
    chord.setDegreesDirect(mixed);

    Arpeggiator arp;
    setup(arp, "O7 1 2");
    arp.setChord(chord);
    const auto ev = collectEvents(arp, 2);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].on);
    CHECK(ev[0].pitch == 84);
    CHECK_FALSE(ev[1].on);
    CHECK(ev[1].pitch == 84);
    CHECK(arp.getLastPlayedNote() == -1);
}

TEST_CASE("Chord::setDegreesDirect sets the degrees as given, without allocating")
{
    MidiTools::Chord chord("CM");
    chord.ensureCapacity(16, 16);
    const int* before = chord.getDegrees().begin();

    const std::vector<int> notes { 2, 4, 6, 7, 9, 11, 13, 14 };
    chord.setDegreesDirect(notes);

    CHECK(chord.getName() == "Diatonic");
    REQUIRE(chord.getDegrees().size() == 8);
    for (int i = 0; i < 8; ++i)
        CHECK(chord.getDegree(i) == notes[(std::size_t) i]);
    CHECK(chord.getRawNotes().isEmpty());
    CHECK(chord.getDegrees().begin() == before);

    chord.setNotesByArray(notes);
    chord.setDegreesDirect(notes);
    CHECK(chord.getRawNotes().isEmpty());
}

// ---------------------------------------------------------------------------
// The engine as a player of the pattern language's passes
// ---------------------------------------------------------------------------
//
// Counting allocations: the whole executable's operator new / delete count
// while `g_countAllocations` is set, so that a test can say "nothing was
// allocated or freed" about a stretch of code (no CHECK inside such a stretch:
// the framework allocates).

namespace
{
    bool g_countAllocations = false;
    long g_allocations = 0, g_frees = 0;
}

void* operator new (std::size_t size)
{
    if (g_countAllocations) ++g_allocations;
    if (void* p = std::malloc (size > 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { if (p != nullptr && g_countAllocations) ++g_frees; std::free (p); }
void operator delete (void* p, std::size_t) noexcept { if (p != nullptr && g_countAllocations) ++g_frees; std::free (p); }

namespace
{
    namespace pl = fxme::pattern;

    struct Ev
    {
        int t = 0, note = 0, velocity = 0;
        bool on = false;
        bool operator== (const Ev& o) const { return t == o.t && note == o.note && velocity == o.velocity && on == o.on; }
    };

    // Plays the blocks in `sizes`, with absolute sample times.
    std::vector<Ev> play (Arpeggiator& arp, const std::vector<int>& sizes)
    {
        std::vector<Ev> out;
        int start = 0;
        for (int n : sizes)
        {
            for (const auto& e : arp.processBlock (n))
                out.push_back ({ start + e.sample, e.note, e.velocity, e.noteOn });
            start += n;
        }
        return out;
    }

    // `steps` blocks as the helpers above cut them: one sample, then 100 per step (absolute time 100 * step).
    std::vector<Ev> playSteps (Arpeggiator& arp, int steps)
    {
        std::vector<int> sizes (static_cast<std::size_t> (steps), 100);
        sizes[0] = 1;
        return play (arp, sizes);
    }

    const std::vector<int> kMajor { 0, 2, 4, 5, 7, 9, 11 };

    // Syntax 3, the major scale as the chord: degree n is the n-th note of C major at octave 4 (48 + ...).
    void setup3 (Arpeggiator& arp, const std::string& text, int syntax = 3)
    {
        MidiTools::Chord scale ("");
        scale.setDegreesDirect (kMajor);
        arp.setPatternSyntax (syntax);
        arp.setChord (scale);
        arp.setPattern (text);
        arp.prepareToPlay (1000.0);
        arp.setTempo (150.0);          // 400 samples a beat, 100 a step
        arp.setSubdivision (4);
        arp.setChordMethod (0);
    }

    std::vector<int> onsetTimes (const std::vector<Ev>& v)
    {
        std::vector<int> t;
        for (const Ev& e : v) if (e.on) t.push_back (e.t);
        return t;
    }

    std::vector<int> notesOn (const std::vector<Ev>& v)
    {
        std::vector<int> n;
        for (const Ev& e : v) if (e.on) n.push_back (e.note);
        return n;
    }
}

TEST_CASE("the rewritten engine plays the old engine's streams (golden recording)")
{
    int n = 0, m = 0;
    const arpgolden::Case* cases = arpgolden::inputs (n);
    const char* const* expected = arpgolden::expected (m);
    REQUIRE (n == m);
    for (int i = 0; i < n; ++i)
    {
        const std::string actual = arpgolden::record<Arpeggiator> (cases[i], 48);
        CHECK_MESSAGE (actual == expected[i], "case ", i, ": ", cases[i].pattern, " (", cases[i].chord, ", method ", cases[i].method, ")");
    }
}

TEST_CASE("the engine defaults to syntax 2 and compiles what it is given")
{
    Arpeggiator arp;
    CHECK (arp.getPatternSyntax() == 2);
    setup (arp, "1 2 3");
    CHECK (arp.getDiagnostics().empty());
    CHECK (arp.hasFixedLength());
    CHECK (arp.getProgram() != nullptr);
    CHECK (arp.getCurrentStepSource() == std::make_pair (-1, -1));

    tick (arp, true);
    CHECK (arp.getCurrentStepSource() == std::make_pair (0, 1));
    tick (arp);
    CHECK (arp.getCurrentStepSource() == std::make_pair (2, 3));
    CHECK (arp.getCurrentStepIndex() == 1);

    arp.setPattern ("p5 (1 2):(3 4 5)");
    CHECK_FALSE (arp.hasFixedLength());
}

TEST_CASE("syntax 3: a chord sounds together and is released together")
{
    Arpeggiator arp;
    setup3 (arp, "{135} _ 2 .");
    const auto ev = playSteps (arp, 8);

    // {135} is held for two steps: three note-ons at 0, three note-offs at 200 (before the next note-on).
    REQUIRE (ev.size() >= 8);
    for (int i = 0; i < 3; ++i)
    {
        CHECK (ev[(std::size_t) i].on);
        CHECK (ev[(std::size_t) i].t == 0);
    }
    CHECK (notesOn (std::vector<Ev> (ev.begin(), ev.begin() + 3)) == std::vector<int> { 48, 52, 55 });
    for (int i = 3; i < 6; ++i)
    {
        CHECK_FALSE (ev[(std::size_t) i].on);
        CHECK (ev[(std::size_t) i].t == 200);
    }
    CHECK (ev[6] == Ev { 200, 50, 96, true });
    CHECK (ev[7] == Ev { 300, 50, 0, false });
}

TEST_CASE("syntax 3: (m32 1 2 3 4) 5 6 starts its notes on the sample, whatever the blocks")
{
    const std::vector<int> expected { 0, 50, 100, 150, 200, 300, 400, 450, 500, 550, 600, 700, 800 };

    auto upTo800 = [] (std::vector<int> t)
    {
        t.erase (std::remove_if (t.begin(), t.end(), [] (int x) { return x > 800; }), t.end());
        return t;
    };

    Arpeggiator a;
    setup3 (a, "(m32 1 2 3 4) 5 6");
    const std::vector<int> onsets = upTo800 (onsetTimes (play (a, { 1, 99, 100, 100, 100, 100, 100, 100, 100, 100 })));   // 900
    CHECK (onsets == expected);

    Arpeggiator b;
    setup3 (b, "(m32 1 2 3 4) 5 6");
    const auto ev = play (b, { 37, 64, 111, 5, 300, 383 });                                                // 900 in other blocks
    CHECK (upTo800 (onsetTimes (ev)) == onsets);
    CHECK (notesOn (ev).size() >= 12);
    CHECK (notesOn (ev)[0] == 48);
    CHECK (notesOn (ev)[3] == 53);
    CHECK (notesOn (ev)[5] == 57);
}

TEST_CASE("syntax 3: a generator places its hits on the grid, pass after pass")
{
    Arpeggiator arp;
    setup3 (arp, "e3/8 1");
    const auto ev = playSteps (arp, 17);                       // two passes of 8 steps, plus the start of a third
    CHECK (onsetTimes (ev) == std::vector<int> { 0, 300, 600, 800, 1100, 1400, 1600 });
    for (int n : notesOn (ev))
        CHECK (n == 48);
}

TEST_CASE("syntax 3: [a|b] changes per pass")
{
    Arpeggiator arp;
    setup3 (arp, "[1|3]");
    CHECK (notesOn (playSteps (arp, 6)) == std::vector<int> { 48, 52, 48, 52, 48, 52 });

    // A restart begins again with the first part.
    arp.reset();
    CHECK (notesOn (playSteps (arp, 3)) == std::vector<int> { 48, 52, 48 });
}

TEST_CASE("syntax 3: O+ is reset at each pass, while syntax 2 lets it drift")
{
    Arpeggiator three;
    setup3 (three, "O+ 1");
    CHECK (notesOn (playSteps (three, 4)) == std::vector<int> { 60, 60, 60, 60 });

    Arpeggiator two;
    setup3 (two, "O+ 1", 2);
    CHECK (notesOn (playSteps (two, 5)) == std::vector<int> { 60, 72, 84, 84, 84 });
}

TEST_CASE("setSeed makes the passes reproducible, whatever came before")
{
    auto run = [] (std::int64_t before, std::int64_t seed)
    {
        Arpeggiator arp;
        setup3 (arp, "? ? [1|?] p5 ? (? ?)*2 v?3");
        if (before != 0)
        {
            arp.setSeed (before);
            playSteps (arp, 23);                              // the engine has been playing for a while
        }
        arp.setSeed (seed);
        arp.reset();
        std::vector<Ev> ev;
        for (int i = 0; i < 80; ++i)
            for (const auto& e : arp.processBlock (i == 0 ? 1 : 100))
                ev.push_back ({ 0, e.note, e.velocity, e.noteOn });   // times are not compared: the blocks differ
        return ev;
    };

    const auto fresh = run (0, 4242);
    CHECK (fresh.size() > 40);
    CHECK (run (0, 4242) == fresh);
    CHECK (run (9, 4242) == fresh);
    CHECK_FALSE (run (0, 777) == fresh);
}

TEST_CASE("without setSeed, a restart draws differently; two engines differ")
{
    auto notes = [] (Arpeggiator& arp) { return notesOn (playSteps (arp, 40)); };
    Arpeggiator a, b;
    setup3 (a, "? ? ? ?");
    setup3 (b, "? ? ? ?");
    const auto first = notes (a);
    a.reset();
    CHECK_FALSE (notes (a) == first);
    CHECK_FALSE (notes (b) == first);
}

TEST_CASE("setProgram swaps a compiled program without allocating or freeing")
{
    pl::Options options;
    options.syntax = 3;
    auto p1 = std::make_shared<const pl::Program> (pl::compile ("1 2 3 4", options));
    auto p2 = std::make_shared<const pl::Program> (pl::compile ("{135} _ 2 [4|6] ((7 8)*2 1)*3 e3/16 1", options));
    REQUIRE (p1->ok());
    REQUIRE (p2->ok());

    Arpeggiator arp;
    setup3 (arp, "1");
    arp.reservePass (p1->info());
    arp.reservePass (p2->info());
    arp.setProgram (p1);
    playSteps (arp, 6);

    g_allocations = g_frees = 0;
    g_countAllocations = true;
    std::shared_ptr<const pl::Program> back1 = arp.setProgram (p2);
    const int blocks = 3 * 18;
    for (int i = 0; i < blocks; ++i)
        arp.processBlock (i == 0 ? 1 : 100);
    std::shared_ptr<const pl::Program> back2 = arp.setProgram (p1);
    for (int i = 0; i < blocks; ++i)
        arp.processBlock (100);
    arp.reset();
    arp.syncToPlayHead (1.25);
    arp.processBlock (100);
    arp.turnOff();
    g_countAllocations = false;

    CHECK (g_allocations == 0);
    CHECK (g_frees == 0);
    CHECK (back1 == p1);               // the previous program comes back to the host, which frees it where it likes
    CHECK (back2 == p2);
    CHECK (arp.getProgram() == p1);
    CHECK (arp.getPattern() == "1 2 3 4");
}

TEST_CASE("setProgram takes its syntax from the program")
{
    pl::Options options;
    options.syntax = 2;
    auto p = std::make_shared<const pl::Program> (pl::compile ("O+ 1", options));
    Arpeggiator arp;
    setup3 (arp, "1");
    arp.setProgram (p);
    CHECK (arp.getPatternSyntax() == 2);
    CHECK (notesOn (playSteps (arp, 3)) == std::vector<int> { 60, 72, 84 });
}

TEST_CASE("a tied first step keeps the sound of the previous pass ringing")
{
    {
        Arpeggiator arp;
        setup3 (arp, "_ 2");
        const auto ev = playSteps (arp, 8);
        // Pass 1: the tie holds nothing, '2' sounds at 100. Pass 2 starts at 200: the tie extends '2',
        // which ends with the tie's step, at 300, when the next '2' begins.
        CHECK (ev == std::vector<Ev> {
            { 100, 50, 96, true },
            { 300, 50, 0, false }, { 300, 50, 96, true },
            { 500, 50, 0, false }, { 500, 50, 96, true },
            { 700, 50, 0, false }, { 700, 50, 96, true } });
    }
    {
        Arpeggiator arp;                       // after a rest a tie holds nothing
        setup3 (arp, "_ 2 .");
        const auto ev = playSteps (arp, 7);
        CHECK (ev == std::vector<Ev> {
            { 100, 50, 96, true }, { 200, 50, 0, false },
            { 400, 50, 96, true }, { 500, 50, 0, false } });
    }
    {
        Arpeggiator arp;                       // a chord is held as a whole
        setup3 (arp, "_ {13}");
        const auto ev = playSteps (arp, 5);
        int offs = 0;
        for (const Ev& e : ev) if (! e.on) { ++offs; CHECK (e.t == 300); }
        CHECK (offs == 2);
    }
}

TEST_CASE("an invalid syntax-3 text reports its diagnostics and keeps the previous pattern playing")
{
    Arpeggiator arp;
    setup3 (arp, "1 2 3");
    CHECK (arp.getDiagnostics().empty());
    CHECK (tick (arp, true).pitch == 48);
    CHECK (tick (arp).pitch == 50);

    arp.setPattern ("1 w 2");
    REQUIRE (! arp.getDiagnostics().empty());
    CHECK (arp.getDiagnostics()[0].position == 2);
    CHECK_FALSE (arp.getDiagnostics()[0].warning);
    CHECK (arp.getPattern() == "1 w 2");                  // the editor keeps what was typed
    CHECK (tick (arp).pitch == 52);                       // "1 2 3" goes on, without restarting
    CHECK (tick (arp).pitch == 48);

    arp.setPattern ("4 3");
    CHECK (arp.getDiagnostics().empty());
    CHECK (tick (arp).pitch == 53);
    CHECK (tick (arp).pitch == 52);
}

TEST_CASE("setPatternSyntax compiles the current text again")
{
    Arpeggiator arp;
    setup (arp, "1 [2|3]");                 // syntax 2 gives the signs no meaning: three steps
    CHECK (collectPitches (arp, 3) == std::vector<int> { C4, E4, G4 });
    arp.setPatternSyntax (3);               // now a step and a choice of two: pass 0 takes the first part
    CHECK (arp.getDiagnostics().empty());
    CHECK (arp.getPattern() == "1 [2|3]");
    arp.reset();
    CHECK (collectPitches (arp, 4) == std::vector<int> { C4, E4, C4, G4 });
}

TEST_CASE("syncToPlayHead anchors a syntax-3 pass to the song")
{
    Arpeggiator arp;
    setup3 (arp, "(m32 1 2 3 4) 5 6");     // one beat; notes at 0, 1/8, 1/4, 3/8, 1/2, 3/4
    arp.syncToPlayHead (8.5);              // half a beat into the pass: its fifth note is next
    CHECK (arp.getSamplesUntilNextNote() == doctest::Approx (0.0));
    const auto ev = play (arp, { 100 });
    REQUIRE (! ev.empty());
    CHECK (ev[0].note == 55);
    CHECK (ev[0].t == 0);

    Arpeggiator mid;
    setup3 (mid, "(m32 1 2 3 4) 5 6");
    mid.syncToPlayHead (8.0625);           // 1/16 beat in: the second note is 1/16 beat away, 25 samples
    CHECK (mid.getSamplesUntilNextNote() == doctest::Approx (25.0));
}
