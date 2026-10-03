/*
  ------------------------------------------------------------------------------
    tests/presets/main.cpp

    fxme::ModulePresetLibrary and fxme::ModulePresetTarget against a real
    APVTS holding the same fake module ("Comp": Attack, Ratio, Mode, On)
    under two prefixes, "A_Comp_" and "B_Comp_", plus a parameter outside
    both ("Other_Gain"), and a PresetManager on the same state:

      1. save from A, load into B: the values arrive;
      2. a file without some key: that parameter goes to its default;
      3. a file with an unknown key: ignored, the rest loads;
      4. a file of another module: refused, nothing changed;
      5. the current name survives copyState() / replaceState(), and the
         replacement does not leave the target dirty;
      6. dirty set by a change in scope, not by one outside it; cleared by
         a load;
      7. factory presets of two modules (and a global preset) in one
         resource list: each library lists only its own;
      8. a rename follows in every target showing the preset;
      9. the global preset: marked modified by a module load (it changes
         parameters), not by the bookkeeping alone (a module preset name).

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include <JuceHeader.h>
#include <cstdio>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

namespace
{
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

    void addModule (Layout& layout, const juce::String& prefix)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { prefix + "Attack", 1 }, prefix + "Attack",
            juce::NormalisableRange<float> (0.1f, 100.0f), 10.0f));
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { prefix + "Ratio", 1 }, prefix + "Ratio",
            juce::NormalisableRange<float> (1.0f, 20.0f), 4.0f));
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { prefix + "Mode", 1 }, prefix + "Mode",
            juce::StringArray { "Linear", "Opto", "Vintage" }, 0));
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { prefix + "On", 1 }, prefix + "On", true));
    }

    Layout makeLayout()
    {
        Layout layout;
        addModule (layout, "A_Comp_");
        addModule (layout, "B_Comp_");
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "Other_Gain", 1 }, "Other Gain",
            juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f));
        return layout;
    }

    struct TestProcessor : juce::AudioProcessor
    {
        TestProcessor() : apvts (*this, nullptr, "Parameters", makeLayout()) {}

        const juce::String getName() const override                   { return "Test"; }
        void prepareToPlay (double, int) override                      {}
        void releaseResources() override                               {}
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        double getTailLengthSeconds() const override                   { return 0.0; }
        bool acceptsMidi() const override                              { return false; }
        bool producesMidi() const override                             { return false; }
        juce::AudioProcessorEditor* createEditor() override            { return nullptr; }
        bool hasEditor() const override                                { return false; }
        int getNumPrograms() override                                  { return 1; }
        int getCurrentProgram() override                               { return 0; }
        void setCurrentProgram (int) override                          {}
        const juce::String getProgramName (int) override               { return {}; }
        void changeProgramName (int, const juce::String&) override     {}
        void getStateInformation (juce::MemoryBlock&) override         {}
        void setStateInformation (const void*, int) override           {}

        juce::AudioProcessorValueTreeState apvts;
    };

    float value (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    {
        return apvts.getRawParameterValue (id)->load();
    }

    void set (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, float v)
    {
        auto* p = apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    bool near (float a, float b) { return std::abs (a - b) < 1.0e-3f * juce::jmax (1.0f, std::abs (b)); }

    /** Lets the asynchronous parts (change messages, the APVTS flush, the
        delayed dirty clear) happen. */
    void settle() { juce::MessageManager::getInstance()->runDispatchLoopUntil (300); }

    //==========================================================================
    // A fake BinaryData: two module presets of different modules, a global
    // preset, and something that is not XML.
    const char* const compFactory =
        R"(<FxmeModulePreset module="Comp" version="1" name="Factory Squash">
             <Param key="Attack" value="1.0"/><Param key="Ratio" value="12.0"/>
           </FxmeModulePreset>)";
    const char* const tubeFactory =
        R"(<FxmeModulePreset module="Tube" version="1" name="Warm"><Param key="Drive" value="6"/></FxmeModulePreset>)";
    const char* const globalFactory = R"(<Parameters presetName="Global"/>)";
    const char* const image = "\x89PNG not xml";

    const char* const resourceNames[] { "Squash_xml", "Warm_xml", "Global_xml", "logo_png" };

    const char* getResource (const char* name, int& size)
    {
        const juce::String n (name);
        const char* data = n == "Squash_xml" ? compFactory
                         : n == "Warm_xml"   ? tubeFactory
                         : n == "Global_xml" ? globalFactory
                         : n == "logo_png"   ? image : nullptr;
        size = data != nullptr ? (int) std::strlen (data) : 0;
        return data;
    }
}

//==============================================================================
int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    std::printf ("ModulePresetLibrary / ModulePresetTarget\n");

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("FxmePresetTests-" + juce::String::toHexString (juce::Random().nextInt64()));
    const auto globalFolder = folder.getChildFile ("Global");

    {
        TestProcessor proc;
        auto& apvts = proc.apvts;

        fxme::PresetManager global (apvts, globalFolder,
                                    resourceNames, (int) std::size (resourceNames), getResource);
        fxme::ModulePresetLibrary comp ("Comp", 1, folder.getChildFile ("Comp"),
                                        resourceNames, (int) std::size (resourceNames), getResource);
        fxme::ModulePresetLibrary tube ("Tube", 1, folder.getChildFile ("Tube"),
                                        resourceNames, (int) std::size (resourceNames), getResource);
        fxme::ModulePresetTarget a (comp, apvts, "A_Comp_");
        fxme::ModulePresetTarget b (comp, apvts, "B_Comp_");
        settle();

        check (a.getParameters().size() == 4 && b.getParameters().size() == 4,
               "scope: four parameters per prefix, the outside one in neither");

        // ---- 7. factory lists ------------------------------------------------
        check (comp.getFactoryPresets().size() == 1 && comp.getFactoryPresets()[0].name == "Factory Squash",
               "factory: the Comp library lists only its own preset");
        check (tube.getFactoryPresets().size() == 1 && tube.getFactoryPresets()[0].name == "Warm",
               "factory: the Tube library lists only its own preset");
        check (global.getFactoryPresets().size() == 1 && global.getFactoryPresets()[0].name == "Global",
               "factory: the global manager lists only the global preset");

        // ---- 1. save from A, load into B ------------------------------------
        set (apvts, "A_Comp_Attack", 33.0f);
        set (apvts, "A_Comp_Ratio", 7.5f);
        set (apvts, "A_Comp_Mode", 2.0f);
        set (apvts, "A_Comp_On", 0.0f);
        check (a.saveUserPreset ("Glue"), "save from A");
        check (comp.getUserPresets().size() == 1 && comp.getUserPresets()[0].name == "Glue",
               "the library lists the saved preset");
        check (b.loadUserPreset (0), "load into B");
        check (near (value (apvts, "B_Comp_Attack"), 33.0f) && near (value (apvts, "B_Comp_Ratio"), 7.5f)
                   && near (value (apvts, "B_Comp_Mode"), 2.0f) && near (value (apvts, "B_Comp_On"), 0.0f),
               "B has A's values (float, float, choice, bool)");
        check (b.getCurrentPresetName() == "Glue" && ! b.currentPresetIsFactory() && b.getCurrentUserIndex() == 0,
               "B shows the preset as its current one");

        // ---- 2. a missing key goes to its default ----------------------------
        juce::XmlElement partial ("FxmeModulePreset");
        partial.setAttribute ("module", "Comp");
        partial.setAttribute ("version", 1);
        auto* attack = partial.createNewChildElement ("Param");
        attack->setAttribute ("key", "Attack");
        attack->setAttribute ("value", 50.0);
        auto* unknown = partial.createNewChildElement ("Param");    // ---- 3.
        unknown->setAttribute ("key", "Sidechain");
        unknown->setAttribute ("value", 1.0);

        check (b.apply (partial, "Partial"), "a partial file with an unknown key loads");
        check (near (value (apvts, "B_Comp_Attack"), 50.0f), "its key is applied");
        check (near (value (apvts, "B_Comp_Ratio"), 4.0f) && near (value (apvts, "B_Comp_Mode"), 0.0f)
                   && near (value (apvts, "B_Comp_On"), 1.0f),
               "the keys it lacks go to their defaults");

        // ---- 4. another module's file is refused -----------------------------
        juce::XmlElement other ("FxmeModulePreset");
        other.setAttribute ("module", "Tube");
        auto* drive = other.createNewChildElement ("Param");
        drive->setAttribute ("key", "Attack");
        drive->setAttribute ("value", 90.0);
        check (! b.apply (other, "Wrong"), "a Tube preset is refused by a Comp target");
        check (near (value (apvts, "B_Comp_Attack"), 50.0f) && b.getCurrentPresetName() == "Partial",
               "nothing changed");

        juce::XmlElement newer (partial);
        newer.setAttribute ("version", 2);
        check (! b.apply (newer, "Newer"), "a preset of a newer format version is refused");

        // ---- 6. dirty ----------------------------------------------------------
        settle();
        check (! b.isDirty(), "not dirty after a load");
        set (apvts, "Other_Gain", 6.0f);
        set (apvts, "A_Comp_Ratio", 3.0f);
        check (! b.isDirty(), "a change outside the scope (or in another target's) does not make it dirty");
        set (apvts, "B_Comp_Ratio", 9.0f);
        check (b.isDirty(), "a change in the scope does");
        check (b.loadUserPreset (0), "load again");
        settle();
        check (! b.isDirty(), "a load clears it");

        // ---- 5. the current name lives in the state -------------------------
        const auto saved = apvts.copyState();
        b.setCurrentPreset ("Something else", false, false);
        set (apvts, "B_Comp_Attack", 1.0f);
        apvts.replaceState (saved);
        settle();
        check (b.getCurrentPresetName() == "Glue", "the name comes back with the state");
        check (near (value (apvts, "B_Comp_Attack"), 33.0f), "and so do the values");
        check (! b.isDirty(), "a replaced state does not leave the target dirty");

        // ---- 8. rename ----------------------------------------------------------
        check (a.loadUserPreset (0) && a.getCurrentPresetName() == "Glue", "A shows Glue too");
        check (a.renameUserPreset (comp.getUserPresets()[0], "Glue 2"), "rename from A");
        check (a.getCurrentPresetName() == "Glue 2" && b.getCurrentPresetName() == "Glue 2",
               "both targets follow the rename");
        check (comp.getUserPresets().size() == 1 && comp.getUserPresets()[0].name == "Glue 2",
               "the list has the new name only");

        // ---- 9. the global preset's dirty flag ------------------------------
        check (global.saveUserPreset ("Whole"), "save a global preset");
        settle();
        check (! global.isDirty(), "the global preset is clean after its save");
        b.setCurrentPreset ("Just a name", false, false);
        settle();
        check (! global.isDirty(), "module bookkeeping alone does not make it modified");
        check (b.loadFactoryPreset (0), "load a factory module preset into B");
        settle();
        check (near (value (apvts, "B_Comp_Ratio"), 12.0f) && b.currentPresetIsFactory()
                   && b.getCurrentFactoryIndex() == 0,
               "the factory preset is applied and current");
        check (global.isDirty(), "a module load makes the global preset modified");

        check (global.loadUserPreset (0), "load the global preset back");
        settle();
        check (b.getCurrentPresetName() == "Glue 2" && near (value (apvts, "B_Comp_Ratio"), 7.5f),
               "the module name and values saved in the global preset come back");
        check (! b.isDirty() && ! global.isDirty(), "nothing dirty after the global load");
    }

    folder.deleteRecursively();

    std::printf ("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
