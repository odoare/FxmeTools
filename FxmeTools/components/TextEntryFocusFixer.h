/*
  ------------------------------------------------------------------------------
    TextEntryFocusFixer.h

    Reliable keyboard behaviour for juce::TextEditor fields inside (Linux)
    plugin windows. An editor that contains any TextEditor owns ONE fixer,
    constructed with the editor as root:

        fxme::TextEntryFocusFixer textEntryFixer { *this };

    What it fixes, for every present and future TextEditor under the root:

    - Linux plugin focus: hosted plugin windows often keep the X11 input
      focus in the DAW while JUCE shows a blinking caret — typing then goes
      to the host, and a single focus request loses the race (hosts re-take
      focus while handling the same click). The fixer enforces it instead:
      every click inside the editor claims the OS focus (which also makes
      plain keyboard shortcuts work after clicking the UI), and while a
      field holds the caret the focus is re-asserted at 10 Hz for a bounded
      ~2 s window, refreshed as long as it actually sticks — so it never
      fights the host forever, and clicking the field again always restarts
      the battle. Harmless in standalone.

    - Return commits (the field's own onReturnKey runs first) and *leaves*
      the field — single-line editors only, multiline keeps Return as
      newline.

    - Escape reverts to the text the field had when it was focused, and
      leaves.

    Conventions the fixer relies on (see the calling code, not this class):
    refresh paths must guard setText() with hasKeyboardFocus(true), and
    transport/utility buttons should setMouseClickGrabsKeyboardFocus(false).

    Message thread only.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace fxme
{

class TextEntryFocusFixer : private juce::MouseListener,
                            private juce::Timer,
                            private juce::FocusChangeListener
{
public:
    explicit TextEntryFocusFixer (juce::Component& rootComponent)
        : root (rootComponent)
    {
        root.addMouseListener (this, true);
        juce::Desktop::getInstance().addFocusChangeListener (this);
    }

    ~TextEntryFocusFixer() override
    {
        detachCurrent();
        juce::Desktop::getInstance().removeFocusChangeListener (this);
        root.removeMouseListener (this);
    }

private:
    static constexpr int enforceIntervalMs = 100;    // 10 Hz
    static constexpr int enforceWindowMs   = 2000;   // give up after ~2 s of losing

    juce::Component& root;
    juce::Component::SafePointer<juce::TextEditor> current;
    juce::String textAtFocus;
    std::function<void()> savedReturn, savedEscape;
    juce::uint32 deadline = 0;

    // ---- mouse: claim the OS focus on every click inside the editor --------
    void mouseDown (const juce::MouseEvent&) override
    {
        grabPeerFocus();
        restartEnforcement();
    }

    // ---- desktop focus: follow the caret ------------------------------------
    void globalFocusChanged (juce::Component* focused) override
    {
        auto* ed = dynamic_cast<juce::TextEditor*> (focused);
        if (ed != nullptr && root.isParentOf (ed))
        {
            if (current == ed)
                return;
            detachCurrent();
            attach (*ed);
        }
        else if (current != nullptr && focused != current.getComponent())
        {
            detachCurrent();
        }
    }

    void attach (juce::TextEditor& ed)
    {
        current     = &ed;
        textAtFocus = ed.getText();
        savedReturn = ed.onReturnKey;
        savedEscape = ed.onEscapeKey;

        ed.onReturnKey = [this]
        {
            if (savedReturn) savedReturn();
            if (current != nullptr && ! current->isMultiLine())
                current->giveAwayKeyboardFocus();
        };
        ed.onEscapeKey = [this]
        {
            if (savedEscape) savedEscape();
            if (current != nullptr)
            {
                current->setText (textAtFocus, false);
                current->giveAwayKeyboardFocus();
            }
        };

        restartEnforcement();
    }

    void detachCurrent()
    {
        if (current != nullptr)
        {
            current->onReturnKey = savedReturn;
            current->onEscapeKey = savedEscape;
        }
        current = nullptr;
        savedReturn = nullptr;
        savedEscape = nullptr;
        stopTimer();
    }

    // ---- focus enforcement ---------------------------------------------------
    void restartEnforcement()
    {
        deadline = juce::Time::getMillisecondCounter() + enforceWindowMs;
        if (current != nullptr)
            startTimer (enforceIntervalMs);
    }

    void timerCallback() override
    {
        if (current == nullptr)
        {
            stopTimer();
            return;
        }

        auto* peer = root.getPeer();
        if (peer == nullptr)
            return;

        const auto now = juce::Time::getMillisecondCounter();
        if (peer->isFocused())
            deadline = now + enforceWindowMs;   // it sticks: keep watching
        else if (now > deadline)
            stopTimer();                        // don't fight the host forever
        else
            peer->grabFocus();
    }

    void grabPeerFocus()
    {
        if (auto* peer = root.getPeer())
            if (! peer->isFocused())
                peer->grabFocus();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TextEntryFocusFixer)
};

} // namespace fxme
