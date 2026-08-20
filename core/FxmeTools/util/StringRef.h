/*
  ------------------------------------------------------------------------------
    util/StringRef.h

    Non-owning reference to a null-terminated UTF-8 string — the text
    counterpart to util/AudioBufferView.h, and it works the same way.

    A core function that took a framework string parameter takes a StringRef
    instead. Because StringRef converts *implicitly* from anything shaped like
    a framework string — detected structurally, without including anything from
    that framework — the call sites do not change:

        int getNoteNumber (StringRef name);     // in core

        getNoteNumber (someFrameworkString);    // still compiles, no adapter
        getNoteNumber (std::string ("c#4"));    // and so does this
        getNoteNumber ("c#4");                  // and this

    The shape it looks for is `toRawUTF8()` plus `getNumBytesAsUTF8()`, which
    is what the framework half of this library provides. Where only
    `toRawUTF8()` exists the length is measured instead, so a narrower
    string type still binds.

    This is a *reference*: it stores a pointer and a length and copies nothing,
    so the string it points at has to outlive it. That makes it right for
    parameters and wrong for members — the same rule AudioBufferView follows.
    Binding one to a temporary is safe for the duration of the full expression,
    which covers the ordinary `f (makeAString())` case, and unsafe past it.

    Always null-terminated, so `data()` can be handed to a C interface. Holding
    no allocation of its own, it neither allocates nor throws.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

namespace fxme
{

namespace detail
{
    /** Matches a framework string that can hand out UTF-8 and knows its own
        byte count — the cheap path, no measuring needed. */
    template <typename T, typename = void>
    struct isSizedUtf8String : std::false_type {};

    template <typename T>
    struct isSizedUtf8String<T, decltype (void (std::declval<const T&>().toRawUTF8()),
                                          void (std::declval<const T&>().getNumBytesAsUTF8()))>
        : std::true_type {};

    /** Matches a string that can hand out UTF-8 but cannot say how much. */
    template <typename T, typename = void>
    struct isUnsizedUtf8String : std::false_type {};

    template <typename T>
    struct isUnsizedUtf8String<T, decltype (void (std::declval<const T&>().toRawUTF8()))>
        : std::true_type {};
}

//==============================================================================
class StringRef
{
public:
    StringRef() = default;

    StringRef (const char* nullTerminated) noexcept
        : text (nullTerminated != nullptr ? nullTerminated : ""),
          len  (nullTerminated != nullptr ? std::strlen (nullTerminated) : 0)
    {
    }

    /** For a substring of something already null-terminated at `start + length`.
        The caller is promising that; nothing here can check it. */
    StringRef (const char* start, std::size_t length) noexcept
        : text (start != nullptr ? start : ""), len (start != nullptr ? length : 0)
    {
    }

    StringRef (const std::string& s) noexcept
        : text (s.c_str()), len (s.size())
    {
    }

    /** Implicit conversion from a framework string that knows its byte count. */
    template <typename StringType,
              typename = typename std::enable_if<detail::isSizedUtf8String<StringType>::value>::type>
    StringRef (const StringType& s)
        : text (s.toRawUTF8()), len (static_cast<std::size_t> (s.getNumBytesAsUTF8()))
    {
    }

    /** Same, for one that does not — the length is measured. */
    template <typename StringType,
              typename = typename std::enable_if<detail::isUnsizedUtf8String<StringType>::value
                                                 && ! detail::isSizedUtf8String<StringType>::value>::type,
              typename = void>
    StringRef (const StringType& s)
        : text (s.toRawUTF8()), len (std::strlen (s.toRawUTF8()))
    {
    }

    //==============================================================================
    /** Null-terminated, so this is safe to pass to a C interface. */
    const char* data()   const noexcept { return text; }
    std::size_t length() const noexcept { return len; }
    std::size_t size()   const noexcept { return len; }

    bool isEmpty()    const noexcept { return len == 0; }
    bool isNotEmpty() const noexcept { return len != 0; }
    bool empty()      const noexcept { return len == 0; }

    char operator[] (std::size_t i) const noexcept { return text[i]; }

    const char* begin() const noexcept { return text; }
    const char* end()   const noexcept { return text + len; }

    /** Copies out. The only operation here that allocates. */
    std::string str() const { return std::string (text, len); }

    //==============================================================================
    /** Byte comparison, not a Unicode collation: two spellings that normalise to
        the same text compare unequal. Everything this library compares is
        note and chord names, which are pre-normalised by construction. */
    bool operator== (const StringRef& other) const noexcept
    {
        return len == other.len && std::memcmp (text, other.text, len) == 0;
    }

    bool operator!= (const StringRef& other) const noexcept { return ! operator== (other); }

    bool operator< (const StringRef& other) const noexcept
    {
        const std::size_t n = len < other.len ? len : other.len;
        const int c = n == 0 ? 0 : std::memcmp (text, other.text, n);
        return c != 0 ? c < 0 : len < other.len;
    }

private:
    const char* text = "";
    std::size_t len  = 0;
};

} // namespace fxme
