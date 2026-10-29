/*
  Golden recording of fxme::Arpeggiator's output, for the engine that played
  patterns by walking their text (before it became a player of the pattern
  language's passes). Shared by the test and by the tool that produced
  ArpeggiatorGolden.h from the old engine.

  Every case is deterministic (no `?`, no probability but p0), so the stream
  does not depend on the random generator.
*/

#pragma once

#include <FxmeTools/midi/MidiTools.h>

#include <string>
#include <vector>

namespace arpgolden
{

struct Case
{
    const char* pattern;
    const char* chord;          ///< "CM", "CM7", "major" (a scale set directly), "raw" (60 64 67 71 as is)
    int method;                 ///< chord method 0, 1, 2
    bool octaveWrap;
    double syncPpq;             ///< < 0: no sync; else the host position at sample 0, synced at every block
    int playedVelocity;         ///< 0: leave the default
};

inline const Case* inputs (int& count)
{
    static const Case cases[] = {
        // The syntax-2 equivalence set of the compiler's tests: single-note mode with a root scale
        { "1 2 3 4 5 6 7", "major", 2, false, -1, 0 },
        { "1 3 5 8", "major", 2, false, -1, 0 },
        { "1 _ 5 .", "major", 2, false, -1, 0 },
        { "1 . _ 3", "major", 2, false, -1, 0 },
        { "1 + + - 5 -", "major", 2, false, -1, 0 },
        { "1 = = 3 =", "major", 2, false, -1, 0 },
        { "0 1 0 3 0", "major", 2, false, -1, 0 },
        { "o+1 2 o-3 4", "major", 2, false, -1, 0 },
        { "O+ 1 2", "major", 2, false, -1, 0 },
        { "1 2 O+", "major", 2, false, -1, 0 },
        { "O+ 1 O- 2", "major", 2, false, -1, 0 },
        { "(O+ 1 2) 3 4", "major", 2, false, -1, 0 },
        { "(O+ 1 (O+ 2 3) 4) 5", "major", 2, false, -1, 0 },
        { "V8 1 2 (VF 3 4) 5", "major", 2, false, -1, 0 },
        { "v9 1 2 vF3 4", "major", 2, false, -1, 0 },
        { "V+ 1 V+ 2 V- 3", "major", 2, false, -1, 0 },
        { "v+ 1 v+ 2 V3 v- 4", "major", 2, false, -1, 0 },
        { "#1 b2 3 #b4", "major", 2, false, -1, 0 },
        { "\"1 2 3\" 4 5", "major", 2, false, -1, 0 },
        { "1 \"2 3 4 5", "major", 2, false, -1, 0 },
        { "\"1 2\" \"3 4\"", "major", 2, false, -1, 0 },
        { "o+ (1 2) 3", "major", 2, false, -1, 0 },
        { "(1 2 o+) 3 4", "major", 2, false, -1, 0 },
        { "vF0 1 0", "major", 2, false, -1, 0 },
        { "o3 1 O2 2 o+ 3", "major", 2, false, -1, 0 },
        { "1 2 3 O+ O+ 4", "major", 2, false, -1, 0 },
        { "8 9 A B C D E F", "major", 2, false, -1, 0 },
        { "(((1 2) 3) 4) 5", "major", 2, false, -1, 0 },
        { "O- 1 O- 2 O- 3 O- 4 O- 5", "major", 2, false, -1, 0 },
        { "O+ O+ O+ 1 2", "major", 2, false, -1, 0 },
        { "_ 1 _ . _ 2", "major", 2, false, -1, 0 },
        { "  1   2  ", "major", 2, false, -1, 0 },
        { "1 w 2 x 3", "major", 2, false, -1, 0 },
        { "1 ) 2", "major", 2, false, -1, 0 },
        { "1 2 o+ #", "major", 2, false, -1, 0 },
        { "V8 1 2 V+ ", "major", 2, false, -1, 0 },
        { "\"O+ 1 2\" 3 \"4\"", "major", 2, false, -1, 0 },

        // The arpeggiator's own tests, in "notes played" mode
        { "1 2 3", "CM", 0, false, -1, 0 },
        { "1 2 3 4", "CM7", 0, false, -1, 0 },
        { "1 . 3", "CM", 0, false, -1, 0 },
        { "1 _ 3", "CM", 0, false, -1, 0 },
        { "2 = =", "CM", 0, false, -1, 0 },
        { "1 + + +", "CM7", 0, false, -1, 0 },
        { "3 -", "CM7", 0, false, -1, 0 },
        { "o+1 1", "CM", 0, false, -1, 0 },
        { "o-1 1", "CM", 0, false, -1, 0 },
        { "#1 1", "CM", 0, false, -1, 0 },
        { "b2 2", "CM", 0, false, -1, 0 },
        { "vF 1 1", "CM", 0, false, -1, 0 },
        { "V8 1 1 1", "CM", 0, false, -1, 0 },
        { "V8 1 V+ 1 V+ 1 V- 1", "CM", 0, false, -1, 0 },
        { "V8 1 v+ 1 1", "CM", 0, false, -1, 0 },
        { "VF 1 V+ 1", "CM", 0, false, -1, 0 },
        { "V1 1 V- 1", "CM", 0, false, -1, 0 },
        { "(o+1 2) 1", "CM", 0, false, -1, 0 },
        { "1 1", "CM", 0, false, -1, 40 },
        { "V4 1 1", "CM", 0, false, -1, 127 },
        { "(V4 1) 1", "CM", 0, false, -1, 127 },
        { "V+ 1 v+ 1 v- 1 V- 1", "CM", 0, false, -1, 40 },
        { "p0 1", "CM", 0, false, -1, 0 },
        { "p0 1:2", "CM", 0, false, -1, 0 },
        { "p0 1:.", "CM", 0, false, -1, 0 },
        { "p0 1:_", "CM", 0, false, -1, 0 },
        { "p0 (12):(34)", "CM7", 0, false, -1, 0 },
        { "p0 (1 2):(3 4)", "CM7", 0, false, -1, 0 },
        { "p0 1:(1 2 3)", "CM7", 0, false, -1, 0 },
        { "1 \"2 3\" 4", "CM", 0, false, -1, 0 },
        { "1 A F", "CM7", 0, false, -1, 0 },
        { "1 b2", "CM", 0, false, -1, 0 },
        { "b1 1", "CM", 0, false, -1, 0 },
        { "1 8 F 9", "major", 0, true, -1, 0 },
        { "1 8 F 9", "major", 0, false, -1, 0 },
        { "7 + +", "major", 0, true, -1, 0 },

        // "Chord played as is": the notes themselves, moved by whole octaves
        { "1 2 3 4", "raw", 1, false, -1, 0 },
        { "1 + + + + + + +", "raw", 1, false, -1, 0 },
        { "1 - - -", "raw", 1, false, -1, 0 },
        { "o+1 2 O+ 3 4 O- 1", "raw", 1, false, -1, 0 },
        { "1 2 3 4 5 6 7 8", "raw", 1, false, -1, 0 },

        // Synced to the host's position
        { "1 2 3 4 1 2 3 4", "CM7", 0, false, 0.0, 0 },
        { "1 2 3 4 1 2 3 4", "CM7", 0, false, 1.0, 0 },
        { "1 2 3 4 1 2 3 4", "CM7", 0, false, 0.25, 0 },
        { "1 2 3 4 1 2 3 4", "CM7", 0, false, 1.125, 0 },
        { "1 2 3 4 1 2 3 4", "CM7", 0, false, 1.5, 0 },
        { "1 2 (O+ 3 4) 1 _ . 2", "CM7", 0, false, 2.0, 0 },
        { "1 2 3", "CM", 0, false, 0.5, 0 },
        { "V8 1 V+ 2 3 4", "CM7", 0, false, 3.0, 0 },
        { "p0 (1 2):(3 4)", "CM7", 0, false, 1.0, 0 },
    };
    count = static_cast<int> (sizeof (cases) / sizeof (cases[0]));
    return cases;
}

/** The stream of `steps` blocks (a first block of one sample, then 100 samples: one step each; 100 samples
    throughout when synced), as text. */
template <class Arp>
std::string record (const Case& c, int steps)
{
    namespace MT = fxme::MidiTools;
    static const std::vector<int> major { 0, 2, 4, 5, 7, 9, 11 };
    static const std::vector<int> minor { 0, 2, 3, 5, 7, 8, 10 };
    static const std::vector<int> rawNotes { 60, 64, 67, 71 };

    Arp arp;
    const std::string name = c.chord;
    MT::Chord chord (name == "major" || name == "raw" ? "" : name);
    if (name == "major") chord.setDegreesDirect (major);
    if (name == "raw") chord.setNotesByArray (rawNotes);
    arp.setChord (chord);
    if (c.method == 2)
    {
        MT::Chord key ("");
        key.setDegreesDirect (minor);
        arp.setRootChord (key);
    }
    arp.setPattern (c.pattern);
    arp.prepareToPlay (1000.0);
    arp.setTempo (150.0);
    arp.setSubdivision (4);
    arp.setChordMethod (c.method);
    arp.setOctaveWrap (c.octaveWrap);
    if (c.playedVelocity > 0)
        arp.setPlayedVelocityFromMidi (c.playedVelocity);

    std::string out;
    int t = 0;
    for (int i = 0; i < steps; ++i)
    {
        const int n = (i == 0 && c.syncPpq < 0.0) ? 1 : 100;   // synced: whole steps, so the host's positions are exact
        if (c.syncPpq >= 0.0)
            arp.syncToPlayHead (c.syncPpq + t / 400.0);
        for (const auto& e : arp.processBlock (n))
            out += std::string (e.noteOn ? "+" : "-") + std::to_string (e.note)
                 + (e.noteOn ? "/" + std::to_string (e.velocity) : std::string()) + "@" + std::to_string (e.sample) + " ";
        out += "#" + std::to_string (arp.getCurrentStepIndex()) + " | ";
        t += n;
    }
    return out;
}

} // namespace arpgolden
