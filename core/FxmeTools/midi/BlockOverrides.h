/*
  ------------------------------------------------------------------------------
    BlockOverrides.h

    The block override mini-language (born in Mango): a sequencer block's
    string is a space-separated list of `key=value` assignments that locally
    override the owner's parameters for that block, e.g.

        dur=0.125 fb=0.6        dur=mididur/2        v0=a v1=u

    Generic: the caller describes its language (OverrideLanguage) — the keys
    it accepts, optionally with named symbols per key (Mango's vowels), and
    the named variables a value may refer to (`mididur`, `midifreq`, or a
    host's own: a chord's root frequency, a bar's length…), each with an
    optional voice digit 1..maxVoice (`mididur2`). The values of the
    variables are supplied at evaluation time by any object with a
    `float variableValue (int variable, int voice) const` member, so the same
    parsed string can resolve against MIDI notes in one host and against a
    harmony in another.

    Values: a number; a variable, optionally scaled (`var*N`, `var/N`,
    `N*var`); a note-value constant `s4 s8 s16 s32 s64` (straight), `t…`
    (triplet), `d…` (dotted) — the fraction of a whole note the
    NoteDuration grid gives, foldable the same way (`t8*2`); a number with a
    trailing `s`, an absolute time in seconds (`1.5s`); or one of the key's
    symbols. What a plain number means (a whole-note fraction for a duration,
    a frequency in Hz…) is the owner's business: Kind tells it how the value
    was written.

    Strict and all-or-nothing: an unknown key, a malformed value, a variable
    digit out of range or more than kMaxEntries assignments -> std::nullopt.
    An empty (or all-whitespace) string is a valid empty set. Parse on a
    non-real-time thread; the result is a flat POD the audio thread reads
    and evaluates without locks or allocation.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <FxmeTools/midi/NoteDuration.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace fxme
{

/** A name a key accepts in place of a number (Mango's vowels: `v0=a`). */
struct OverrideSymbol
{
    const char* name = nullptr;
    float value = 0.0f;
};

/** One key of a language, and the symbols it accepts (none: numbers only). */
struct OverrideKey
{
    const char* name = nullptr;
    const OverrideSymbol* symbols = nullptr;
    int numSymbols = 0;
};

/** A variable a value may name; `maxVoice` > 0 allows a digit 1..maxVoice after it. */
struct OverrideVariable
{
    const char* name = nullptr;
    int maxVoice = 0;
};

/** What a host's strings may say. Plain arrays: a language is usually static. */
struct OverrideLanguage
{
    const OverrideKey* keys = nullptr;
    int numKeys = 0;
    const OverrideVariable* variables = nullptr;
    int numVariables = 0;
};

/** One value: a constant, an absolute-seconds constant (`1.5s`), or
    `value` times a variable (division by N is folded into `value`).
    `voice` is the variable's digit, 0 without one. */
struct OverrideExpr
{
    enum Kind { Const, SecondsConst, Variable };
    Kind  kind     = Const;
    float value    = 0.0f;
    int   variable = -1;
    int   voice    = 0;

    /** `source.variableValue (variable, voice)` supplies the variables. */
    template <typename Source>
    float eval (const Source& source) const
    {
        if (kind != Variable)
            return value;
        return value * source.variableValue (variable, voice);
    }
};

/** A parsed string: up to kMaxEntries assignments, keyed by the language's key index. */
struct ParsedOverrides
{
    static constexpr int kMaxEntries = 16;

    struct Entry
    {
        int key = -1;
        OverrideExpr expr;
    };

    Entry entries[kMaxEntries];
    int   count = 0;

    const OverrideExpr* find (int key) const
    {
        for (int i = 0; i < count; ++i)
            if (entries[i].key == key)
                return &entries[i].expr;
        return nullptr;
    }

    /** For hosts keeping their keys in an enum. */
    template <typename Key>
    const OverrideExpr* find (Key key) const { return find (static_cast<int> (key)); }

    bool add (int key, OverrideExpr e)
    {
        if (count >= kMaxEntries)
            return false;
        entries[count++] = { key, e };
        return true;
    }

    template <typename Key>
    bool add (Key key, OverrideExpr e) { return add (static_cast<int> (key), e); }
};

namespace overrides_detail
{
    /** Strict float: the whole string must be consumed. */
    inline std::optional<float> parseNumber (const std::string& s)
    {
        if (s.empty())
            return std::nullopt;
        char* end = nullptr;
        const float v = std::strtof (s.c_str(), &end);
        if (end != s.c_str() + s.size())
            return std::nullopt;
        return v;
    }

    /** A variable at `pos`: the longest name that matches, plus its optional digit. */
    struct VariableRef
    {
        int variable = -1;
        int voice = 0;
        std::size_t len = 0;
    };

    inline std::optional<VariableRef> parseVariable (const std::string& s, std::size_t pos, const OverrideLanguage& lang)
    {
        VariableRef best;
        for (int v = 0; v < lang.numVariables; ++v)
        {
            const auto& var = lang.variables[v];
            const std::size_t n = std::strlen (var.name);
            if (n > best.len && s.compare (pos, n, var.name) == 0)
                best = { v, 0, n };
        }
        if (best.variable < 0)
            return std::nullopt;

        // An out-of-range digit is left unconsumed rather than clamped, so
        // `mididur5` fails to parse instead of quietly meaning something else.
        const int maxVoice = lang.variables[best.variable].maxVoice;
        if (pos + best.len < s.size())
        {
            const char c = s[pos + best.len];
            if (maxVoice > 0 && c >= '1' && c <= '9' && c - '0' <= maxVoice)
            {
                best.voice = c - '0';
                ++best.len;
            }
        }
        return best;
    }

    /** A note-value constant at `pos` (`s4`…`d64`): its whole-note fraction and length. */
    struct NoteConstRef
    {
        float value = 0.0f;
        std::size_t len = 0;
    };

    inline std::optional<NoteConstRef> parseNoteConst (const std::string& s, std::size_t pos)
    {
        static const char* const names[3][kNumNoteBases] = {
            { "s4", "s8", "s16", "s32", "s64" },
            { "t4", "t8", "t16", "t32", "t64" },
            { "d4", "d8", "d16", "d32", "d64" },
        };
        NoteConstRef best;
        for (int m = 0; m < kNumNoteMods; ++m)
            for (int b = 0; b < kNumNoteBases; ++b)
            {
                const std::size_t n = std::strlen (names[m][b]);
                if (n > best.len && s.compare (pos, n, names[m][b]) == 0)
                    best = { (float) noteDurationBeats ((NoteBase) b, (NoteMod) m) * 0.25f, n };
            }
        if (best.len == 0)
            return std::nullopt;
        return best;
    }

    /** `ref`, `ref*N` or `ref/N` filling the whole of `s`; `make` builds the
        expression from the scale. N must be a number, and not 0 for a division. */
    template <typename Make>
    std::optional<OverrideExpr> scaledFromFront (const std::string& s, std::size_t len, Make make)
    {
        if (len == s.size())
            return make (1.0f);
        if (s.size() > len + 1)
        {
            const char op = s[len];
            const auto n = parseNumber (s.substr (len + 1));
            if (! n)
                return std::nullopt;
            if (op == '*')
                return make (*n);
            if (op == '/' && (*n > 0.0f || *n < 0.0f))
                return make (1.0f / *n);
        }
        return std::nullopt;
    }

    inline std::optional<OverrideExpr> parseExpr (const std::string& s, const OverrideKey& key, const OverrideLanguage& lang)
    {
        for (int i = 0; i < key.numSymbols; ++i)
            if (s == key.symbols[i].name)
                return OverrideExpr { OverrideExpr::Const, key.symbols[i].value, -1, 0 };

        // var[digit], var*N, var/N
        if (const auto v = parseVariable (s, 0, lang))
            return scaledFromFront (s, v->len, [&] (float scale) { return OverrideExpr { OverrideExpr::Variable, scale, v->variable, v->voice }; });

        // N*var
        if (const auto star = s.find ('*'); star != std::string::npos)
            if (const auto v = parseVariable (s, star + 1, lang); v && v->len == s.size() - star - 1)
            {
                const auto n = parseNumber (s.substr (0, star));
                if (! n)
                    return std::nullopt;
                return OverrideExpr { OverrideExpr::Variable, *n, v->variable, v->voice };
            }

        // Note-value constants fold to a constant: they depend on nothing at run time.
        if (const auto c = parseNoteConst (s, 0))
            return scaledFromFront (s, c->len, [&] (float scale) { return OverrideExpr { OverrideExpr::Const, c->value * scale, -1, 0 }; });

        if (const auto star = s.find ('*'); star != std::string::npos)
            if (const auto c = parseNoteConst (s, star + 1); c && c->len == s.size() - star - 1)
            {
                const auto n = parseNumber (s.substr (0, star));
                if (! n)
                    return std::nullopt;
                return OverrideExpr { OverrideExpr::Const, *n * c->value, -1, 0 };
            }

        // N s: absolute seconds. strtof never takes a trailing 's', so no clash with a number.
        if (s.size() > 1 && s.back() == 's')
            if (const auto n = parseNumber (s.substr (0, s.size() - 1)))
                return OverrideExpr { OverrideExpr::SecondsConst, *n, -1, 0 };

        if (const auto n = parseNumber (s))
            return OverrideExpr { OverrideExpr::Const, *n, -1, 0 };
        return std::nullopt;
    }
} // namespace overrides_detail

/** The index of `name` among the language's keys, or -1. */
inline int overrideKeyIndex (const OverrideLanguage& lang, const std::string& name)
{
    for (int i = 0; i < lang.numKeys; ++i)
        if (name == lang.keys[i].name)
            return i;
    return -1;
}

/** Parses a block's string. std::nullopt on any error; whitespace alone is a valid empty set. */
inline std::optional<ParsedOverrides> parseOverrides (const std::string& text, const OverrideLanguage& lang)
{
    ParsedOverrides result;
    std::size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n'))
            ++i;
        if (i >= text.size())
            break;

        std::size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\t' && text[j] != '\n')
            ++j;
        const std::string token = text.substr (i, j - i);
        i = j;

        const auto eq = token.find ('=');
        if (eq == std::string::npos || eq == 0 || eq == token.size() - 1)
            return std::nullopt;

        const int key = overrideKeyIndex (lang, token.substr (0, eq));
        if (key < 0)
            return std::nullopt;

        const auto expr = overrides_detail::parseExpr (token.substr (eq + 1), lang.keys[key], lang);
        if (! expr || ! result.add (key, *expr))
            return std::nullopt;
    }
    return result;
}

} // namespace fxme
