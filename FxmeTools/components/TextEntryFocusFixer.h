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
      EVERY click inside the editor starts a 10 Hz claim battle for the OS
      focus — a short (~0.8 s) window for plain clicks, so component
      shortcuts like Delete-on-a-selected-block work, and a longer ~2 s
      window while a field holds the caret, refreshed as long as the focus
      actually sticks. Both windows are bounded, so it never fights the
      host forever (and never yanks the focus back from a user who clicked
      into the DAW more than a moment ago); clicking again always restarts
      the battle. Harmless in standalone. If typing is still dead in a
      specific DAW, it's the host's keyboard routing (REAPER: FX menu ->
      "Send all keyboard input to plugin").

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
    static constexpr int clickWindowMs     = 800;    // plain clicks: short battle
    static constexpr int caretWindowMs     = 2000;   // active caret: refreshed battle

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

        grabPeerFocus();   // also covers non-click focus paths (tab, code)
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
        // Plain clicks fight briefly (enough to win the click's focus race,
        // short enough never to bother a user who moved on to the DAW); an
        // active caret keeps the longer, refreshed window.
        deadline = juce::Time::getMillisecondCounter()
                 + (current != nullptr ? caretWindowMs : clickWindowMs);
        startTimer (enforceIntervalMs);
    }

    void timerCallback() override
    {
        auto* peer = root.getPeer();
        if (peer == nullptr)
            return;

        const auto now = juce::Time::getMillisecondCounter();
        if (now > deadline)
        {
            stopTimer();                        // don't fight the host forever
            return;
        }

        if (peer->isFocused())
        {
            // Focus sticks. With a caret, keep watching (the host may steal
            // it back later); after a plain click just let the window lapse.
            if (current != nullptr)
                deadline = now + caretWindowMs;
        }
        else
        {
            peer->grabFocus();
        }
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
