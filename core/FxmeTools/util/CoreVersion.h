/*
  ------------------------------------------------------------------------------
    util/CoreVersion.h

    Version of the JUCE-free half of FxmeTools.

    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#define FXME_CORE_VERSION_MAJOR 0
#define FXME_CORE_VERSION_MINOR 1
#define FXME_CORE_VERSION_PATCH 0
#define FXME_CORE_VERSION_STRING "0.1.0"

namespace fxme
{

/** Returns the FxmeCore version the caller was linked against. */
const char* coreVersionString() noexcept;

} // namespace fxme
