/*
  ------------------------------------------------------------------------------
    util/ArrayView.h

    Read-only view over a contiguous sequence — the same idea as
    util/AudioBufferView.h, applied to plain arrays of values.

    It exists for the two directions a core container crosses the boundary:

      - as a *parameter*, it converts implicitly from any contiguous container
        with data() and size(), which covers the framework's own array type and
        std::vector alike, so a core signature can stop naming a framework type
        without any call site changing;
      - as a *return type*, it lets a class store std::vector internally — the
        convention everywhere else in core — while still handing callers
        something that answers to isEmpty(), size() and [] the way the
        framework array they used to get did. Existing call sites keep
        compiling.

    size() returns int rather than a size_t on purpose. The surrounding code is
    written in the framework's house style, where sizes and indices are int;
    returning an unsigned type would turn every existing `for (int i = 0;
    i < a.size(); ++i)` into a signed-unsigned warning for no gain.

    It owns nothing and copies nothing, so whatever it points at has to outlive
    it. Returning one from an accessor is safe — it points into a member that
    outlives the expression; storing one as a member is how you get a dangling
    view.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace fxme
{

namespace detail
{
    /** Matches any contiguous container: the framework's array type,
        std::vector, std::array. */
    template <typename T, typename = void>
    struct isContiguousContainer : std::false_type {};

    template <typename T>
    struct isContiguousContainer<T, decltype (void (std::declval<const T&>().data()),
                                              void (std::declval<const T&>().size()))>
        : std::true_type {};
}

//==============================================================================
template <typename ElementType>
class ArrayView
{
public:
    ArrayView() = default;

    ArrayView (const ElementType* first, int numElements) noexcept
        : items (first), count (numElements) {}

    /** Implicit conversion from any contiguous container of the same element
        type. This is what keeps call sites unchanged. */
    template <typename ContainerType,
              typename = typename std::enable_if<
                             detail::isContiguousContainer<ContainerType>::value
                             && ! std::is_same<typename std::decay<ContainerType>::type, ArrayView>::value>::type>
    ArrayView (const ContainerType& container) noexcept
        : items (container.data()), count (static_cast<int> (container.size())) {}

    //==============================================================================
    int  size()    const noexcept { return count; }
    bool isEmpty() const noexcept { return count <= 0; }
    bool empty()   const noexcept { return count <= 0; }

    const ElementType* data() const noexcept { return items; }

    const ElementType& operator[]   (int i) const noexcept { return items[i]; }
    const ElementType& getUnchecked (int i) const noexcept { return items[i]; }

    const ElementType& getFirst() const noexcept { return items[0]; }
    const ElementType& getLast()  const noexcept { return items[count - 1]; }

    const ElementType* begin() const noexcept { return items; }
    const ElementType* end()   const noexcept { return items + count; }

    /** Index of the first match, or -1 — the framework array's spelling and
        its return convention, so call sites testing against -1 still work. */
    int indexOf (const ElementType& value) const noexcept
    {
        for (int i = 0; i < count; ++i)
            if (items[i] == value)
                return i;

        return -1;
    }

    bool contains (const ElementType& value) const noexcept { return indexOf (value) >= 0; }

private:
    const ElementType* items = nullptr;
    int count = 0;
};

} // namespace fxme
