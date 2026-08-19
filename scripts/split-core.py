#!/usr/bin/env python3
"""
split-core.py — move the JUCE-free half of FxmeTools into core/.

Run from the root of the FxmeTools repository (the directory holding
FxmeTools/, WDL/ and cmake/).

    python3 scripts/split-core.py                  # dry run: full report, no writes
    python3 scripts/split-core.py --diff           # dry run + unified diffs
    python3 scripts/split-core.py --apply          # do it, on a new git branch
    python3 scripts/split-core.py --promotable     # what the next batch would be

What it does
------------
1.  Classifies every source under FxmeTools/. A file is CORE when its *code*
    contains no JUCE reference — comments are ignored for the decision, then
    tidied, because a stale `juce::Foo` in a doc comment should not pin a file
    to the JUCE side (and would trip core's CheckNoJuce guard).
2.  `git mv`s the CORE files to core/FxmeTools/<same relative path>. The public
    include spelling is unchanged — <FxmeTools/dsp/Ambisonics.h> still resolves,
    because core/ becomes a second include root. FxmeFX and SuperMoTo need no
    edits at all.
3.  Fixes every include the move invalidates, in both directions:
      * shell file including a moved file relatively   ->  <FxmeTools/...>
      * moved file including a shell file relatively   ->  <FxmeTools/...>
      * moved file reaching outside FxmeTools/ (WDL)   ->  one more ../
4.  Drops the moved .cpp files from the module's unity TU (FxmeTools.cpp);
    core/CMakeLists.txt compiles them into FxmeCore instead.

Everything is computed in memory first, so --diff shows exactly what --apply
will write. There is no second code path.

What it deliberately does NOT do
--------------------------------
Files whose only JUCE contact is a helper with a drop-in core equivalent
(jlimit, Decibels, MathConstants, Random, dsp::ProcessSpec) are reported by
--promotable and left untouched. Those edits are one line per call site and one
of them changes behaviour — fxme::Random's sequence differs from JUCE's for a
given seed — so they want a human and a listen, not a regex.

SPDX-License-Identifier: LGPL-3.0-or-later
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

MODULE_DIR = Path("FxmeTools")
CORE_ROOT = Path("core")
CORE_DIR = CORE_ROOT / "FxmeTools"
SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".mm", ".c")

# The module's umbrella header and unity TU are the JUCE side by definition,
# whatever their contents look like.
NEVER_MOVE = {"FxmeTools.h", "FxmeTools.cpp", "FxmeTools.mm"}

INCLUDE_RE = re.compile(r'^(\s*#\s*include\s*)"([^"]+)"(.*)$')
# Must stay in step with core/cmake/CheckNoJuce.cmake, or the script moves a
# file the guard then rejects. JUCE_* and jassert matter because a macro leaves
# no juce:: token behind — a class whose only JUCE contact is
# JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR looks entirely JUCE-free here.
JUCE_ANY_RE = re.compile(
    r"juce::|JuceHeader\.h|#\s*include\s*<juce_|JUCE_[A-Z0-9_]+|jassert")
JUCE_TOKEN_RE = re.compile(r"juce::(?:dsp::)?[A-Za-z_]\w*(?:::[A-Za-z_]\w*)?")
COMMENT_MENTION_RE = re.compile(r"juce::")

PROMOTABLE = {
    "juce::jlimit": "fxme::jlimit                <FxmeTools/util/Math.h>",
    "juce::jmax": "fxme::jmax                  <FxmeTools/util/Math.h>",
    "juce::jmin": "fxme::jmin                  <FxmeTools/util/Math.h>",
    "juce::jmap": "fxme::jmap                  <FxmeTools/util/Math.h>",
    "juce::roundToInt": "fxme::roundToInt            <FxmeTools/util/Math.h>",
    "juce::nextPowerOfTwo": "fxme::nextPowerOfTwo        <FxmeTools/util/Math.h>",
    "juce::isPositiveAndBelow": "fxme::isPositiveAndBelow    <FxmeTools/util/Math.h>",
    "juce::degreesToRadians": "fxme::degreesToRadians      <FxmeTools/util/Math.h>",
    "juce::MathConstants": "fxme::MathConstants         <FxmeTools/util/Math.h>",
    "juce::Decibels": "fxme::Decibels              <FxmeTools/util/Math.h>",
    "juce::Random": "fxme::Random                <FxmeTools/util/Random.h>   [!] different sequence per seed",
    "juce::dsp::ProcessSpec": "fxme::ProcessSpec           <FxmeTools/util/ProcessSpec.h>",
    "juce::int64": "std::int64_t                <cstdint>",
    "juce::uint32": "std::uint32_t               <cstdint>",
    "juce::uint8": "std::uint8_t                <cstdint>",
    "juce::AudioBuffer": "fxme::AudioBufferView       <FxmeTools/util/AudioBufferView.h>  [!] signature change",
}


# --------------------------------------------------------------------------
# helpers


def strip_comments(text: str) -> str:
    """Blanks out // and /* */ comments and string literals, keeping offsets sane."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(" " * (j - i))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def tidy_juce_comments(text: str) -> tuple[str, int]:
    """Rewrites `juce::Foo` -> `JUCE's Foo` inside comments only. Returns (text, count).

    strip_comments blanks comments while preserving offsets, so a match whose
    span is blank in the stripped copy is exactly a match inside a comment.
    """
    code = strip_comments(text)
    pieces: list[str] = []
    last = 0
    count = 0

    for m in COMMENT_MENTION_RE.finditer(text):
        s, e = m.span()
        if code[s:e].strip() != "":
            continue                     # real code — leave it alone
        pieces.append(text[last:s])
        pieces.append("JUCE's ")
        last = e
        count += 1

    if count == 0:
        return text, 0

    pieces.append(text[last:])
    return "".join(pieces), count


def promotable_target(symbol: str) -> str | None:
    """Longest-prefix lookup, so juce::Decibels::gainToDecibels matches juce::Decibels."""
    if symbol in PROMOTABLE:
        return PROMOTABLE[symbol]
    for key in sorted(PROMOTABLE, key=len, reverse=True):
        if symbol.startswith(key + "::"):
            return PROMOTABLE[key]
    return None


def module_id(path: Path) -> str:
    """Path relative to whichever root it currently lives under, as a posix string."""
    for base in (CORE_DIR, MODULE_DIR):
        try:
            return path.relative_to(base).as_posix()
        except ValueError:
            continue
    raise ValueError(path)


def collect(root: Path) -> list[Path]:
    out: list[Path] = []
    for dirpath, _d, filenames in os.walk(root):
        for name in filenames:
            if name.endswith(SOURCE_SUFFIXES):
                out.append(Path(dirpath) / name)
    return sorted(out)


def resolve_id(source_id: str, spec: str) -> str:
    """Resolve an #include "spec" from a file identified by source_id."""
    return PurePosixPath(os.path.normpath(
        str(PurePosixPath(source_id).parent / spec))).as_posix()


# --------------------------------------------------------------------------


def classify(files: list[Path]) -> tuple[dict[str, str], dict[str, set[str]]]:
    """-> (id -> 'core'|'shell', id -> the juce:: symbols used in actual code)"""
    where: dict[str, str] = {}
    symbols: dict[str, set[str]] = {}

    for f in files:
        ident = module_id(f)
        code = strip_comments(f.read_text(encoding="utf-8", errors="replace"))

        symbols[ident] = set(JUCE_TOKEN_RE.findall(code))
        is_core = (JUCE_ANY_RE.search(code) is None) and ident not in NEVER_MOVE
        where[ident] = "core" if is_core else "shell"

    return where, symbols


def block_unmovable(where: dict[str, str], contents: dict[str, str]) -> dict[str, str]:
    """A core file that includes a shell file cannot move yet."""
    blocked: dict[str, str] = {}
    changed = True
    while changed:
        changed = False
        for ident, loc in list(where.items()):
            if loc != "core" or ident in blocked:
                continue
            for line in contents[ident].splitlines():
                m = INCLUDE_RE.match(line)
                if not m:
                    continue
                target = resolve_id(ident, m.group(2))
                if target.startswith(".."):
                    continue  # outside the module tree; handled by depth fix
                if target not in where:
                    continue  # not one of ours
                if where[target] == "shell" or target in blocked:
                    blocked[ident] = f'includes "{m.group(2)}" which stays on the JUCE side'
                    changed = True
                    break
    return blocked


def transform(ident: str, text: str, in_core: bool, relocating: bool,
              where: dict[str, str]) -> tuple[str, list[str]]:
    """Returns the file's post-split content plus a log of what changed.

    in_core     — where the file ends up (drives include spelling)
    relocating  — whether it is physically moving in this run (drives ../ depth)
    """
    log: list[str] = []
    lines = text.splitlines(keepends=True)
    out: list[str] = []

    drop_cpp = ident == "FxmeTools.cpp"

    for line in lines:
        m = INCLUDE_RE.match(line.rstrip("\n"))
        if not m:
            out.append(line)
            continue

        prefix, spec, suffix = m.group(1), m.group(2), m.group(3)
        nl = "\n" if line.endswith("\n") else ""
        target = resolve_id(ident, spec)

        if drop_cpp and where.get(target) == "core" and target.endswith(".cpp"):
            log.append(f'dropped #include "{spec}" (now compiled into FxmeCore)')
            continue

        if target.startswith(".."):
            # Outside FxmeTools/ — WDL and friends. One directory deeper now.
            if relocating:
                fixed = "../" + spec
                out.append(f'{prefix}"{fixed}"{suffix}{nl}')
                log.append(f'"{spec}" -> "{fixed}"')
            else:
                out.append(line)
            continue

        if target not in where:
            out.append(line)
            continue

        target_in_core = where[target] == "core"
        if in_core == target_in_core:
            out.append(line)          # both sides land in the same tree
            continue

        angle = f"<FxmeTools/{target}>"
        out.append(f"{prefix}{angle}{suffix}{nl}")
        log.append(f'"{spec}" -> {angle}')

    result = "".join(out)

    if relocating:
        result, tidied = tidy_juce_comments(result)
        if tidied:
            log.append(f"tidied {tidied} JUCE mention(s) in comments "
                       f"(so core/'s CheckNoJuce guard stays strict)")

    return result, log


# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="write changes (default: dry run)")
    ap.add_argument("--diff", action="store_true", help="show unified diffs")
    ap.add_argument("--promotable", action="store_true",
                    help="list shell files whose JUCE use has a core equivalent")
    ap.add_argument("--branch", default="core-split", help="branch to create with --apply")
    ap.add_argument("--no-branch", action="store_true", help="apply on the current branch")
    args = ap.parse_args()

    if not MODULE_DIR.is_dir():
        print(f"error: {MODULE_DIR}/ not found — run from the FxmeTools repo root", file=sys.stderr)
        return 2

    if args.apply:
        # Checked before anything is printed: a `git mv` spree onto an unclean
        # tree is not something to discover at the bottom of a long report.
        status = subprocess.run(["git", "status", "--porcelain"],
                                capture_output=True, text=True, check=True)
        dirty = [ln for ln in status.stdout.splitlines() if ln.strip()]
        if dirty:
            print("error: working tree is not clean — commit, stash or ignore these first:",
                  file=sys.stderr)
            for ln in dirty[:20]:
                print(f"  {ln}", file=sys.stderr)
            if len(dirty) > 20:
                print(f"  ... and {len(dirty) - 20} more", file=sys.stderr)
            print("\n(build directories belong in .gitignore.)", file=sys.stderr)
            return 2

    module_files = collect(MODULE_DIR)
    core_files = collect(CORE_DIR) if CORE_DIR.is_dir() else []
    files = module_files + core_files
    paths = {module_id(f): f for f in files}
    # Files already living under core/ (the util layer) must not be "moved"
    # again, but they still count as the core side when resolving includes.
    already_core = {module_id(f) for f in core_files}
    contents = {i: p.read_text(encoding="utf-8", errors="replace") for i, p in paths.items()}

    where, symbols = classify(files)
    for ident in already_core:
        where[ident] = "core"          # by construction

    blocked = block_unmovable(where, contents)
    for ident in blocked:
        where[ident] = "shell"         # stays put this run
    to_move = {i for i, loc in where.items()
               if loc == "core" and i not in already_core}

    print(f"FxmeTools — {len(files)} sources")
    print(f"  JUCE-free (code)   : {sum(1 for v in where.values() if v == 'core')}")
    print(f"  movable now        : {len(to_move)}")
    print(f"  blocked            : {len(blocked)}")
    print(f"  JUCE-coupled       : {sum(1 for v in where.values() if v == 'shell')}")
    print()

    if args.promotable:
        rows = [(i, s) for i, s in sorted(symbols.items())
                if where[i] == "shell" and s and all(promotable_target(x) for x in s)]
        print(f"Promotable with the core util layer — {len(rows)} file(s):\n")
        for ident, syms in rows:
            print(f"  {ident}")
            for s in sorted(syms):
                print(f"      {s:<26} -> {promotable_target(s)}")
            print()
        rest = [(i, s) for i, s in sorted(symbols.items())
                if where[i] == "shell" and s and not all(promotable_target(x) for x in s)]
        print(f"Needs design work — {len(rest)} file(s). Blocking symbols:\n")
        for ident, syms in rest:
            hard = sorted(x for x in syms if not promotable_target(x))
            print(f"  {ident:<44} {', '.join(hard[:6])}{' …' if len(hard) > 6 else ''}")
        return 0

    if blocked:
        print("Blocked — promote the dependency first:")
        for ident, why in sorted(blocked.items()):
            print(f"  {ident}: {why}")
        print()

    print(f"Moving {len(to_move)} file(s) to {CORE_DIR}/:")
    for ident in sorted(to_move):
        print(f"  {ident}")
    print()

    # ---- compute every file's new content, once -----------------------------
    new_contents: dict[str, str] = {}
    logs: dict[str, list[str]] = {}
    for ident, text in contents.items():
        result, log = transform(ident, text,
                                where[ident] == "core", ident in to_move, where)
        if result != text:
            new_contents[ident] = result
        if log:
            logs[ident] = log

    if logs:
        print("Edits:")
        for ident in sorted(logs):
            print(f"  {ident}")
            for entry in logs[ident]:
                print(f"      {entry}")
        print()

    if args.diff:
        for ident in sorted(new_contents):
            diff = difflib.unified_diff(
                contents[ident].splitlines(keepends=True),
                new_contents[ident].splitlines(keepends=True),
                fromfile=f"a/{paths[ident]}", tofile=f"b/{paths[ident]}")
            sys.stdout.writelines(diff)
        print()

    if not args.apply:
        print("(dry run — nothing written. Re-run with --apply.)")
        return 0

    # ---- apply --------------------------------------------------------------
    if not args.no_branch:
        subprocess.run(["git", "checkout", "-b", args.branch], check=True)

    for ident in sorted(to_move):
        src = paths[ident]
        dst = CORE_DIR / ident
        dst.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "mv", str(src), str(dst)], check=True)
        paths[ident] = dst

    for ident, text in new_contents.items():
        paths[ident].write_text(text, encoding="utf-8")

    print(f"Moved {len(to_move)} file(s), rewrote {len(new_contents)} file(s).\n")
    print("Next steps:")
    print("  1. Wire core/ into cmake/FxmeTools.cmake — see core/README.md.")
    print("  2. cmake -S core -B build-core && cmake --build build-core \\")
    print("       && ctest --test-dir build-core       # builds with no JUCE at all")
    print("  3. Rebuild SuperMoTo / FxmeFX — consumer includes are unchanged.")
    print("  4. python3 scripts/split-core.py --promotable   # plan the next batch")
    return 0


if __name__ == "__main__":
    sys.exit(main())
