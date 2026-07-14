/*
  ------------------------------------------------------------------------------
    MidiTools.h

    Note-name parsing and conversion (incl. French names), the Scale and Chord
    classes used by the text arpeggiator (TeAr), and a Euclidean rhythm
    generator. Moved here from the CppMusicTools repository; the original
    `MidiTools` namespace is kept, nested inside `fxme`.

    Depends on juce_core only.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_core/juce_core.h>
#include <map>

namespace fxme
{
namespace MidiTools
{
    /**
        Returns a map of note names (C, C#, Db, etc.) to their semitone offset from C.
        This is defined as a static function to ensure it's initialized only once.
    */
    inline const std::map<juce::String, int>& getNoteNameOffsetMap()
    {
        static const std::map<juce::String, int> noteOffsets = {
            {"c", 0}, {"b#", 0},
            {"c#", 1}, {"db", 1},
            {"d", 2},
            {"d#", 3}, {"eb", 3},
            {"e", 4}, {"fb", 4},
            {"f", 5}, {"e#", 5},
            {"f#", 6}, {"gb", 6},
            {"g", 7},
            {"g#", 8}, {"ab", 8},
            {"a", 9},
            {"a#", 10}, {"bb", 10},
            {"b", 11}, {"cb", 11}
        };
        return noteOffsets;
    }

    /**
        Represents a musical scale, defined by a root note and a type.
        The class stores the 7 notes of the scale as semitone values (0-11).
    */
    class Scale
    {
    public:
        /** The available scale types. */
        enum class Type
        {
            // Major Scale Modes
            Major,
            Dorian,
            Phrygian,
            Lydian,
            Mixolydian,
            Aeolian,
            Locrian,
            // Melodic Minor Modes
            MelodicMinor,
            Dorianb9,
            LydianSharp5,
            Lydianb7, // Previously Bartok
            Mixolydianb13,
            LocrianNatural9,
            Altered,
            // Harmonic Minor Modes
            HarmonicMinor,
            LocrianNatural6,
            IonianSharp5,
            DorianSharp4,
            PhrygianDominant,
            LydianSharp2,
            Altered_bb7,
            // Other 7-note scales
            HarmonicMajor,
            DoubleHarmonicMajor,
            HungarianMinor,
            NeapolitanMajor,
            NeapolitanMinor,
            // Non-diatonic scales
            MajorPentatonic,
            MinorPentatonic,
            Blues,
            WholeTone,
            OctatonicHalfWhole, // Diminished (Half-Whole)
            OctatonicWholeHalf,
        };

        /**
            Constructs a scale from a root note name and a scale type.
            @param rootNoteName The name of the root note (e.g., "C", "F#", "Bb").
            @param scaleType    The type of scale to generate.
        */
        Scale(const juce::String& rootNoteName, Type scaleType)
        {
            auto& noteMap = getNoteNameOffsetMap();
            auto cleanedName = rootNoteName.trim().toLowerCase();
            int rootSemitone = 0;

            if (noteMap.count(cleanedName))
                rootSemitone = noteMap.at(cleanedName);

            buildScale(rootSemitone, scaleType);
        }

        /**
            Constructs a scale from a MIDI note number and a scale type.
            @param rootNoteNumber The MIDI note number of the root. The octave is ignored.
            @param scaleType      The type of scale to generate.
        */
        Scale(int rootNoteNumber, Type scaleType)
        {
            buildScale(rootNoteNumber % 12, scaleType);
        }

        /** Returns a sorted array of 7 semitones (0-11) representing the notes in the scale. */
        const juce::Array<int>& getNotes() const
        {
            return notes;
        }

        /**
            In-place reset: rebuilds the scale without freeing the underlying storage of `notes`.
            Safe to call from a real-time thread once the scale has been pre-warmed (i.e. its
            internal array has been grown to the largest capacity it will need).
        */
        void reset(int rootNoteNumber, Type scaleType)
        {
            notes.clearQuick();
            buildScale(rootNoteNumber % 12, scaleType);
        }

        int getRootNote() const { return rootNote; }
        Type getType() const { return type; }

        /** Returns an ordered list of names for all available scale types. */

        /** Returns an ordered list of names for all available scale types. */
        static const juce::StringArray& getScaleTypeNames()
        {
            static const juce::StringArray names = {
                // Major Scale Modes
                "Major (Ionian)",
                "Dorian",
                "Phrygian",
                "Lydian",
                "Mixolydian",
                "Aeolian",
                "Locrian",
                // Melodic Minor Modes
                "Melodic Minor",
                "Dorian b9",
                "Lydian #5",
                "Lydian b7 (Bartok)",
                "Mixolydian b13",
                "Locrian Natural 9",
                "Altered",
                // Harmonic Minor Modes
                "Harmonic Minor",
                "Locrian Natural 6",
                "Ionian #5",
                "Dorian #4",
                "Phrygian Dominant",
                "Lydian #2",
                "Altered bb7 (Ultralocrian)",
                // Other 7-note scales
                "Harmonic Major",
                "Double Harmonic Major",
                "Hungarian Minor",
                "Neapolitan Major",
                "Neapolitan Minor",
                // Non-diatonic scales
                "Major Pentatonic",
                "Minor Pentatonic",
                "Blues",
                "Whole Tone",
                "Octatonic (Half-Whole)",
                "Octatonic (Whole-Half)",
            };
            return names;
        }

    private:
        void buildScale(int rootSemitone, Type scaleType)
        {
            this->rootNote = rootSemitone;
            this->type = scaleType;

            static const std::map<Type, juce::Array<int>> scaleIntervals = {
                {Type::Major,           {0, 2, 4, 5, 7, 9, 11}}, // Ionian
                {Type::Dorian,          {0, 2, 3, 5, 7, 9, 10}},
                {Type::Phrygian,        {0, 1, 3, 5, 7, 8, 10}},
                {Type::Lydian,          {0, 2, 4, 6, 7, 9, 11}},
                {Type::Mixolydian,      {0, 2, 4, 5, 7, 9, 10}},
                {Type::Aeolian,         {0, 2, 3, 5, 7, 8, 10}},
                {Type::Locrian,         {0, 1, 3, 5, 6, 8, 10}},
                {Type::MelodicMinor,    {0, 2, 3, 5, 7, 9, 11}},
                {Type::Dorianb9,        {0, 1, 3, 5, 7, 9, 10}},
                {Type::LydianSharp5,    {0, 2, 4, 6, 8, 9, 11}},
                {Type::Lydianb7,        {0, 2, 4, 6, 7, 9, 10}}, // Bartok / Lydian Dominant
                {Type::Mixolydianb13,   {0, 2, 4, 5, 7, 8, 10}},
                {Type::LocrianNatural9, {0, 2, 3, 5, 6, 8, 10}},
                {Type::Altered,         {0, 1, 3, 4, 6, 8, 10}},
                {Type::HarmonicMinor,   {0, 2, 3, 5, 7, 8, 11}},
                {Type::LocrianNatural6, {0, 1, 3, 5, 6, 9, 10}},
                {Type::IonianSharp5,    {0, 2, 4, 5, 8, 9, 11}},
                {Type::DorianSharp4,    {0, 2, 3, 6, 7, 9, 10}},
                {Type::PhrygianDominant,{0, 1, 4, 5, 7, 8, 10}},
                {Type::LydianSharp2,    {0, 3, 4, 6, 7, 9, 11}},
                {Type::Altered_bb7,     {0, 1, 3, 4, 6, 8, 9}},
                // Other 7-note scales
                {Type::HarmonicMajor,       {0, 2, 4, 5, 7, 8, 11}},
                {Type::DoubleHarmonicMajor, {0, 1, 4, 5, 7, 8, 11}},
                {Type::HungarianMinor,      {0, 2, 3, 6, 7, 8, 11}},
                {Type::NeapolitanMajor,     {0, 1, 4, 5, 7, 9, 11}},
                {Type::NeapolitanMinor,     {0, 1, 3, 5, 7, 8, 11}},
                // Non-diatonic scales
                {Type::MajorPentatonic,     {0, 2, 4, 7, 9}}, // 5 notes
                {Type::MinorPentatonic,     {0, 3, 5, 7, 10}}, // 5 notes
                {Type::Blues,               {0, 3, 5, 6, 7, 10}}, // 6 notes
                {Type::WholeTone,           {0, 2, 4, 6, 8, 10}}, // 6 notes
                {Type::OctatonicHalfWhole,  {0, 1, 3, 4, 6, 7, 9, 10}}, // 8 notes
                {Type::OctatonicWholeHalf,  {0, 2, 3, 5, 6, 8, 9, 11}}, // 8 notes
            };

            const auto& intervals = scaleIntervals.at(scaleType);
            for (int interval : intervals)
                // notes.add((rootSemitone + interval) % 12);
                notes.add((rootSemitone + interval) % 12);
        }

        juce::Array<int> notes; // Stores the 7 semitones of the scale (0-11).
        int rootNote = 0;
        Type type = Type::Major;
    };

    /**
        Represents a musical chord, with properties like its name and the semitones it contains.
    */
    class Chord
    {
    public:
        /**
            Constructs a Chord object from a chord name string.
            The constructor parses the name to determine the root note and chord quality,
            then populates the set of semitones that define the chord.
            @param chordName The name of the chord, e.g., "C", "Am", "G7", "F#M7".
        */
        Chord(const juce::String& chordName) : name(chordName)
        {
            // Initialize all 7 degrees to -1 (absent)
            // [0] = fundamental, [1] = 3rd, [2] = 5th, [3] = 7th, [4] = 9th, [5] = 11th, [6] = 13th
            degrees.insertMultiple(0, -1, 7);

            juce::String input = name.trim();
            if (input.isEmpty())
                return;

            juce::String rootNoteStr;
            auto& noteMap = getNoteNameOffsetMap();
            int root = -1;

            // --- 1. Parse the chord string to find the root and quality ---
            if (input.endsWith("M7"))
            {
                rootNoteStr = input.dropLastCharacters(2).toLowerCase();
                if (noteMap.find(rootNoteStr) == noteMap.end()) return;
                root = noteMap.at(rootNoteStr);
                degrees.set(0, root);                  // Root
                degrees.set(1, (root + 4) % 12);       // Major Third
                degrees.set(2, (root + 7) % 12);       // Perfect Fifth
                degrees.set(3, (root + 11) % 12);      // Major Seventh
            }
            else if (input.endsWith("m7"))
            {
                rootNoteStr = input.dropLastCharacters(2).toLowerCase();
                if (noteMap.find(rootNoteStr) == noteMap.end()) return;
                root = noteMap.at(rootNoteStr);
                degrees.set(0, root);                  // Root
                degrees.set(1, (root + 3) % 12);       // Minor Third
                degrees.set(2, (root + 7) % 12);       // Perfect Fifth
                degrees.set(3, (root + 10) % 12);      // Minor Seventh
            }
            else if (input.endsWith("7"))
            {
                rootNoteStr = input.dropLastCharacters(1).toLowerCase();
                if (noteMap.find(rootNoteStr) == noteMap.end()) return;
                root = noteMap.at(rootNoteStr);
                degrees.set(0, root);                  // Root
                degrees.set(1, (root + 4) % 12);       // Major Third
                degrees.set(2, (root + 7) % 12);       // Perfect Fifth
                degrees.set(3, (root + 10) % 12);      // Minor Seventh
            }
            else if (input.endsWith("5"))
            {
                rootNoteStr = input.dropLastCharacters(1).toLowerCase();
                if (noteMap.find(rootNoteStr) == noteMap.end()) return;
                root = noteMap.at(rootNoteStr);
                degrees.set(0, root);                  // Root
                degrees.set(2, (root + 7) % 12);       // Perfect Fifth
            }
            else if (input.endsWith("m"))
            {
                rootNoteStr = input.dropLastCharacters(1).toLowerCase();
                if (noteMap.find(rootNoteStr) == noteMap.end()) return;
                root = noteMap.at(rootNoteStr);
                degrees.set(0, root);                  // Root
                degrees.set(1, (root + 3) % 12);       // Minor Third
                degrees.set(2, (root + 7) % 12);       // Perfect Fifth
            }
            else if (input.endsWith("M"))
            {
                rootNoteStr = input.dropLastCharacters(1).toLowerCase();
                if (noteMap.find(rootNoteStr) == noteMap.end()) return;
                root = noteMap.at(rootNoteStr);
                degrees.set(0, root);                  // Root
                degrees.set(1, (root + 4) % 12);       // Major Third
                degrees.set(2, (root + 7) % 12);       // Perfect Fifth
            }
            else // Assume single note
            {
                rootNoteStr = input.toLowerCase();
                if (noteMap.find(rootNoteStr) == noteMap.end()) return;
                root = noteMap.at(rootNoteStr);
                degrees.set(0, root); // Only the root note
            }
        }

        /** Returns an ordered array of 7 semitones representing the chord's degrees.
            The order is: fundamental, 3rd, 5th, 7th, 9th, 11th, 13th.
            An absent degree is represented by -1.
        */
        const juce::Array<int>& getDegrees() const { return degrees; }

        /** Returns the original name of the chord. */
        const juce::String& getName() const { return name; }

        /** Gets the semitone value of a specific degree of the chord.
         *  This is primarily for named chords (e.g. "CM7") where degrees have musical meaning.
         *  For chords set by raw notes, this will reflect the semitone of the Nth note in the sorted array.
         *
            @param degreeIndex The index of the degree (0=fundamental, 1=third, etc.).
            @return The semitone value (0-11), or -1 if the degree is absent or the index is invalid.
        */
        int getDegree(int degreeIndex) const
        {
            if (juce::isPositiveAndBelow(degreeIndex, degrees.size()))
                return degrees[degreeIndex];
            return -1;
        }

        /**
            Sets the chord's degrees directly from an array of semitones.
            This is for the "Notes Played" mode. It converts MIDI notes to unique, octave-less
            semitones and assigns them to the first available degree slots.
            The degrees are assigned in the order they appear in the input array.
            @param notes An array of MIDI note numbers.
        */
        void setDegreesByArray(const juce::Array<int>& notes)
        {
            name = "Custom";
            degrees.clear();
            degrees.insertMultiple(0, -1, 7); // Reset to 7 absent degrees

            if (notes.isEmpty())
                return;

            juce::Array<int> sortedNotes = notes;
            sortedNotes.sort();

            int lowestNote = sortedNotes.getFirst() % 12;

            juce::SortedSet<int> relativeSemitones;
            for (int note : sortedNotes)
            {
                if (lowestNote > note % 12)
                    relativeSemitones.add(note % 12 + 12);
                else
                    relativeSemitones.add(note % 12);
            }

            for (int i = 0; i < juce::jmin(7, relativeSemitones.size()); ++i)
                degrees.set(i, relativeSemitones.getUnchecked(i));
        }

        /**
            Sets the chord directly from an array of raw MIDI note numbers.
            This is for the "Chord Played As Is" mode. It stores the exact notes,
            preserving octave and allowing for complex, non-standard chords.
            @param notes An array of MIDI note numbers.
        */
        void setNotesByArray(const juce::Array<int>& notes)
        {
            name = "Custom";
            rawNotes = notes;
            rawNotes.sort(); // Keep a consistent order
        }

        /** Returns the raw MIDI notes that were set via setNotesByArray. */
        const juce::Array<int>& getRawNotes() const
        {
            return rawNotes;
        }

        /**
            Creates a new Chord by building a diatonic 7-note chord from a given scale and root degree.
            This is primarily used for the "Single Note" mode.
            @param scale The scale to pick notes from.
            @param degree The root degree within the scale (0-6) to build the chord on.
            @param chordMode If true, builds a traditional chord by stacking thirds from the scale.
                           If false (default), the chord's "degrees" will be populated with the
                           notes of the scale in order, starting from the specified degree.
            @return A new Chord object containing the diatonic notes.
        */
        static Chord fromScaleAndDegree(const Scale& scale, int degree, bool chordMode=false)
        {
            Chord newChord("Diatonic");
            const auto& scaleNotes = scale.getNotes();
            if (scaleNotes.isEmpty())
                return newChord; // Return empty chord if scale has no notes

            int scaleSize = scaleNotes.size();
            degree = degree % scaleSize; // Ensure degree is within bounds of the actual scale size

            int fundamental = scaleNotes[(degree + 0) % scaleSize];
            newChord.degrees.set(0, fundamental); // Fundamental

            auto getVoicedNote = [&](int interval) -> int
            {
                int note = scaleNotes[(degree + interval) % scaleSize];
                // If the note is lower than the fundamental, transpose it up an octave
                // to ensure the fundamental is the lowest note in the chord voicing.
                return (note < fundamental) ? note + 12 : note;
            };

            if (chordMode && scaleSize == 7) // Stacking thirds only makes sense for 7-note scales
            {
                // Build the 7-note chord by stacking thirds from the scale
                // The interval indices (2, 4, 6, 1, 3, 5) refer to diatonic steps.
                // We map these to the actual scale size.
                newChord.degrees.set(1, getVoicedNote(2)); // Third (2 steps in the scale)
                newChord.degrees.set(2, getVoicedNote(4)); // Fifth (4 steps in the scale)
                newChord.degrees.set(3, getVoicedNote(6)); // Seventh (6 steps in the scale)
                newChord.degrees.set(4, getVoicedNote(1)); // Ninth (1 step in the scale, but diatonic 9th is 2nd note)
                newChord.degrees.set(5, getVoicedNote(3)); // Eleventh (3 steps in the scale, but diatonic 11th is 4th note)
                newChord.degrees.set(6, getVoicedNote(5)); // Thirteenth (5 steps in the scale, but diatonic 13th is 6th note)
            }
            else
            {
                // The chord's "degrees" will simply be the notes of the scale, voiced above the fundamental.
                newChord.degrees.clear();
                for (int i = 0; i < scaleSize; ++i)
                {
                    int noteInScale = scaleNotes[(degree + i) % scaleSize];
                    int voicedNote = (noteInScale < fundamental) ? noteInScale + 12 : noteInScale;
                    newChord.degrees.add(voicedNote);
                }
            }
            return newChord;
        }

        /**
            In-place reset to an empty chord. Reuses the existing storage of the internal
            arrays (no heap free), so this is safe to call from a real-time thread once
            the chord has been pre-warmed.
        */
        void reset()
        {
            static const juce::String emptyName;
            name = emptyName;
            degrees.clearQuick();
            rawNotes.clearQuick();
            for (int i = 0; i < 7; ++i)
                degrees.add(-1); // 7 absent degrees
        }

        /**
            In-place equivalent of `fromScaleAndDegree`. Rebuilds this chord from the given
            scale and degree without allocating, provided the internal `degrees` array has
            sufficient capacity (pre-warm with an 8-note scale to cover all cases).
        */
        void setFromScaleAndDegree(const Scale& scale, int degree, bool chordMode = false)
        {
            static const juce::String diatonicName ("Diatonic");
            name = diatonicName;
            degrees.clearQuick();
            rawNotes.clearQuick();

            const auto& scaleNotes = scale.getNotes();
            if (scaleNotes.isEmpty())
            {
                for (int i = 0; i < 7; ++i)
                    degrees.add(-1);
                return;
            }

            const int scaleSize = scaleNotes.size();
            degree = degree % scaleSize;

            const int fundamental = scaleNotes[(degree + 0) % scaleSize];

            auto getVoicedNote = [&](int interval) -> int
            {
                int note = scaleNotes[(degree + interval) % scaleSize];
                return (note < fundamental) ? note + 12 : note;
            };

            if (chordMode && scaleSize == 7)
            {
                degrees.add(fundamental);
                degrees.add(getVoicedNote(2));
                degrees.add(getVoicedNote(4));
                degrees.add(getVoicedNote(6));
                degrees.add(getVoicedNote(1));
                degrees.add(getVoicedNote(3));
                degrees.add(getVoicedNote(5));
            }
            else
            {
                degrees.add(fundamental);
                for (int i = 1; i < scaleSize; ++i)
                {
                    int noteInScale = scaleNotes[(degree + i) % scaleSize];
                    int voicedNote = (noteInScale < fundamental) ? noteInScale + 12 : noteInScale;
                    degrees.add(voicedNote);
                }
            }
        }

        /** Pre-warms the chord's internal storage so that subsequent in-place updates
            up to `maxDegrees` and `maxRawNotes` elements will not allocate.
        */
        void ensureCapacity(int maxDegrees, int maxRawNotes)
        {
            degrees.ensureStorageAllocated(maxDegrees);
            rawNotes.ensureStorageAllocated(maxRawNotes);
        }

        /** Returns a SortedSet of the present semitones (0-11) in the chord.
            This is useful for checking against a collection of played MIDI notes
            where order and octave do not matter.
        */
        juce::SortedSet<int> getSortedSet() const
        {
            juce::SortedSet<int> presentSemitones;
            for (int degree : degrees)
            {
                if (degree != -1)
                    presentSemitones.add(degree);
            }
            return presentSemitones;
        }

    private:
        juce::String name;
        juce::Array<int> degrees; // Stores 7 degrees: 1, 3, 5, 7, 9, 11, 13. -1 means absent.
        juce::Array<int> rawNotes; // Stores raw MIDI notes for "as is" mode.
    };

    /**
        Returns a map of French note names (Do, Ré b, etc.) to their semitone offset from C.
    */
    inline const std::map<juce::String, int>& getFrenchNoteNameOffsetMap()
    {
        static const std::map<juce::String, int> noteOffsets = {
            {"do", 0},
            {"do#", 1}, {"reb", 1},
            {"re", 2}, {"ré", 2},
            {"re#", 3}, {"ré#", 3}, {"mib", 3},
            {"mi", 4},
            {"fa", 5},
            {"fa#", 6}, {"solb", 6},
            {"sol", 7},
            {"sol#", 8}, {"lab", 8},
            {"la", 9},
            {"la#", 10}, {"sib", 10},
            {"si", 11}
        };
        return noteOffsets;
    }


    /**
        Converts a MIDI note number into its string representation (e.g., 60 -> "C4").
        @param noteNumber The MIDI note number (0-127).
        @return A string representing the note name and octave.
    */
    inline juce::String getNoteName(int noteNumber)
    {
        if (noteNumber < 0 || noteNumber > 127)
            return "Invalid";

        static const juce::String noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

        const int octave = (noteNumber / 12) - 1;
        const juce::String& note = noteNames[noteNumber % 12];

        return note + juce::String(octave);
    }

    /**
        Converts a note name string (e.g., "C#4") into a MIDI note number.
        Handles sharps (#), flats (b), and octave numbers. Case-insensitive.
        @param noteNameWithOctave The note string, e.g., "C4", "Db-1", "f#5".
        @return The MIDI note number (0-127), or -1 if the string is invalid.
    */
    inline int getNoteNumber(const juce::String& noteNameWithOctave)
    {
        juce::String input = noteNameWithOctave.trim().toLowerCase();
        if (input.isEmpty())
            return -1;

        juce::String notePart;
        int octavePartStartIndex = 1;

        if (input.length() > 1 && (input[1] == '#' || input[1] == 'b'))
        {
            notePart = input.substring(0, 2);
            octavePartStartIndex = 2;
        }
        else
        {
            notePart = input.substring(0, 1);
        }

        auto& noteMap = getNoteNameOffsetMap();
        if (noteMap.find(notePart) == noteMap.end())
            return -1; // Note name not found

        int noteOffset = noteMap.at(notePart);

        juce::String octaveString = input.substring(octavePartStartIndex);
        if (octaveString.isEmpty() || !octaveString.containsOnly("-0123456789"))
            return -1; // Invalid or missing octave

        int octave = octaveString.getIntValue();

        int midiNote = (octave + 1) * 12 + noteOffset;

        if (midiNote >= 0 && midiNote <= 127)
            return midiNote;

        return -1; // Result is out of MIDI range
    }

    /**
        Checks if a MIDI note number corresponds to a note name, ignoring the octave.
        @param noteNumber The MIDI note number to check.
        @param noteName   The note name to compare against (e.g., "C", "Db", "F#"). Case-insensitive.
        @return True if the note number's pitch class matches the note name, false otherwise.
    */
    inline bool isNoteEqual(int noteNumber, const juce::String& noteName)
    {
        if (noteNumber < 0 || noteNumber > 127)
            return false;

        juce::String cleanedNoteName = noteName.trim().toLowerCase();
        if (cleanedNoteName.isEmpty())
            return false;

        const int noteNumberSemitone = noteNumber % 12;

        auto& noteMap = getNoteNameOffsetMap();
        if (noteMap.find(cleanedNoteName) == noteMap.end())
            return false; // The provided note name is not valid

        return noteNumberSemitone == noteMap.at(cleanedNoteName);
    }

    /**
        Parses a chord name and returns the MIDI note number of its root, ignoring octave.
        @param chordName The chord name, e.g., "C", "Am", "G7", "F#5".
        @return The semitone of the root note (0-11), or 0 (C) if parsing fails.
    */
    inline int getRootNoteFromChord(const juce::String& chordName)
    {
        juce::String input = chordName.trim();
        if (input.isEmpty())
            return 0;

        juce::String rootNoteStr;
        auto& noteMap = getNoteNameOffsetMap();

        // Check for multi-character suffixes first
        if (input.endsWith("M7") || input.endsWith("m7"))
        {
            rootNoteStr = input.dropLastCharacters(2).toLowerCase();
        }
        else if (input.endsWith("7") || input.endsWith("5") || input.endsWith("m") || input.endsWith("M"))
        {
            rootNoteStr = input.dropLastCharacters(1).toLowerCase();
        }
        else // Assume single note
        {
            rootNoteStr = input.toLowerCase();
        }

        // Handle sharp/flat in root note
        if (rootNoteStr.length() > 1 && (rootNoteStr.endsWith("#") || rootNoteStr.endsWith("b")))
        {
            // Already have the full root note string
        }
        else if (rootNoteStr.length() > 1) // e.g. "solb"
        {
             // keep as is for french notation
        }
        else {
            // single character root note
        }

        if (noteMap.count(rootNoteStr))
            return noteMap.at(rootNoteStr);

        return 0; // Default to C if parsing fails
    }

    /**
        Checks if a collection of MIDI notes forms a specific major or minor chord,
        regardless of octave or inversion.
        @param heldNotes          A collection of MIDI note numbers currently being played.
        @param chordName          The chord to check for, e.g., "CM", "F#m", "Ebm".
                                  Case-insensitive. 'M' or no suffix for major, 'm' for minor.
        @return True if the notes form the specified chord, false otherwise.
    */
    template <typename Collection>
    inline bool isChordEqual(const Collection& heldNotes, const juce::String& chordName)
    {
        if (chordName.trim().isEmpty() || heldNotes.isEmpty())
            return false;
    
        // 1. Create a Chord object to get the target semitones.
        Chord targetChord(chordName);
        juce::SortedSet<int> targetSemitones = targetChord.getSortedSet();
    
        // If the chord name was invalid, the set of target notes will be empty.
        if (targetSemitones.isEmpty())
            return false;
    
        // 2. Build a set of the currently played semitones.
        juce::SortedSet<int> playedSemitones;
        for (const auto& noteNumber : heldNotes)
        {
            playedSemitones.add(noteNumber % 12);
        }
    
        // --- 3. Compare the sets ---
        return playedSemitones == targetSemitones;
    }

    /**
        Returns a random major or minor chord name string.
        Uses the same nomenclature as isChordEqual (e.g., "C#M", "Am").
        @return A string representing a random chord.
    */
    inline juce::String getRandomChordName()
    {
        static const juce::String rootNotes[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        
        juce::Random& random = juce::Random::getSystemRandom();
        
        // 1. Pick a random root note
        const juce::String& root = rootNotes[random.nextInt(12)];
        
        // 2. Pick a random quality (major or minor)
        const bool isMinor = random.nextBool();
        
        return root + (isMinor ? "m" : "M");
    }

    /**
        Returns a random single note name string (e.g., "C", "F#", "Bb").
        Note that "Bb" will be represented as "A#".
        @return A string representing a random note name.
    */
    inline juce::String getRandomSingleNoteName()
    {
        static const juce::String noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

        juce::Random& random = juce::Random::getSystemRandom();

        return noteNames[random.nextInt(12)];
    }

    /**
        Returns a random fifth interval name string (e.g., "C5", "F#5").
        @return A string representing a random fifth interval.
    */
    inline juce::String getRandomFifthInterval()
    {
        static const juce::String noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

        juce::Random& random = juce::Random::getSystemRandom();

        const juce::String& root = noteNames[random.nextInt(12)];
        return root + "5";
    }

    /**
        Returns a random 7th chord name string (e.g., "CM7", "Am7", "G7").
        @return A string representing a random 7th chord.
    */
    inline juce::String getRandomSeventhChord()
    {
        static const juce::String rootNotes[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        static const juce::String chordTypes[] = { "M7", "m7", "7" };

        juce::Random& random = juce::Random::getSystemRandom();

        // 1. Pick a random root note
        const juce::String& root = rootNotes[random.nextInt(12)];

        // 2. Pick a random 7th quality
        const juce::String& type = chordTypes[random.nextInt(3)];

        return root + type;
    }

    /**
        Converts a standard international note name (e.g., "C#") to its French equivalent (e.g., "Do#").
        @param standardNoteName The standard note name (C, C#, Db, etc.).
        @return The corresponding French note name as a string. Returns an empty string if not found.
    */
    inline juce::String getFrenchNoteName(const juce::String& standardNoteName)
    {
        static const juce::String frenchNoteNames[] = { "Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" };

        auto& noteMap = getNoteNameOffsetMap();
        auto cleanedName = standardNoteName.trim().toLowerCase();

        if (noteMap.count(cleanedName))
        {
            int semitone = noteMap.at(cleanedName);
            return frenchNoteNames[semitone];
        }
        return {}; // Return empty string if not found
    }

    /**
        Converts a standard international chord name (e.g., "C", "Am", "G5") to its French equivalent.
        @param standardChordName The standard chord name.
        @return The corresponding French chord name as a string. Returns the original name if it can't be parsed.
    */
    inline juce::String getFrenchChordName(const juce::String& standardChordName)
    {
        juce::String input = standardChordName.trim();
        if (input.isEmpty())
            return {};

        juce::String rootNoteStr;
        juce::String suffix;

        // Check for longer suffixes first
        if (input.endsWith("M7"))
        {
            rootNoteStr = input.dropLastCharacters(2);
            suffix = "M7"; // e.g., "DoM7"
        }
        else if (input.endsWith("m7"))
        {
            rootNoteStr = input.dropLastCharacters(2);
            suffix = "m7"; // e.g., "Lam7"
        }
        else if (input.endsWith("7"))
        {
            rootNoteStr = input.dropLastCharacters(1);
            suffix = "7"; // e.g., "Sol7"
        }
        else if (input.endsWith("5"))
        {
            rootNoteStr = input.dropLastCharacters(1);
            suffix = "5"; // e.g., "Sol5"
        }
        else if (input.endsWith("m"))
        {
            rootNoteStr = input.dropLastCharacters(1);
            suffix = "m";
        }
        else if (input.endsWith("M"))
        {
            rootNoteStr = input.dropLastCharacters(1);
            suffix = "M";
        }
        else // Handles single notes ("C")
        {
            rootNoteStr = input;
            suffix = ""; // This case now only handles single notes.
        }

        juce::String frenchRoot = getFrenchNoteName(rootNoteStr);
        return frenchRoot.isNotEmpty() ? frenchRoot + suffix : standardChordName;
    }

    /**
        Generates a Euclidean rhythm using a Bresenham-based algorithm.
        This distributes 'hits' pulses as evenly as possible over 'steps'.
        @param hits The number of active steps (pulses).
        @param steps The total number of steps.
        @return A juce::Array<bool> where true represents a hit and false a rest.
    */
    inline juce::Array<bool> euclidianRythm(int hits, int steps, int rotation = 0)
    {
        juce::Array<bool> pattern;
        if (steps <= 0) return pattern;
        
        hits = juce::jlimit(0, steps, hits);

        for (int i = 0; i < steps; ++i)
        {
            int index = (i - rotation) % steps;
            if (index < 0) index += steps;
            pattern.add(((index * hits) % steps) < hits);
        }

        return pattern;
    }
} // namespace MidiTools
} // namespace fxme
