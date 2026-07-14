/*
  ==============================================================================

    TextEntryFocusFixer.h

    Makes text entry reliable across hosts, especially Linux plugin windows.

    Hosted plugin windows on Linux often keep the X11 input focus in the host
    even though JUCE shows a blinking caret in a TextEditor — typing then goes
    to the DAW, not the field. A single focus request after the click is not
    enough: many hosts re-take the focus while (or shortly after) handling the
    same click. This helper therefore *enforces* the focus:

      - every mouse click anywhere inside `root` claims the OS input focus for
        the plugin window (this also makes plain keyboard shortcuts work after
        clicking the UI, not just text entry);
      - while a TextEditor inside `root` holds the caret, the focus is
        re-asserted at 10 Hz for a bounded window (~2 s, refreshed for as long
        as the window actually keeps the focus or the user interacts). The
        bound means we never fight the host forever: if the DAW insists on
        keeping the keyboard, we give up until the next click — clicking the
        field again always restarts the battle.

    It also normalises the keyboard behaviour of every TextEditor under
    `root` (present and future ones — it hooks the global focus change),
    including the inline editors JUCE creates for editable Labels (e.g. the
    FxmeSlider right-click value entry):

      - Return commits (the field's own handler runs first) and then *leaves*
        the field (single-line editors only), so keyboard shortcuts go back
        to the rest of the UI;
      - Escape restores the text the field had when it gained focus and
        leaves without committing.

    Usage: give the plugin editor one member, constructed with itself as root:

        fxme::TextEntryFocusFixer textEntryFixer { *this };

    Two conventions keep the behaviour predictable:
      - never call setText() on a field from a refresh/timer path without
        checking `hasKeyboardFocus (true)` first (it would stomp typing);
      - buttons that must not disturb typing or keyboard shortcuts should get
        `setMouseClickGrabsKeyboardFocus (false)` — but leave it *enabled* on
        buttons whose action needs pending edits committed first (the focus
        loss triggers the field's commit).

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace fxme
{

class TextEntryFocusFixer : private juce::FocusChangeListener,
                            private juce::MouseListener,
                            private juce::Timer
{
public:
    explicit TextEntryFocusFixer (juce::Component& root) : root_ (root)
    {
        juce::Desktop::getInstance().addFocusChangeListener (this);
        root_.addMouseListener (this, true);   // all nested children
    }

    ~TextEntryFocusFixer() override
    {
        root_.removeMouseListener (this);
        juce::Desktop::getInstance().removeFocusChangeListener (this);
    }

private:
    void globalFocusChanged (juce::Component* focused) override
    {
        auto* ed = dynamic_cast<juce::TextEditor*> (focused);
        if (ed == nullptr || ! root_.isParentOf (ed))
        {
            stopTimer();   // none of our fields holds the caret any more
            return;
        }

        current_     = ed;
        textOnFocus_ = ed->getText();

        // Escape = revert to the text present on focus, and leave.
        ed->onEscapeKey = [this]
        {
            if (auto* e = current_.getComponent())
            {
                e->setText (textOnFocus_, false);
                e->giveAwayKeyboardFocus();
            }
        };

        // Return = commit (the field's own handler) and leave — single-line
        // fields only; multiline editors keep Return as a line break.
        // Wrapped once per editor.
        if (! ed->getProperties().contains (kReturnWrapped))
        {
            ed->getProperties().set (kReturnWrapped, true);
            auto prev = ed->onReturnKey;
            juce::Component::SafePointer<juce::TextEditor> safe (ed);
            ed->onReturnKey = [prev, safe]
            {
                if (prev != nullptr)
                    prev();
                if (safe != nullptr && ! safe->isMultiLine())
                    safe->giveAwayKeyboardFocus();
            };
        }

        startEnforcing();
    }

    // Any click inside the editor: claim the OS input focus for our window
    // (hosted Linux windows often leave it with the DAW otherwise).
    void mouseDown (const juce::MouseEvent&) override
    {
        grabPeerFocus();
        if (fieldHasCaret())
            startEnforcing();
    }

    void timerCallback() override
    {
        if (! fieldHasCaret())
        {
            stopTimer();
            return;
        }

        if (auto* peer = root_.getPeer())
        {
            if (peer->isFocused())
            {
                // We actually hold the OS focus: keep watching (refresh the
                // deadline) in case the host snatches it back later.
                enforceUntil_ = juce::Time::getMillisecondCounter() + kEnforceMs;
            }
            else if (juce::Time::getMillisecondCounter() < enforceUntil_)
            {
                peer->grabFocus();
            }
            else
            {
                stopTimer();   // the host insists — give up until the next click
            }
        }
    }

    void startEnforcing()
    {
        enforceUntil_ = juce::Time::getMillisecondCounter() + kEnforceMs;
        grabPeerFocus();
        startTimer (100);
    }

    bool fieldHasCaret() const
    {
        auto* e = current_.getComponent();
        return e != nullptr && e->hasKeyboardFocus (true);
    }

    void grabPeerFocus()
    {
        if (auto* peer = root_.getPeer())
            if (! peer->isFocused())
                peer->grabFocus();
    }

    static constexpr juce::uint32 kEnforceMs = 2000;

    juce::Component& root_;
    juce::Component::SafePointer<juce::TextEditor> current_;
    juce::String textOnFocus_;
    juce::uint32 enforceUntil_ = 0;

    static constexpr const char* kReturnWrapped = "fxmeReturnWrapped";

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TextEntryFocusFixer)
};

} // namespace fxme
