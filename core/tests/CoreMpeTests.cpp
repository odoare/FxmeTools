#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <FxmeTools/midi/PassNotes.h>

#include <array>
#include <cmath>
#include <set>
#include <vector>

using namespace fxme;

namespace
{
    /** A host's output: everything written, in order. */
    struct Out
    {
        struct E { char kind; double time; int channel, a, b; };
        std::vector<E> all;
        void noteOn (double t, int c, int n, int v) { all.push_back ({ 'n', t, c, n, v }); }
        void noteOff (double t, int c, int n) { all.push_back ({ 'f', t, c, n, 0 }); }
        void pitchBend (double t, int c, int v) { all.push_back ({ 'b', t, c, v, 0 }); }
        void controller (double t, int c, int n, int v) { all.push_back ({ 'c', t, c, n, v }); }
        int count (char kind) const
        {
            int k = 0;
            for (const auto& e : all)
                k += e.kind == kind ? 1 : 0;
            return k;
        }
        const E* last (char kind) const
        {
            for (auto it = all.rbegin(); it != all.rend(); ++it)
                if (it->kind == kind)
                    return &*it;
            return nullptr;
        }
    };

    ArrayView<float> view (const std::vector<float>& v) { return { v.data(), static_cast<int> (v.size()) }; }

    pattern::Pass realized (const std::string& text, pattern::Program& program)
    {
        pattern::Options o;
        o.syntax = 3;
        o.defaultStepBeats = 0.25;
        program = pattern::compile (text, o);
        REQUIRE (program.ok());
        pattern::Pass pass;
        pass.reserve (program.info());
        program.realize ({}, pass);
        return pass;
    }
}

TEST_CASE ("MpeOut: the zone once, each note on its own channel with its exact pitch; off: one channel, rounded")
{
    MpeOut mpe;
    Out out;
    const auto a = mpe.noteOn (out, 0.0, 60.5f, 100);
    CHECK (out.count ('c') == 6 * 16);
    CHECK (a.channel >= 1);
    CHECK (a.note == 61);
    CHECK (out.last ('b')->a == -85);    // -0.5 over ±48
    const auto b = mpe.noteOn (out, 0.0, 64.0f, 100);
    CHECK (out.count ('c') == 6 * 16);   // announced once
    CHECK (b.channel != a.channel);

    MpeOut plain;
    plain.setMpe (false);
    plain.setChannel (4);
    Out o2;
    CHECK (plain.noteOn (o2, 0.0, 60.4f, 100) == MpeOut::Note { 4, 60 });
    CHECK (o2.count ('b') == 0);
    CHECK_FALSE (plain.noteOn (o2, 0.0, 128.0f, 100));
}

TEST_CASE ("MpeIn: what MpeOut sends comes back as the exact pitch; the master moves every channel")
{
    MpeOut mpe;
    Out out;
    const auto n = mpe.noteOn (out, 0.0, 60.25f, 100);
    MpeIn in;
    for (const auto& e : out.all)
    {
        if (e.kind == 'b') in.readPitchBend (e.channel, e.a);
        if (e.kind == 'c') in.readController (e.channel, e.a, e.b);
    }
    CHECK (static_cast<float> (n.note) + in.bend (n.channel) == doctest::Approx (60.25).epsilon (0.0001));
    in.readPitchBend (0, 4096);          // channel 1: ±2 by default
    CHECK (in.own (0) == doctest::Approx (1.0));
    CHECK (in.bend (n.channel) == doctest::Approx (1.25).epsilon (0.001));
    CHECK (in.own (n.channel) == doctest::Approx (0.25).epsilon (0.01));
}

TEST_CASE ("HeldNotes: same note, ends in the order they started, ties")
{
    HeldNotes h;
    MpeOut mpe;
    mpe.setMpe (false);
    Out out;
    CHECK (h.start (out, mpe, 0.0, 1.0, 60.0f, 100));
    CHECK (h.start (out, mpe, 0.0, 1.0, 64.0f, 100));
    CHECK_FALSE (h.start (out, mpe, 0.5, 2.0, 60.0f, 100, HeldNotes::SameNote::Keep));   // it rings on
    CHECK (h.sounding() == 2);
    CHECK (out.count ('f') == 0);
    CHECK (h.nextEnd() == doctest::Approx (1.0));
    h.extend (1.0, 0.5);
    CHECK (h.nextEnd() == doctest::Approx (1.5));
    CHECK (h.releaseDue (out, mpe, 1.0) == 0);
    CHECK (h.releaseDue (out, mpe, 1.5) == 2);
    REQUIRE (out.count ('f') == 2);
    CHECK (out.all[out.all.size() - 2].a == 60);
    CHECK (out.all.back().a == 64);
    CHECK (std::isinf (h.nextEnd()));
}

TEST_CASE ("HeldNotes: a glide moves the bend, the target is not attacked; without MPE the notes overlap")
{
    {
        HeldNotes h;
        MpeOut mpe;
        Out out;
        h.start (out, mpe, 0.0, 0.25, 60.0f, 100);
        const std::vector<float> to { 64.0f };
        h.link (0.0, HeldNotes::Link::Glide, 0.0, 0.25, view (to));
        h.releaseUpTo (out, mpe, 0.25, true, 0.0);    // the target's date: not released
        CHECK (h.sounding() == 1);
        const int before = out.count ('b');
        h.glide (out, mpe, 0.0, 0.25);
        CHECK (out.count ('b') - before == 16);       // every 1/64 beat
        CHECK (h.arrive (out, mpe, 0.25, 0.5, view (to)) == 0u);
        CHECK (out.count ('n') == 1);
        CHECK (out.last ('b')->a == MpeOut::bendValue (4.0f));
        h.releaseUpTo (out, mpe, 0.5, true, 0.25);
        CHECK (out.count ('f') == 1);
    }
    {
        HeldNotes h;
        MpeOut mpe;
        mpe.setMpe (false);
        Out out;
        h.start (out, mpe, 0.0, 0.25, 60.0f, 100);
        const std::vector<float> to { 62.0f };
        h.link (0.0, HeldNotes::Link::Glide, 0.0, 0.25, view (to));
        h.glide (out, mpe, 0.0, 0.25);
        CHECK (out.count ('b') == 0);
        out.all.clear();
        h.arrive (out, mpe, 0.25, 0.5, view (to));
        REQUIRE (out.all.size() == 2);
        CHECK (out.all[0].kind == 'n');               // the next note first,
        CHECK (out.all[0].a == 62);
        CHECK (out.all[1].kind == 'f');               // then the end of this one
        CHECK (out.all[1].a == 60);
    }
}

TEST_CASE ("HeldNotes: chords, voice by voice; a voice left alone ends or starts")
{
    HeldNotes h;
    MpeOut mpe;
    Out out;
    for (float p : { 60.0f, 64.0f, 67.0f })
        h.start (out, mpe, 0.0, 0.5, p, 100);
    const std::vector<float> to { 69.0f, 62.0f, 65.0f };   // any order: paired by pitch
    h.link (0.0, HeldNotes::Link::Glide, 0.25, 0.5, view (to));
    CHECK (h.arrive (out, mpe, 0.5, 1.0, view (to)) == 0u);
    CHECK (out.count ('n') == 3);
    CHECK (h.sounding() == 3);

    HeldNotes g;
    Out o2;
    g.start (o2, mpe, 0.0, 0.5, 60.0f, 100);
    g.start (o2, mpe, 0.0, 0.5, 67.0f, 100);
    const std::vector<float> one { 66.0f };
    g.link (0.0, HeldNotes::Link::Legato, 0.0, 0.5, view (one));
    g.releaseUpTo (o2, mpe, 0.5, true, 0.5);               // 60 had no partner: it ends as written
    CHECK (g.arrive (o2, mpe, 0.5, 1.0, view (one)) == 0u);
    REQUIRE (o2.count ('f') == 1);
    CHECK (o2.last ('f')->a == 60);
    const std::vector<float> two { 65.0f, 70.0f };
    g.link (0.5, HeldNotes::Link::Legato, 0.5, 1.0, view (two));
    CHECK (g.arrive (o2, mpe, 1.0, 1.5, view (two)) == 0b10u);   // 70 continues nothing: the caller starts it
}

TEST_CASE ("playEvent: a pass with links plays one attack, and its end")
{
    pattern::Program program;
    const auto pass = realized ("1~5 ^3 . 1", program);
    auto pitchOf = [] (const pattern::Note& n, double) { return static_cast<float> (60 + n.degree); };
    HeldNotes h;
    MpeOut mpe;
    Out out;
    for (const auto& e : pass.events)
    {
        h.releaseUpTo (out, mpe, e.onset, true, e.onset);
        const auto played = pattern::playEvent (h, mpe, out, pass, e, e.onset, e.onset, e.onset + e.length, 100, -1.0f, pitchOf);
        h.glide (out, mpe, e.onset, e.onset + 0.25);
        if (&e == &pass.events[1])
            CHECK (played.started == 0);                  // arrived, not attacked
    }
    CHECK (out.count ('n') == 2);                         // 1~5^3 is one note; the last 1 another
    CHECK (out.count ('f') == 1);
    h.silence (out, mpe, 2.0);
    CHECK (out.count ('f') == 2);
}

TEST_CASE ("playEvent: a hit plays the host's note, or nothing")
{
    pattern::Program program;
    const auto pass = realized ("x", program);
    auto pitchOf = [] (const pattern::Note&, double) { return -1.0f; };
    HeldNotes h;
    MpeOut mpe;
    mpe.setMpe (false);
    Out out;
    CHECK (pattern::playEvent (h, mpe, out, pass, pass.events[0], 0.0, 0.0, 0.25, 100, -1.0f, pitchOf).started == 0);
    const auto played = pattern::playEvent (h, mpe, out, pass, pass.events[0], 0.0, 0.0, 0.25, 100, 36.0f, pitchOf);
    CHECK (played.started == 1);
    CHECK (played.lastPitch == doctest::Approx (36.0));
}
