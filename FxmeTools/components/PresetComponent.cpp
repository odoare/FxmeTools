/*
  ==============================================================================

    PresetComponent.cpp

  ==============================================================================
*/

#include "PresetComponent.h"

namespace fxme
{

PresetComponent::PresetComponent (PresetManager& managerToUse)
    : manager (managerToUse)
{
    manager.addChangeListener (this);

    currentLabel.setJustificationType (juce::Justification::centred);
    currentLabel.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    addAndMakeVisible (currentLabel);

    folderLabel.setJustificationType (juce::Justification::centred);
    folderLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    folderLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.4f));
    folderLabel.setText (manager.getUserPresetDirectory().getFullPathName(),
                         juce::dontSendNotification);
    folderLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (folderLabel);

    for (auto* b : { &prevButton, &nextButton, &saveButton, &saveAsButton, &renameButton, &deleteButton })
    {
        b->setLookAndFeel (&lookAndFeel);
        addAndMakeVisible (*b);
    }

    prevButton.setTooltip ("Previous preset");
    nextButton.setTooltip ("Next preset");
    saveButton.setTooltip ("Overwrite the current user preset (asks for a name if none)");
    saveAsButton.setTooltip ("Save the current state as a new user preset");
    renameButton.setTooltip ("Rename the selected user preset");
    deleteButton.setTooltip ("Delete the selected user preset");

    prevButton.onClick = [this] { manager.loadPrevious(); };
    nextButton.onClick = [this] { manager.loadNext(); };

    saveButton.onClick = [this]
    {
        // Overwrite in place only when the current preset is a user preset;
        // otherwise (factory / unnamed state) fall through to Save As.
        if (manager.getCurrentUserIndex() >= 0)
            manager.saveUserPreset (manager.getCurrentPresetName());
        else
            saveAsButton.onClick();
    };

    saveAsButton.onClick = [this]
    {
        auto suggested = manager.getCurrentPresetName();
        promptForName ("Save preset", suggested.isEmpty() ? "New Preset" : suggested,
                       [this] (const juce::String& name) { manager.saveUserPreset (name); });
    };

    renameButton.onClick = [this]
    {
        if (const auto* row = selectedRow(); row != nullptr && row->kind == Row::user)
        {
            const auto preset = manager.getUserPresets()[(size_t) row->presetIndex];
            promptForName ("Rename preset", preset.name,
                           [this, preset] (const juce::String& name)
                           { manager.renameUserPreset (preset, name); });
        }
    };

    deleteButton.onClick = [this]
    {
        const auto* row = selectedRow();
        if (row == nullptr || row->kind != Row::user)
            return;

        const auto preset = manager.getUserPresets()[(size_t) row->presetIndex];
        juce::AlertWindow::showOkCancelBox (
            juce::MessageBoxIconType::WarningIcon,
            "Delete preset",
            "Delete user preset \"" + preset.name + "\"?",
            {}, {}, this,
            juce::ModalCallbackFunction::create ([this, preset] (int result)
            {
                if (result == 1)
                    manager.deleteUserPreset (preset);
            }));
    };

    listBox.setModel (this);
    listBox.setRowHeight (26);
    listBox.setColour (juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha (0.25f));
    listBox.setColour (juce::ListBox::outlineColourId, juce::Colours::white.withAlpha (0.12f));
    listBox.setOutlineThickness (1);
    addAndMakeVisible (listBox);

    setAccentColour (accent);
    refresh();
}

PresetComponent::~PresetComponent()
{
    manager.removeChangeListener (this);
    for (auto* b : { &prevButton, &nextButton, &saveButton, &saveAsButton, &renameButton, &deleteButton })
        b->setLookAndFeel (nullptr);
}

void PresetComponent::setAccentColour (juce::Colour newAccent)
{
    accent = newAccent;

    currentLabel.setColour (juce::Label::textColourId, accent.brighter (0.5f));

    for (auto* b : { &prevButton, &nextButton, &saveButton, &saveAsButton, &renameButton, &deleteButton })
    {
        b->setColour (juce::TextButton::buttonColourId,  juce::Colours::black.withAlpha (0.4f));
        b->setColour (juce::TextButton::textColourOffId, accent.brighter (0.3f));
    }

    listBox.repaint();
    repaint();
}

//==============================================================================
void PresetComponent::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (2.0f);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRoundedRectangle (b, 4.0f);
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.drawRoundedRectangle (b, 4.0f, 1.0f);
}

void PresetComponent::resized()
{
    auto area = getLocalBounds().reduced (12, 10);
    auto col  = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), 560), area.getHeight());

    auto top = col.removeFromTop (44);
    prevButton.setBounds (top.removeFromLeft (44));
    nextButton.setBounds (top.removeFromRight (44));
    currentLabel.setBounds (top);

    folderLabel.setBounds (col.removeFromBottom (18));

    auto buttonRow = col.removeFromBottom (40);
    const int buttonW = buttonRow.getWidth() / 4;
    saveButton.setBounds   (buttonRow.removeFromLeft (buttonW));
    saveAsButton.setBounds (buttonRow.removeFromLeft (buttonW));
    renameButton.setBounds (buttonRow.removeFromLeft (buttonW));
    deleteButton.setBounds (buttonRow);

    listBox.setBounds (col.reduced (0, 8));
}

//==============================================================================
void PresetComponent::refresh()
{
    rebuildRows();
    listBox.updateContent();

    auto name = manager.getCurrentPresetName();
    if (name.isEmpty())
        name = "(no preset)";
    currentLabel.setText (manager.isDirty() ? name + " *" : name, juce::dontSendNotification);

    if (const int row = rowOfCurrentPreset(); row >= 0)
        listBox.selectRow (row);
    else
        listBox.deselectAllRows();

    updateButtonStates();
    listBox.repaint();
}

void PresetComponent::rebuildRows()
{
    rows.clear();

    if (! manager.getFactoryPresets().empty())
    {
        rows.push_back ({ Row::header, "FACTORY", -1 });
        int i = 0;
        for (const auto& p : manager.getFactoryPresets())
            rows.push_back ({ Row::factory, p.name, i++ });
    }

    rows.push_back ({ Row::header, "USER", -1 });
    int i = 0;
    for (const auto& p : manager.getUserPresets())
        rows.push_back ({ Row::user, p.name, i++ });
}

int PresetComponent::rowOfCurrentPreset() const
{
    const int fi = manager.getCurrentFactoryIndex();
    const int ui = manager.getCurrentUserIndex();

    for (size_t r = 0; r < rows.size(); ++r)
        if ((rows[r].kind == Row::factory && rows[r].presetIndex == fi)
         || (rows[r].kind == Row::user    && rows[r].presetIndex == ui))
            return (int) r;

    return -1;
}

const PresetComponent::Row* PresetComponent::selectedRow() const
{
    const int r = listBox.getSelectedRow();
    return juce::isPositiveAndBelow (r, (int) rows.size()) ? &rows[(size_t) r] : nullptr;
}

void PresetComponent::updateButtonStates()
{
    const auto* row = selectedRow();
    const bool userSelected = row != nullptr && row->kind == Row::user;
    renameButton.setEnabled (userSelected);
    deleteButton.setEnabled (userSelected);
}

void PresetComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

//==============================================================================
int PresetComponent::getNumRows()
{
    return (int) rows.size();
}

void PresetComponent::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                        int width, int height, bool rowIsSelected)
{
    if (! juce::isPositiveAndBelow (rowNumber, (int) rows.size()))
        return;

    const auto& row = rows[(size_t) rowNumber];
    const auto area = juce::Rectangle<int> (0, 0, width, height);

    if (row.kind == Row::header)
    {
        g.setColour (accent.withAlpha (0.8f));
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (row.text, area.reduced (10, 0), juce::Justification::centredLeft);

        const float y = (float) height * 0.5f;
        const float x = 20.0f + juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), row.text);
        g.setColour (accent.withAlpha (0.3f));
        g.drawHorizontalLine ((int) y, x, (float) width - 10.0f);
        return;
    }

    if (rowIsSelected)
    {
        g.setColour (accent.withAlpha (0.25f));
        g.fillRect (area);
    }

    const bool isCurrent = rowNumber == rowOfCurrentPreset();
    g.setColour (isCurrent ? accent.brighter (0.5f) : juce::Colours::white.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (15.0f)));
    g.drawText (row.text, area.reduced (18, 0), juce::Justification::centredLeft);
}

void PresetComponent::listBoxItemClicked (int rowNumber, const juce::MouseEvent&)
{
    if (! juce::isPositiveAndBelow (rowNumber, (int) rows.size()))
        return;

    const auto& row = rows[(size_t) rowNumber];

    if (row.kind == Row::factory)
        manager.loadFactoryPreset (row.presetIndex);
    else if (row.kind == Row::user)
        manager.loadUserPreset (row.presetIndex);
    else
        refresh();   // header clicked — restore the selection to the current preset
}

void PresetComponent::selectedRowsChanged (int)
{
    updateButtonStates();
}

void PresetComponent::deleteKeyPressed (int)
{
    deleteButton.onClick();
}

//==============================================================================
void PresetComponent::promptForName (const juce::String& title,
                                     const juce::String& initialName,
                                     std::function<void (const juce::String&)> onOk)
{
    auto* aw = new juce::AlertWindow (title, "Preset name:", juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor ("name", initialName);
    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([aw, onOk] (int result)
        {
            const auto name = aw->getTextEditorContents ("name").trim();
            if (result == 1 && name.isNotEmpty())
                onOk (name);
        }),
        true);   // delete the window when dismissed
}

} // namespace fxme
