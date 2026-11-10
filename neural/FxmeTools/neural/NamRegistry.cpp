/*
  ------------------------------------------------------------------------------
    NamRegistry.cpp

    NAM Core registers each architecture with a static object in that
    architecture's .cpp (lstm.cpp, container.cpp, wavenet/model.cpp). From a
    static library the linker only takes the object files something
    references, and nothing references lstm.cpp or container.cpp: their
    registration would silently vanish ("No config parser registered for
    architecture: LSTM"). Taking the address of each create_config pulls
    them in, in every consumer, whatever its link.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <NAM/container.h>
#include <NAM/lstm.h>
#include <NAM/wavenet/model.h>

namespace fxme::neural::detail
{
    /** Called by the loader; returns something that depends on every anchor
        so that none is optimised away. */
    bool namArchitecturesLinked() noexcept
    {
        static void* const volatile anchors[] {
            reinterpret_cast<void*> (&nam::lstm::create_config),
            reinterpret_cast<void*> (&nam::container::create_config),
            reinterpret_cast<void*> (&nam::wavenet::create_config),
        };
        return anchors[0] != nullptr && anchors[1] != nullptr && anchors[2] != nullptr;
    }
}
