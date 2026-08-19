/*
  ------------------------------------------------------------------------------
    util/CoreVersion.cpp

    The one guaranteed translation unit of FxmeCore. Its practical job is to
    give the static library a source file even before any .cpp has migrated
    into core/, so the target configures and builds from day one; its useful
    job is to let a host confirm which core it linked against.

    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/util/CoreVersion.h>

namespace fxme
{

const char* coreVersionString() noexcept
{
    return FXME_CORE_VERSION_STRING;
}

} // namespace fxme
