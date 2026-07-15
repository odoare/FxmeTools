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
TEST_CASE("'v8' sets local note velocity to 127")
{
    Arpeggiator arp;
    setup(arp, "v8 1 1");

    auto e0 = tick(arp, true);
    auto e1 = tick(arp);

    CHECK(e0.pitch    == C4);
    CHECK(e0.velocity == 127);
    CHECK(e1.pitch    == C4);
    CHECK(e1.velocity == DEFAULT_VEL);  // local modifier expired
}

TEST_CASE("'V4' sets global velocity to 64 for subsequent notes")
{
    Arpeggiator arp;
    setup(arp, "V4 1 1 1");

    auto e0 = tick(arp, true);  // 'V4' consumed as prefix
    auto e1 = tick(arp);
    auto e2 = tick(arp);

    CHECK(e0.velocity == 64);
    CHECK(e1.velocity == 64);
    CHECK(e2.velocity == 64);
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
