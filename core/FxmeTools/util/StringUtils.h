/*
  ------------------------------------------------------------------------------
    util/StringUtils.h

    The handful of string operations the core code actually performs, so that
    parsing a note or chord name does not require a framework string class.

    These are the operations the text parsing in midi/MidiTools.h had been
    reaching into the framework for — trim, lower-case, suffix tests, a couple
    of slices and an integer parse — and nothing more. Each takes a StringRef,
    so it accepts a framework string, a std::string or a literal without any
    conversion at the call site, and each returns a std::string, because a
    result that was computed has to be owned by somebody.

    Whitespace is defined exactly as the framework defines it (space, or 9-13),
    so trim() agrees with what it replaces to the character.

    Two deliberate deviations, both about non-ASCII text:

      - Positions and lengths here are counted in *bytes*, not code points.
        For the ASCII note and chord names this library parses the two are the
        same. Slicing text that is not ASCII can split a multi-byte character,
        which yields a fragment that matches nothing and is then discarded — a
        lookup that would have failed still fails.
      - toLower() maps A-Z only, where the framework applies the full Unicode
        mapping. The one place this could show is the French note names, whose
        accented characters are already lower case, so it does not.

    Everything here is a pure function: no state, no locking. Only the ones
    returning std::string allocate, which keeps them off the audio thread —
    where none of this is called from anyway, since parsing a chord name is
    setup work.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/util/StringRef.h>

#include <cstddef>
#include <cstring>
#include <string>

namespace fxme
{

//==============================================================================
/** Space, tab, newline, vertical tab, form feed or carriage return — the same
    set the framework's own trim uses. */
inline bool isWhitespace (char c) noexcept
{
    return c == ' ' || (c >= 9 && c <= 13);
}

/** Removes leading and trailing whitespace. */
inline std::string trim (StringRef s)
{
    std::size_t b = 0;
    std::size_t e = s.length();

    while (b < e && isWhitespace (s[b]))
        ++b;

    while (e > b && isWhitespace (s[e - 1]))
        --e;

    return std::string (s.data() + b, e - b);
}

/** Maps A-Z to a-z and leaves every other byte alone. */
inline std::string toLower (StringRef s)
{
    std::string out (s.data(), s.length());

    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char> (c - 'A' + 'a');

    return out;
}

/** Maps a-z to A-Z and leaves every other byte alone. */
inline std::string toUpper (StringRef s)
{
    std::string out (s.data(), s.length());

    for (auto& c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char> (c - 'a' + 'A');

    return out;
}

//==============================================================================
inline bool startsWith (StringRef s, StringRef prefix) noexcept
{
    return prefix.length() <= s.length()
             && std::memcmp (s.data(), prefix.data(), prefix.length()) == 0;
}

inline bool endsWith (StringRef s, StringRef suffix) noexcept
{
    return suffix.length() <= s.length()
             && std::memcmp (s.data() + s.length() - suffix.length(),
                             suffix.data(), suffix.length()) == 0;
}

/** True if every byte of `s` appears in `allowed`. An empty string contains
    only allowed bytes and so returns true, matching what it replaces. */
inline bool containsOnly (StringRef s, StringRef allowed) noexcept
{
    for (std::size_t i = 0; i < s.length(); ++i)
    {
        bool found = false;

        for (std::size_t j = 0; j < allowed.length() && ! found; ++j)
            found = (s[i] == allowed[j]);

        if (! found)
            return false;
    }

    return true;
}

//==============================================================================
/** Everything but the last `n` bytes. Asking for more than there is gives an
    empty string rather than an error, as the framework version does. */
inline std::string dropLast (StringRef s, std::size_t n)
{
    return n >= s.length() ? std::string()
                           : std::string (s.data(), s.length() - n);
}

/** From `start` to the end, clamped. */
inline std::string substring (StringRef s, std::size_t start)
{
    return start >= s.length() ? std::string()
                               : std::string (s.data() + start, s.length() - start);
}

/** The half-open range [start, end), clamped at both ends. */
inline std::string substring (StringRef s, std::size_t start, std::size_t end)
{
    if (end > s.length())
        end = s.length();

    return start >= end ? std::string()
                        : std::string (s.data() + start, end - start);
}

//==============================================================================
/** Reads an optionally signed run of digits from the front and stops at the
    first byte that is not one, returning 0 when there are none. This is the
    behaviour of the framework's getIntValue, including its silence about
    trailing junk. Overflow is not detected, also as there. */
inline int toInt (StringRef s) noexcept
{
    std::size_t i = 0;
    bool negative = false;

    while (i < s.length() && isWhitespace (s[i]))
        ++i;

    if (i < s.length() && (s[i] == '-' || s[i] == '+'))
    {
        negative = (s[i] == '-');
        ++i;
    }

    int value = 0;

    while (i < s.length() && s[i] >= '0' && s[i] <= '9')
        value = value * 10 + (s[i++] - '0');

    return negative ? -value : value;
}

} // namespace fxme
