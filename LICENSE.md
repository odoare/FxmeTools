# Licensing

FxmeTools is two halves under two different licences. Which one applies to a
given file is stated in that file's own header as an SPDX identifier; this
document explains why the split exists and what each half means for a consumer.

```
core/        LGPL-3.0-or-later
FxmeTools/   AGPL-3.0-or-later, or commercial terms from the author
WDL/         zlib (a separate submodule, Cockos Incorporated)
```

## core/ — LGPL-3.0-or-later

Everything under `core/` is framework-free. It does not include a JUCE header,
link a JUCE library, or name a JUCE symbol, and a build-time guard
(`FxmeCoreGuard`) fails the build if that ever stops being true, comments
included. It builds and tests with a bare C++17 toolchain and no JUCE checkout.

It is therefore not a derivative of JUCE in any sense, and its licence is
independent of the framework's. This is the practical point of the whole
core/shell separation: the DSP, music theory, acoustics and maths can be used
from a non-JUCE project, a command-line tool or an embedded target under the
LGPL alone.

    SPDX-License-Identifier: LGPL-3.0-or-later

Full text in `core/LICENSE`. LGPLv3 is written as a set of additional
permissions on top of GPLv3, so `core/LICENSE.GPL` carries the GPLv3 text it
incorporates; the two are read together.

## FxmeTools/ — AGPL-3.0-or-later, or commercial

The `FxmeTools/` directory is a JUCE module. It only compiles against JUCE, and
a work combining the two is bound by JUCE's own terms.

JUCE 8 is itself dual-licensed: under the AGPLv3, or under the commercial JUCE 8
licence. This half mirrors that shape rather than fighting it, so that whichever
route a consumer is already on for JUCE, the same route works here:

    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial

- **Under the AGPLv3** (full text in `FxmeTools/LICENSE`): use it freely,
  including over a network, provided the combined work is also released under
  the AGPLv3 and its complete source is offered to users. This pairs with using
  JUCE under its own AGPLv3 option.
- **Under commercial terms**: available from the author for consumers who hold a
  commercial JUCE licence and do not wish to release their plugin under the
  AGPL. `LicenseRef-FXME-Commercial` refers to those terms. Contact via
  [github.com/odoare](https://github.com/odoare) or www.fx-mechanics.com.

Note that a commercial grant here covers *this* code only. It is not a JUCE
licence and does not substitute for one; JUCE must be licensed separately from
Raw Material Software under whichever of its two options applies.

## What this means for a consuming plugin

A plugin links both halves, so the JUCE half's terms govern what it may be
distributed under. Distributing a plugin built on `FxmeTools/` means either
releasing it under the AGPLv3 with sources, or holding both a commercial JUCE
licence and commercial terms for this module.

A tool that links **only** `FxmeCore` — a headless test, an offline renderer, a
Pure Data external, a console utility — takes on no JUCE obligation at all and
needs only the LGPL. That is the case the split was built to make possible.

## WDL

`WDL/` is a git submodule from Cockos Incorporated under the zlib licence, with
its own terms. It supplies the convolution and reverb engines. It is unaffected
by any of the above.

---

Copyright (c) 2023-2026 Olivier Doaré.
