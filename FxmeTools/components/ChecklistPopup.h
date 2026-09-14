/*
  ------------------------------------------------------------------------------
    ChecklistPopup.h

    A list of rows, each a few columns of text with a checkbox in front, under
    a title, with All / None / Cancel / OK. The host gets the checked state of
    every row when OK is pressed, and nothing otherwise, so Cancel really does
    change nothing. Built for "choose which of these to keep" questions: which
    measurement runs feed an analysis, which files to import, which presets to
    export.

    Usage:

        std::vector<fxme::ChecklistPopup::Row> rows;
        rows.push_back ({ { "1", "12:04", "first take" }, true });
        rows.push_back ({ { "2", "12:11", "mic knocked" }, false });

        auto popup = std::make_unique<fxme::ChecklistPopup> (
            "Runs to include",
            std::vector<fxme::ChecklistPopup::Column> { { "#", 30 }, { "Time", 90 },
                                                        { "Comment", 0 } },
            std::move (rows));
        popup->validate = [] (const std::vector<bool>& checked)
        {
            return std::find (checked.begin(), checked.end(), true) == checked.end()
                       ? juce::String ("Keep at least one run") : juce::String();
        };
        popup->onOk = [safe = juce::Component::SafePointer<MyEditor> (this)]
                      (const std::vector<bool>& checked)
        {
            if (safe != nullptr)
                safe->applySelection (checked);
        };
        fxme::ChecklistPopup::showAsCallOut (std::move (popup), runsButton);

    Interaction: a click on a row toggles it (shift- or cmd-click only selects,
    for a range), Space toggles the selected rows, Return presses OK. Hovering a
    row shows every column of it in full as a tooltip, so a long comment that
    is cut short in its column can still be read (the host needs a
    juce::TooltipWindow, as for any tooltip).

    Closing: OK calls onOk and closes. Cancel, Escape and a click outside the
    callout close it without calling anything. The popup closes itself when it
    sits in a juce::CallOutBox (showAsCallOut) or in a juce::DialogWindow
    launched with LaunchOptions::launchAsync; hosted anywhere else, the host
    removes it from onOk.

    Never a synchronous modal loop: plugins must not run one, and neither
    launcher above does.

    Palette is injected via setColours() (defaults to a dark theme).

    Author: Olivier Doaré, github.com/odoare
    Dual-licensed, mirroring the JUCE framework it depends on: under the GNU
    AGPL Version 3.0, or under commercial terms available from the author.
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace fxme
{

class ChecklistPopup : public juce::Component,
                       private juce::ListBoxModel
{
public:
    /** Popup palette. Defaults to a dark theme. */
    struct Colours
    {
        juce::Colour panel     { 0xff20202c };   // background
        juce::Colour panelLine { 0xff3a3a4c };   // border, header rule, buttons
        juce::Colour text      { 0xffd8d8e0 };   // title, checked rows
        juce::Colour dimText   { 0xff8a8a9a };   // header, unchecked rows, summary
        juce::Colour accent    { 0xffe0586f };   // checkbox fill, OK button
        juce::Colour warning   { 0xffe0a040 };   // the reason OK is refused
    };

    struct Column
    {
        juce::String name;
        int width = 100;            // pixels; 0 = share the width left over
    };

    struct Row
    {
        juce::StringArray cells;    // one string per column, missing ones blank
        bool checked = true;
    };

    ChecklistPopup (const juce::String& titleText, std::vector<Column> columnList,
                    std::vector<Row> rowList)
        : columns (std::move (columnList)), rows (std::move (rowList))
    {
        title.setText (titleText, juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        addAndMakeVisible (title);

        description.setFont (juce::Font (juce::FontOptions (12.0f)));
        description.setJustificationType (juce::Justification::topLeft);
        description.setMinimumHorizontalScale (1.0f);   // wrap, never squash
        addChildComponent (description);

        list.setModel (this);
        list.setRowHeight (rowHeight);
        list.setMultipleSelectionEnabled (true);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        summary.setFont (juce::Font (juce::FontOptions (12.0f)));
        summary.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (summary);

        allButton.setButtonText ("All");
        allButton.onClick = [this] { setAll (true); };
        addAndMakeVisible (allButton);

        noneButton.setButtonText ("None");
        noneButton.onClick = [this] { setAll (false); };
        addAndMakeVisible (noneButton);

        cancelButton.setButtonText ("Cancel");
        cancelButton.onClick = [this] { close(); };
        addAndMakeVisible (cancelButton);

        okButton.setButtonText ("OK");
        okButton.onClick = [this] { confirm(); };
        addAndMakeVisible (okButton);

        applyColours();
        updateSize();
        refreshValidation();
    }

    ~ChecklistPopup() override
    {
        list.setModel (nullptr);
    }

    //==========================================================================
    void setColours (const Colours& c)
    {
        colours = c;
        applyColours();
        repaint();
    }

    const Colours& getColours() const noexcept      { return colours; }

    /** Optional text under the title, e.g. what unchecking a row does. Wraps
        over as many lines as it needs; the popup grows to fit. */
    void setDescription (const juce::String& text)
    {
        description.setText (text, juce::dontSendNotification);
        description.setVisible (text.isNotEmpty());
        updateSize();
    }

    /** Checks each selection as it changes. Return an empty string to allow
        OK, or the reason it is refused, shown beside the buttons with OK
        disabled. It also runs when the popup is shown, so assigning it any time
        before that is enough; call revalidate() after a later change. */
    std::function<juce::String (const std::vector<bool>& checked)> validate;

    /** OK was pressed: the checked state of every row, in the order the rows
        were given. The popup is already closing when this runs. */
    std::function<void (const std::vector<bool>& checked)> onOk;

    std::vector<bool> getCheckedStates() const
    {
        std::vector<bool> checked;
        checked.reserve (rows.size());
        for (const auto& r : rows)
            checked.push_back (r.checked);
        return checked;
    }

    /** Re-runs validate on the current selection. */
    void revalidate()                               { refreshValidation(); }

    /** Launches the popup in a juce::CallOutBox pointing at `anchor`, inside
        anchor's top-level component, so it stays within a plugin editor's
        window. The callout owns the popup and deletes it on close. */
    static void showAsCallOut (std::unique_ptr<ChecklistPopup> popup, juce::Component& anchor)
    {
        auto* parent = anchor.getTopLevelComponent();
        const auto area = parent->getLocalArea (&anchor, anchor.getLocalBounds());
        juce::CallOutBox::launchAsynchronously (std::move (popup), area, parent);
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours.panel);
        g.setColour (colours.panelLine);
        g.drawRect (getLocalBounds(), 1);

        // Column names, on the same x positions paintListBoxItem uses.
        g.setColour (colours.dimText);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        const auto widths = columnWidths (list.getVisibleRowWidth());
        int x = list.getX() + list.getOutlineThickness() + checkboxColumnWidth;
        for (size_t c = 0; c < columns.size(); ++c)
        {
            g.drawText (columns[c].name, x + cellPadding, headerArea.getY(),
                        widths[c] - 2 * cellPadding, headerArea.getHeight(),
                        juce::Justification::centredLeft, true);
            x += widths[c];
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (margin);

        title.setBounds (r.removeFromTop (titleHeight));
        if (description.isVisible())
        {
            r.removeFromTop (2);
            description.setBounds (r.removeFromTop (descriptionHeight));
        }
        r.removeFromTop (8);

        auto bar = r.removeFromBottom (buttonHeight);
        allButton.setBounds (bar.removeFromLeft (52));
        bar.removeFromLeft (4);
        noneButton.setBounds (bar.removeFromLeft (60));
        okButton.setBounds (bar.removeFromRight (64));
        bar.removeFromRight (6);
        cancelButton.setBounds (bar.removeFromRight (76));
        bar.removeFromRight (10);
        bar.removeFromLeft (10);
        summary.setBounds (bar);
        r.removeFromBottom (8);

        headerArea = r.removeFromTop (headerHeight);
        list.setBounds (r);
    }

    void parentHierarchyChanged() override
    {
        refreshValidation();    // validate may have been assigned after construction
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::spaceKey))
        {
            toggleSelectedRows();
            return true;
        }
        return false;
    }

private:
    static constexpr int margin              = 12;
    static constexpr int titleHeight         = 22;
    static constexpr int headerHeight        = 20;
    static constexpr int rowHeight           = 22;
    static constexpr int buttonHeight        = 26;
    static constexpr int checkboxColumnWidth = 28;
    static constexpr int cellPadding         = 4;
    static constexpr int flexibleMinWidth    = 200;  // a 0-width column's share, at least
    static constexpr int maxVisibleRows      = 14;
    static constexpr int minVisibleRows      = 3;
    static constexpr int minWidth            = 440;  // the button bar
    static constexpr int scrollbarAllowance  = 18;   // widest stock scrollbar

    //==========================================================================
    // ListBoxModel

    int getNumRows() override                       { return (int) rows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                           bool selected) override
    {
        if (row < 0 || row >= (int) rows.size())
            return;

        const auto& r = rows[(size_t) row];

        if (selected)
            g.fillAll (colours.accent.withAlpha (0.16f));
        else if (row % 2 == 1)
            g.fillAll (colours.panelLine.withAlpha (0.25f));

        // Checkbox.
        const auto box = juce::Rectangle<float> (14.0f, 14.0f)
                             .withCentre ({ checkboxColumnWidth * 0.5f, height * 0.5f });
        if (r.checked)
        {
            g.setColour (colours.accent);
            g.fillRoundedRectangle (box, 3.0f);

            juce::Path tick;
            tick.startNewSubPath (box.getX() + 3.0f, box.getCentreY());
            tick.lineTo (box.getX() + 6.0f, box.getBottom() - 3.5f);
            tick.lineTo (box.getRight() - 3.0f, box.getY() + 3.5f);
            g.setColour (colours.panel);
            g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }
        else
        {
            g.setColour (colours.dimText);
            g.drawRoundedRectangle (box.reduced (0.5f), 3.0f, 1.0f);
        }

        // Cells, dimmed when the row is left out.
        g.setColour (r.checked ? colours.text : colours.dimText);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        const auto widths = columnWidths (width);
        int x = checkboxColumnWidth;
        for (size_t c = 0; c < columns.size(); ++c)
        {
            g.drawText (r.cells[(int) c], x + cellPadding, 0, widths[c] - 2 * cellPadding, height,
                        juce::Justification::centredLeft, true);
            x += widths[c];
        }
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        // Modifier clicks extend the selection for Space instead of toggling.
        if (e.mods.isShiftDown() || e.mods.isCommandDown())
            return;
        toggleRow (row);
    }

    void returnKeyPressed (int) override
    {
        if (okButton.isEnabled())
            confirm();
    }

    juce::String getNameForRow (int row) override
    {
        if (row < 0 || row >= (int) rows.size())
            return {};
        return (rows[(size_t) row].checked ? "Checked: " : "Unchecked: ")
             + rows[(size_t) row].cells.joinIntoString (", ");
    }

    juce::String getTooltipForRow (int row) override
    {
        if (row < 0 || row >= (int) rows.size())
            return {};

        juce::StringArray lines;
        for (size_t c = 0; c < columns.size(); ++c)
        {
            const auto cell = rows[(size_t) row].cells[(int) c];
            if (cell.isNotEmpty())
                lines.add (columns[c].name.isNotEmpty() ? columns[c].name + ": " + cell : cell);
        }
        return lines.joinIntoString ("\n");
    }

    //==========================================================================
    /** Fixed columns get their width; 0-width ones split what is left of
        `rowWidth`, never below flexibleMinWidth. */
    std::vector<int> columnWidths (int rowWidth) const
    {
        std::vector<int> widths;
        int fixed = 0, flexible = 0;
        for (const auto& c : columns)
        {
            fixed += juce::jmax (0, c.width);
            flexible += c.width <= 0 ? 1 : 0;
        }

        const int share = flexible > 0
            ? juce::jmax (flexibleMinWidth,
                          (rowWidth - checkboxColumnWidth - fixed) / flexible)
            : 0;
        for (const auto& c : columns)
            widths.push_back (c.width > 0 ? c.width : share);
        return widths;
    }

    void updateSize()
    {
        int w = checkboxColumnWidth;
        for (const auto& c : columns)
            w += c.width > 0 ? c.width : flexibleMinWidth;
        w += scrollbarAllowance + 2;                         // scrollbar + outline
        w = juce::jmax (minWidth, w + 2 * margin);

        descriptionHeight = 0;
        if (description.isVisible())
        {
            // Wrap the text at the label's text width (the popup's inner
            // width less the label's own 5 px side insets) to size it.
            juce::AttributedString s;
            s.append (description.getText(), description.getFont());
            juce::TextLayout layout;
            layout.createLayout (s, (float) (w - 2 * margin - 10));
            descriptionHeight = (int) std::ceil (layout.getHeight()) + 4;
        }

        const int visibleRows = juce::jlimit (minVisibleRows, maxVisibleRows, (int) rows.size());
        const int h = 2 * margin + titleHeight
                    + (descriptionHeight > 0 ? 2 + descriptionHeight : 0)
                    + 8 + headerHeight + visibleRows * rowHeight + 2
                    + 8 + buttonHeight;
        setSize (w, h);
    }

    void applyColours()
    {
        title.setColour (juce::Label::textColourId, colours.text);
        description.setColour (juce::Label::textColourId, colours.dimText);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list.setColour (juce::ListBox::outlineColourId, colours.panelLine);

        for (auto* b : { &allButton, &noneButton, &cancelButton })
        {
            b->setColour (juce::TextButton::buttonColourId, colours.panelLine);
            b->setColour (juce::TextButton::textColourOffId, colours.text);
        }
        okButton.setColour (juce::TextButton::buttonColourId, colours.accent.darker (0.4f));
        okButton.setColour (juce::TextButton::textColourOffId, colours.text);

        refreshValidation();
    }

    void toggleRow (int row)
    {
        if (row < 0 || row >= (int) rows.size())
            return;
        rows[(size_t) row].checked = ! rows[(size_t) row].checked;
        list.repaintRow (row);
        refreshValidation();
    }

    /** Space on a selection: all of it takes the opposite of the first
        selected row's state, so a mixed selection ends up uniform. */
    void toggleSelectedRows()
    {
        const auto selected = list.getSelectedRows();
        if (selected.isEmpty())
            return;

        const int first = selected[0];
        if (first < 0 || first >= (int) rows.size())
            return;

        const bool newState = ! rows[(size_t) first].checked;
        for (int i = 0; i < selected.size(); ++i)
            if (const int row = selected[i]; row >= 0 && row < (int) rows.size())
                rows[(size_t) row].checked = newState;

        list.repaint();
        refreshValidation();
    }

    void setAll (bool checked)
    {
        for (auto& r : rows)
            r.checked = checked;
        list.repaint();
        refreshValidation();
    }

    void refreshValidation()
    {
        const auto checked = getCheckedStates();
        const auto reason = validate != nullptr ? validate (checked) : juce::String();
        okButton.setEnabled (reason.isEmpty());

        if (reason.isNotEmpty())
        {
            summary.setColour (juce::Label::textColourId, colours.warning);
            summary.setText (reason, juce::dontSendNotification);
        }
        else
        {
            const auto n = std::count (checked.begin(), checked.end(), true);
            summary.setColour (juce::Label::textColourId, colours.dimText);
            summary.setText (juce::String ((int) n) + " of " + juce::String ((int) rows.size())
                                 + " checked",
                             juce::dontSendNotification);
        }
    }

    void confirm()
    {
        // Copied first: closing starts the popup's deletion (asynchronously,
        // but a host callback should not have to know that).
        auto callback = onOk;
        const auto checked = getCheckedStates();
        close();
        if (callback != nullptr)
            callback (checked);
    }

    void close()
    {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
        else if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (0);
    }

    //==========================================================================
    std::vector<Column> columns;
    std::vector<Row> rows;
    Colours colours;

    juce::Label title, description, summary;
    juce::ListBox list;
    juce::TextButton allButton, noneButton, cancelButton, okButton;
    juce::Rectangle<int> headerArea;
    int descriptionHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChecklistPopup)
};

} // namespace fxme
