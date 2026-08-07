#include <doctest/doctest.h>
#include <JuceHeader.h>
#include <FxmeTools/midi/Arpeggiator.h>

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
               const juce::String& pattern,
               const juce::String& chordName = "CM")
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
        for (const auto& meta : buf)
        {
            auto msg = meta.getMessage();
            if (msg.isNoteOn())
                return { msg.getNoteNumber(), msg.getVelocity() };
        }
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
    juce::AudioPlayHead::CurrentPositionInfo hostAt(double ppq)
    {
        juce::AudioPlayHead::CurrentPositionInfo p;
        p.bpm         = 150.0;
        p.ppqPosition = ppq;
        p.isPlaying   = true;
        return p;
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

    const juce::StringArray patterns {
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
        const juce::String migrated = A::migratePatternV1toV2("v" + juce::String(oldLevel) + " 1");
        const int newLevel = A::hexValue(migrated[1]);
        const int oldVel   = juce::jmin(127, oldLevel * 16);

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
