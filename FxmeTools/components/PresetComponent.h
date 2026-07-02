/*
  ==============================================================================

    PresetComponent.h

    Ready-made preset browser for FX-Mechanics plugins, designed to fill a
    "Presets" tab. Shows the current preset name (with a dirty marker), a
    prev/next stepper, a sectioned list of factory and user presets
    (click to load), and Save / Save As / Rename / Delete buttons operating
    on the user bank. All file handling is delegated to a PresetManager.

    Usage (editor side):

        presetComponent = std::make_unique<fxme::PresetComponent> (processor.getPresetManager());
        presetComponent->setAccentColour (myThemeColour);
        tabs.addTab ("Presets", bg, presetComponent.get(), false);

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../presets/PresetManager.h"
#include "../lookandfeels/FxmeLookAndFeel.h"

namespace fxme
{

class PresetComponent : public juce::Component,
                        private juce::ChangeListener,
                        private juce::ListBoxModel
{
public:
    explicit PresetComponent (PresetManager& managerToUse);
    ~PresetComponent() override;

    void setAccentColour (juce::Colour newAccent);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row
    {
        enum Kind { header, factory, user };
        Kind kind;
        juce::String text;
        int presetIndex = -1;   // index into the manager's factory or user list
    };

    void refresh();
    void rebuildRows();
    int  rowOfCurrentPreset() const;
    void updateButtonStates();
    const Row* selectedRow() const;

    void promptForName (const juce::String& title,
                        const juce::String& initialName,
                        std::function<void (const juce::String&)> onOk);

    // ChangeListener
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics&, int width, int height, bool rowIsSelected) override;
    void listBoxItemClicked (int rowNumber, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void deleteKeyPressed (int lastRowSelected) override;

    PresetManager& manager;

    std::vector<Row> rows;

    juce::Label currentLabel, folderLabel;
    juce::TextButton prevButton { "<" }, nextButton { ">" };
    juce::TextButton saveButton { "Save" }, saveAsButton { "Save As" },
                     renameButton { "Rename" }, deleteButton { "Delete" };
    juce::ListBox listBox;

    juce::Colour accent { juce::Colours::orange };
    FxmeLookAndFeel lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetComponent)
};

} // namespace fxme
