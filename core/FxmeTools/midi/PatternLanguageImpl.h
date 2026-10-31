/*
  ------------------------------------------------------------------------------
    PatternLanguageImpl.h

    The compiler and the realizer behind PatternLanguage.h (included at its
    end). Header-only, C++17, no dependency. Read PatternLanguage.h for the API
    and Otomatotamoto's doc/pattern-language.md for the grammar.

    Shape of the code
    -----------------
    - detail::Parser turns the text into a tree of Items. An Item is the
      prefixes written before a "core" (a step, a chord, a group, a variation,
      a generator, a double quote) and the core itself. Prefixes that end a
      sequence become an Item with no core.
    - detail::Measurer walks the tree once to know the pattern's length, its
      shortest step and upper bounds on events, notes and steps (Info), and to
      check that probability branches last the same (syntax 3).
    - detail::Realizer walks the tree for one pass with a cursor of musical
      state. Like TeAr's engine, a prefix only sets "pending" local state,
      which the next step consumes: that is what makes `o+ (1 2)` act on the
      first step only, and what lets prefixes that end a pass reach the first
      step of the next one in syntax 2 (through Carry).

    Draws
    -----
    Every draw of a pass takes the next k (0, 1, 2...) of the pass, in the order
    the realizer meets them, and its value is draw (seed, k). The order is the
    text's, depth first:
      - a prefix `pN` rolls when the prefix is met (syntax 3: `p0` draws nothing; `p'expr'`: one k for the
        expression); a probability with no digit draws nothing;
      - `o?` and `v?` draw when their prefix is met;
      - `?` draws when its step is met (after its prefixes);
      - a generator draws its ranges first, in the order k, n, r (euclid) or
        n, value (binary), only for those that really are ranges, then its
        hits roll their own `p` prefixes in turn;
      - an expression `'...'` takes one k and is given mix (seed ^ mix (k)) as
        its own seed; a plain number between quotes takes none;
      - a branch that does not play (the other side of a probability, the
        parts of a variation not chosen) draws nothing; a group silenced by a
        failed probability (syntax 3) is still walked, so it draws as if played.

    Author: Olivier Doaré, github.com/odoare
    Licenced under the GNU LGPL Version 3.0
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include "PatternLanguage.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <utility>

namespace fxme::pattern
{

//==============================================================================
// Public helpers

inline std::uint64_t mix (std::uint64_t x) noexcept
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline double draw (std::uint64_t seed, std::uint64_t k) noexcept
{
    const std::uint64_t h = mix (seed ^ mix (k));
    return static_cast<double> (h >> 11) * (1.0 / 9007199254740992.0);   // 53 bits
}

inline int velocityForLevel (int level) noexcept
{
    const long v = std::lround (static_cast<float> (level) * 127.0f / 15.0f);
    return static_cast<int> (std::min (127L, std::max (1L, v)));
}

namespace detail
{

constexpr int kMaxDepth = 32;
constexpr long long kMaxSteps = 16384;     // upper bound of a pass, repeats and generators counted
constexpr long long kSaturate = 1000000000;

inline std::size_t uz (int i) noexcept { return static_cast<std::size_t> (i); }

inline int hexValue (char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

inline int levelForVelocity (float midi) noexcept
{
    const long v = std::lround (midi * 15.0f / 127.0f);
    return static_cast<int> (std::min (15L, std::max (1L, v)));
}

//==============================================================================
// The tree

struct Value
{
    enum class Kind { None, Number, Plus, Minus, Random, Expr, Whole };
    Kind kind = Kind::None;
    int number = 0;                    // Number: the digit, the level, the tenths
    double constant = 0.0;             // Expr without a host: the plain number
    int expr = -1;                     // Expr with a host: its id; -1: the constant
};

struct Prefix
{
    char type = 0;                     // o O v V p g G m # b ~
    Value value;
    double beats = 0.0;                // m
    int pos = 0;
};

enum class Core { None, Step, Chord, Group, Variation, Generator, Quote };
enum class Cmd { Degree, Same, Next, Prev, Random, Rest, Hold, Hit };

inline bool isPitchedCmd (Cmd c) noexcept
{
    return c == Cmd::Degree || c == Cmd::Same || c == Cmd::Next || c == Cmd::Prev || c == Cmd::Random;
}

struct Range
{
    int lo = 0, hi = 0;
    bool isRange() const noexcept { return hi != lo; }
};

struct GenSpec
{
    bool euclid = true;
    Range hits, steps, rot;            // euclid: k / n / r; binary: steps is n
    bool valueGiven = false;           // binary: lo-hi given
    long long vlo = 0, vhi = 0;
};

struct ChordNote
{
    std::vector<Prefix> prefixes;      // o # b
    Cmd cmd = Cmd::Degree;
    int degree = 0;
};

struct Item
{
    std::vector<Prefix> prefixes;
    Core core = Core::None;
    int start = 0, end = 0;            // characters, prefixes included
    int probPos = 0;                   // the `p` that makes a branch
    Cmd cmd = Cmd::Rest;               // Step
    int degree = 0;                    // Step: 0-based
    std::vector<ChordNote> chord;      // Chord
    std::vector<Item> children;        // Group
    int repeat = 1;                    // Group
    std::vector<std::vector<Item>> parts;   // Variation
    GenSpec gen;                       // Generator
    std::vector<Item> what;            // Generator: what the hits play (one item)
    std::vector<Item> alt;             // the other branch of a probability (a step or a group)
};

struct Compiled
{
    std::string text;
    Options options;
    std::vector<Diagnostic> diagnostics;
    Info info;
    bool ok = true;
    std::vector<Item> items;
    bool pitched = false, multi = false, glides = false;
};

//==============================================================================
// Parser

class Parser
{
public:
    explicit Parser (Compiled& c)
        : out (c), text (c.text), syn3 (c.options.syntax >= 3) {}

    void run()
    {
        parseSequence (out.items, 0, 0);
        if (syn3 && quoteOpen >= 0)
            error (quoteOpen, "unbalanced '\"'");
    }

private:
    Compiled& out;
    const std::string& text;
    const bool syn3;
    int at = 0;
    int depth = 0;
    int quoteOpen = -1;
    bool seen = false;                 // a token other than a `let` line was read

    int size() const noexcept { return static_cast<int> (text.size()); }
    bool atEnd() const noexcept { return at >= size(); }
    char peek() const noexcept { return atEnd() ? '\0' : text[uz (at)]; }

    void error (int pos, std::string message)
    {
        out.diagnostics.push_back ({ pos, std::move (message), false });
    }
    void warn (int pos, std::string message)
    {
        out.diagnostics.push_back ({ pos, std::move (message), true });
    }

    static bool isSpace (char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool isDigit (char c) noexcept { return c >= '0' && c <= '9'; }

    void skipBlank()
    {
        while (! atEnd())
        {
            const char c = text[uz (at)];
            if (isSpace (c))
                ++at;
            else if (syn3 && c == ';')
                while (! atEnd() && text[uz (at)] != '\n') ++at;
            else
                break;
        }
    }

    bool isLetLine() const
    {
        if (text.compare (uz (at), 3, "let") != 0) return false;
        if (at + 3 >= size() || (text[uz (at + 3)] != ' ' && text[uz (at + 3)] != '\t')) return false;
        for (int i = at - 1; i >= 0 && text[uz (i)] != '\n'; --i)
            if (text[uz (i)] != ' ' && text[uz (i)] != '\t' && text[uz (i)] != '\r')
                return false;
        return true;
    }

    bool isPrefixStart (char c) const noexcept
    {
        if (c == 'o' || c == 'O' || c == 'v' || c == 'V' || c == 'p' || c == '#' || c == 'b')
            return true;
        return syn3 && (c == 'g' || c == 'G' || c == 'm' || c == '~');
    }

    bool isStepStart (char c) const noexcept
    {
        if (hexValue (c) >= 0 || c == '+' || c == '-' || c == '?' || c == '=' || c == '.' || c == '_')
            return true;
        return syn3 && c == 'x';
    }

    bool isCoreStart (char c) const noexcept
    {
        if (isStepStart (c) || c == '(' || c == '"') return true;
        return syn3 && (c == '{' || c == '[' || c == 'e' || c == 'n');
    }

    //--------------------------------------------------------------------------
    // Values after a prefix

    bool parseExpression (Value& v)
    {
        const int q = at;
        const std::size_t close = text.find ('\'', uz (at + 1));
        if (close == std::string::npos)
        {
            error (q, "missing closing quote");
            at = size();
            return false;
        }
        const std::string body = text.substr (uz (at + 1), close - uz (at + 1));
        at = static_cast<int> (close) + 1;

        if (out.options.expressions != nullptr)
        {
            std::string why;
            const int id = out.options.expressions->compile (body, why);
            if (id < 0)
            {
                error (q, "in '" + body + "': " + why);
                return false;
            }
            v.kind = Value::Kind::Expr;
            v.expr = id;
            return true;
        }

        // No host: a plain number only.
        bool plain = ! body.empty();
        for (char ch : body)
            if (! (isDigit (ch) || ch == '.' || ch == 'e' || ch == 'E' || ch == '-' || ch == '+'))
                plain = false;
        char* endp = nullptr;
        double d = 0.0;
        if (plain)
        {
            d = std::strtod (body.c_str(), &endp);
            plain = endp != nullptr && *endp == '\0';
        }
        if (! plain)
        {
            error (q, "in '" + body + "': expressions need a host");
            return false;
        }
        v.kind = Value::Kind::Expr;
        v.constant = d;
        v.expr = -1;
        return true;
    }

    bool parseNoteValue (Prefix& pf)
    {
        long long n = 0;
        int digits = 0;
        while (isDigit (peek()) && digits < 4)
        {
            n = n * 10 + (peek() - '0');
            ++at;
            ++digits;
        }
        bool triplet = false;
        if (peek() == 't') { triplet = true; ++at; }
        if (digits == 0 || (n != 1 && n != 2 && n != 4 && n != 8 && n != 16 && n != 32 && n != 64))
        {
            error (pf.pos, "bad note value after 'm'");
            return false;
        }
        pf.beats = 4.0 / static_cast<double> (n) * (triplet ? 2.0 / 3.0 : 1.0);
        return true;
    }

    bool parseValuePrefix (Prefix& pf, std::vector<Prefix>& pre)
    {
        const char t = pf.type;
        if (! syn3)
        {
            // Syntax 2 reads the next character, whatever it is.
            if (atEnd()) return false;
            const char v = text[uz (at)];
            ++at;
            Value& val = pf.value;
            if (t == 'o' || t == 'O')
            {
                if (v == '+') val.kind = Value::Kind::Plus;
                else if (v == '-') val.kind = Value::Kind::Minus;
                else if (v == '?') val.kind = Value::Kind::Random;
                else if (isDigit (v)) { val.kind = Value::Kind::Number; val.number = v - '0'; }
            }
            else if (t == 'v' || t == 'V')
            {
                if (v == '+') val.kind = Value::Kind::Plus;
                else if (v == '-') val.kind = Value::Kind::Minus;
                else if (v == '?') val.kind = Value::Kind::Random;
                else if (hexValue (v) > 0) { val.kind = Value::Kind::Number; val.number = hexValue (v); }
            }
            else if (isDigit (v))        // p
            {
                val.kind = Value::Kind::Number;
                val.number = v - '0';
            }
            pre.push_back (pf);
            return true;
        }

        const std::string name (1, t);
        if (atEnd() || isSpace (peek()))
        {
            error (pf.pos, "prefix '" + name + "' without a value");
            return false;
        }
        const char v = text[uz (at)];
        Value& val = pf.value;
        if (v == '\'')
        {
            if (! parseExpression (val)) return false;
            pre.push_back (pf);
            return true;
        }

        bool good = false;
        const char* what = "";
        if (t == 'o' || t == 'O')
        {
            what = "a value must be 0-7 here";
            if (v == '+') { val.kind = Value::Kind::Plus; good = true; }
            else if (v == '-') { val.kind = Value::Kind::Minus; good = true; }
            else if (v == '?') { val.kind = Value::Kind::Random; good = true; }
            else if (v >= '0' && v <= '7') { val.kind = Value::Kind::Number; val.number = v - '0'; good = true; }
        }
        else if (t == 'v' || t == 'V')
        {
            what = "a value must be 1-F here";
            if (v == '+') { val.kind = Value::Kind::Plus; good = true; }
            else if (v == '-') { val.kind = Value::Kind::Minus; good = true; }
            else if (v == '?') { val.kind = Value::Kind::Random; good = true; }
            else if (hexValue (v) >= 0)
            {
                good = true;
                if (hexValue (v) > 0) { val.kind = Value::Kind::Number; val.number = hexValue (v); }
            }
        }
        else if (t == 'p')
        {
            what = "a value must be 0-9 here";
            if (isDigit (v)) { val.kind = Value::Kind::Number; val.number = v - '0'; good = true; }
        }
        else   // g G
        {
            what = "a value must be 1-9 or x here";
            if (v == 'x') { val.kind = Value::Kind::Whole; good = true; }
            else if (v >= '1' && v <= '9') { val.kind = Value::Kind::Number; val.number = v - '0'; good = true; }
        }
        if (! good)
        {
            error (at, what);
            ++at;
            return false;
        }
        ++at;
        pre.push_back (pf);
        return true;
    }

    bool parsePrefix (std::vector<Prefix>& pre)
    {
        Prefix pf;
        pf.type = text[uz (at)];
        pf.pos = at;
        ++at;
        switch (pf.type)
        {
            case '#': case 'b':
                pre.push_back (pf);
                return true;
            case '~':
                out.glides = true;
                pre.push_back (pf);
                return true;
            case 'm':
                if (! parseNoteValue (pf)) return false;
                pre.push_back (pf);
                return true;
            default:
                return parseValuePrefix (pf, pre);
        }
    }

    //--------------------------------------------------------------------------
    // Cores

    void parseStep (Item& it, char c)
    {
        it.core = Core::Step;
        ++at;
        const int h = hexValue (c);
        if (h > 0) { it.cmd = Cmd::Degree; it.degree = h - 1; }
        else if (c == '0' || c == '=') it.cmd = Cmd::Same;
        else if (c == '+') it.cmd = Cmd::Next;
        else if (c == '-') it.cmd = Cmd::Prev;
        else if (c == '?') it.cmd = Cmd::Random;
        else if (c == '.') it.cmd = Cmd::Rest;
        else if (c == '_') it.cmd = Cmd::Hold;
        else it.cmd = Cmd::Hit;        // x
        if (isPitchedCmd (it.cmd)) out.pitched = true;
        it.end = at;
    }

    void parseGroup (Item& it, bool allowRepeat)
    {
        const int open = at;
        if (depth >= kMaxDepth)
        {
            error (at, "too deeply nested");
            ++at;
            return;
        }
        ++at;
        ++depth;
        it.core = Core::Group;
        parseSequence (it.children, ')', open);
        --depth;
        if (peek() == ')')
            ++at;
        else if (syn3)
            error (open, "missing ')'");
        it.end = at;

        if (allowRepeat && syn3)
        {
            const int save = at;
            skipBlank();
            if (peek() == '*')
            {
                const int starPos = at;
                ++at;
                long long n = 0;
                int digits = 0;
                const int numPos = at;
                while (isDigit (peek()) && digits < 6) { n = n * 10 + (peek() - '0'); ++at; ++digits; }
                if (digits == 0 || n < 2 || n > 64)
                    error (digits == 0 ? starPos : numPos, "a repeat must be 2-64");
                else
                    it.repeat = static_cast<int> (n);
                it.end = at;
            }
            else
                at = save;
        }
    }

    void parseVariation (Item& it)
    {
        const int open = at;
        if (depth >= kMaxDepth)
        {
            error (at, "too deeply nested");
            ++at;
            return;
        }
        ++at;
        ++depth;
        it.core = Core::Variation;
        for (;;)
        {
            it.parts.emplace_back();
            parseSequence (it.parts.back(), ']', open);
            if (peek() == '|') { ++at; continue; }
            break;
        }
        --depth;
        if (peek() == ']')
            ++at;
        else
            error (open, "missing ']'");
        it.end = at;
    }

    void parseChord (Item& it)
    {
        const int open = at;
        ++at;
        it.core = Core::Chord;
        std::vector<Prefix> pre;
        for (;;)
        {
            skipBlank();
            if (atEnd())
            {
                error (open, "missing '}'");
                break;
            }
            const char c = text[uz (at)];
            if (c == '}') { ++at; break; }
            if (c == 'o' || c == '#' || c == 'b')
            {
                parsePrefix (pre);
                continue;
            }
            if (c == 'x' || c == '.' || c == '_' || c == '(')
            {
                error (at, "'x' or a rest inside a chord");
                ++at;
                continue;
            }
            const int h = hexValue (c);
            if (h >= 0 || c == '+' || c == '-' || c == '=' || c == '?')
            {
                ChordNote cn;
                cn.prefixes = std::move (pre);
                pre.clear();
                ++at;
                if (h > 0) { cn.cmd = Cmd::Degree; cn.degree = h - 1; }
                else if (c == '0' || c == '=') cn.cmd = Cmd::Same;
                else if (c == '+') cn.cmd = Cmd::Next;
                else if (c == '-') cn.cmd = Cmd::Prev;
                else cn.cmd = Cmd::Random;
                it.chord.push_back (std::move (cn));
                continue;
            }
            error (at, std::string ("unknown character '") + c + "'");
            ++at;
        }
        if (it.chord.empty() && out.diagnostics.empty())
            error (open, "empty chord");
        out.pitched = true;
        if (it.chord.size() > 1) out.multi = true;
        it.end = at;
    }

    bool readInt (long long& v)
    {
        if (! isDigit (peek())) return false;
        v = 0;
        int digits = 0;
        while (isDigit (peek()))
        {
            if (digits < 15) v = v * 10 + (peek() - '0');
            ++digits;
            ++at;
        }
        return digits < 10;
    }

    bool readRange (Range& r)
    {
        long long a = 0, b = 0;
        if (! readInt (a)) return false;
        b = a;
        if (peek() == '-' && at + 1 < size() && isDigit (text[uz (at + 1)]))
        {
            ++at;
            if (! readInt (b)) return false;
        }
        r.lo = static_cast<int> (a);
        r.hi = static_cast<int> (b);
        return true;
    }

    void parseGenerator (Item& it)
    {
        const int gpos = at;
        const char g = text[uz (at)];
        ++at;
        it.core = Core::Generator;
        GenSpec& s = it.gen;
        s.euclid = g == 'e';
        bool good = true;

        if (s.euclid)
        {
            good = readRange (s.hits);
            if (good && peek() == '/') ++at; else good = false;
            good = good && readRange (s.steps);
            if (good && peek() == '+')
            {
                ++at;
                good = readRange (s.rot);
            }
            good = good && s.hits.lo <= s.hits.hi && s.steps.lo <= s.steps.hi && s.rot.lo <= s.rot.hi
                        && s.steps.lo >= 1 && s.steps.hi <= 1024;
        }
        else
        {
            good = readRange (s.steps);
            if (good && peek() == ':')
            {
                ++at;
                long long lo = 0, hi = 0;
                good = readInt (lo);
                hi = lo;
                if (good && peek() == '-')
                {
                    ++at;
                    good = readInt (hi);
                }
                s.valueGiven = true;
                s.vlo = lo;
                s.vhi = hi;
                good = good && lo <= hi && hi < (1LL << std::min (s.steps.hi, 32));
            }
            good = good && s.steps.lo <= s.steps.hi && s.steps.lo >= 1 && s.steps.hi <= 32;
        }
        if (! good)
        {
            error (gpos, "bad generator");
            it.core = Core::None;
            it.end = at;
            return;
        }

        skipBlank();
        const char c = peek();
        Item w;
        w.start = at;
        bool taken = false;
        if (c == '{') { parseChord (w); taken = true; }
        else if (c == '(' ) { parseGroup (w, false); taken = w.core == Core::Group; }
        else if (isStepStart (c) && c != '.' && c != '_') { parseStep (w, c); taken = true; }
        if (! taken)
        {
            error (gpos, "a generator needs what its hits play");
            it.core = Core::None;
            it.end = at;
            return;
        }
        w.end = at;
        it.what.push_back (std::move (w));
        it.end = at;
    }

    // The other branch of a probability: `:` then a step or a group.
    void parseAlt (Item& it)
    {
        const int save = at;
        skipBlank();
        if (peek() != ':')
        {
            at = save;
            return;
        }
        ++at;
        skipBlank();
        Item a;
        a.start = at;
        const char c = peek();
        if (c == '(')
            parseGroup (a, false);
        else if (isStepStart (c) && c != 'x')
            parseStep (a, c);
        else
        {
            if (syn3) error (at, "a step or a group after ':'");
            at = save;
            return;
        }
        a.end = at;
        it.alt.push_back (std::move (a));
        it.end = at;
    }

    void parseCore (Item& it, char c)
    {
        if (c == '"')
        {
            it.core = Core::Quote;
            if (quoteOpen < 0) quoteOpen = at; else quoteOpen = -1;
            ++at;
            it.end = at;
            return;
        }
        if (c == '(') { parseGroup (it, true); }
        else if (c == '[') { parseVariation (it); }
        else if (c == '{') { parseChord (it); }
        else if (c == 'e' || c == 'n') { parseGenerator (it); }
        else parseStep (it, c);

        // A probability's other branch.
        bool hasP = false;
        for (const Prefix& p : it.prefixes)
            if (p.type == 'p') { hasP = true; it.probPos = p.pos; }
        if (hasP)
        {
            if (it.core == Core::Step || (it.core == Core::Group && it.prefixes.back().type == 'p'))
                parseAlt (it);
        }
    }

    //--------------------------------------------------------------------------
    void parseSequence (std::vector<Item>& items, char closer, int)
    {
        std::vector<Prefix> pre;
        for (;;)
        {
            skipBlank();
            if (atEnd()) break;
            const char c = text[uz (at)];
            if (closer == ')' && c == ')') break;
            if (closer == ']' && (c == ']' || c == '|')) break;

            if (syn3 && c == 'l' && isLetLine())
            {
                if (seen)
                    error (at, "let after the first step");
                while (! atEnd() && text[uz (at)] != '\n') ++at;
                continue;
            }
            seen = true;

            if (isPrefixStart (c))
            {
                parsePrefix (pre);
                continue;
            }
            if (! isCoreStart (c))
            {
                if (syn3)
                {
                    if (c == ')') error (at, "')' without '('");
                    else if (c == '*') error (at, "'*' needs a group before it");
                    else error (at, std::string ("unknown character '") + c + "'");
                }
                ++at;
                continue;
            }

            Item it;
            it.start = pre.empty() ? at : pre.front().pos;
            it.prefixes = std::move (pre);
            pre.clear();
            parseCore (it, c);
            items.push_back (std::move (it));
        }

        if (! pre.empty())
        {
            if (syn3)
                warn (pre.front().pos, "prefixes without a step after them are ignored");
            else
            {
                Item it;
                it.start = pre.front().pos;
                it.end = at;
                it.prefixes = std::move (pre);
                items.push_back (std::move (it));
            }
        }
    }
};

//==============================================================================
// Measures: lengths, bounds, the equal-length rule

struct Metrics
{
    double beats = 0.0;
    long long steps = 0, events = 0, notes = 0;
    double shortest = 0.0;             // 0: no step
    bool variable = false;
};

class Measurer
{
public:
    explicit Measurer (Compiled& c) : out (c), syn3 (c.options.syntax >= 3) {}

    Metrics measureAll()
    {
        double nv = out.options.defaultStepBeats;
        return measure (out.items, nv);
    }

private:
    Compiled& out;
    const bool syn3;

    static long long sat (long long v) noexcept { return std::min (v, kSaturate); }

    static void add (Metrics& a, const Metrics& b)
    {
        a.beats += b.beats;
        a.steps = sat (a.steps + b.steps);
        a.events = sat (a.events + b.events);
        a.notes = sat (a.notes + b.notes);
        if (b.shortest > 0.0 && (a.shortest <= 0.0 || b.shortest < a.shortest)) a.shortest = b.shortest;
        a.variable = a.variable || b.variable;
    }

    Metrics measure (const std::vector<Item>& items, double& nv)
    {
        Metrics m;
        for (const Item& it : items)
            add (m, measureItem (it, nv));
        return m;
    }

    Metrics measureGroup (const Item& g, double nv)
    {
        double inner = nv;
        Metrics c = measure (g.children, inner);
        const long long r = g.repeat;
        c.beats *= static_cast<double> (r);
        c.steps = sat (c.steps * r);
        c.events = sat (c.events * r);
        c.notes = sat (c.notes * r);
        return c;
    }

    static Metrics stepMetrics (Cmd cmd, double nv)
    {
        Metrics m;
        m.beats = nv;
        m.steps = 1;
        m.shortest = nv;
        m.events = (cmd == Cmd::Rest || cmd == Cmd::Hold) ? 0 : 1;
        m.notes = isPitchedCmd (cmd) ? 1 : 0;
        return m;
    }

    // Notes of the largest unit (step or chord) in a group, for generators.
    static long long unitNotes (const std::vector<Item>& items)
    {
        long long best = 0;
        for (const Item& it : items)
        {
            if (it.core == Core::Step) best = std::max<long long> (best, isPitchedCmd (it.cmd) ? 1 : 0);
            else if (it.core == Core::Chord) best = std::max<long long> (best, static_cast<long long> (it.chord.size()));
            else if (it.core == Core::Group) best = std::max (best, unitNotes (it.children));
        }
        return best;
    }

    // `main` is the branch that plays; `alt` the other. Equal lengths in syntax 3.
    void branches (Metrics& main, const Metrics& alt, int probPos)
    {
        if (std::fabs (main.beats - alt.beats) > 1.0e-9)
        {
            if (syn3)
                out.diagnostics.push_back ({ probPos, "a probability's branches must last the same", false });
            main.variable = true;
        }
        main.steps = std::max (main.steps, alt.steps);
        main.events = std::max (main.events, alt.events);
        main.notes = std::max (main.notes, alt.notes);
        main.variable = main.variable || alt.variable;
        if (alt.shortest > 0.0 && (main.shortest <= 0.0 || alt.shortest < main.shortest)) main.shortest = alt.shortest;
    }

    Metrics measureItem (const Item& it, double& nv)
    {
        for (const Prefix& p : it.prefixes)
            if (p.type == 'm') nv = p.beats;

        Metrics m;
        switch (it.core)
        {
            case Core::Step:
            {
                m = stepMetrics (it.cmd, nv);
                if (! it.alt.empty())
                {
                    const Item& a = it.alt[0];
                    const Metrics am = a.core == Core::Step ? stepMetrics (a.cmd, nv) : measureGroup (a, nv);
                    branches (m, am, it.probPos);
                }
                break;
            }
            case Core::Chord:
                m.beats = nv;
                m.steps = 1;
                m.events = 1;
                m.notes = static_cast<long long> (it.chord.size());
                m.shortest = nv;
                break;
            case Core::Group:
            {
                m = measureGroup (it, nv);
                if (it.prefixes.size() > 0 && it.prefixes.back().type == 'p')
                {
                    if (! it.alt.empty())
                    {
                        const Item& a = it.alt[0];
                        const Metrics am = a.core == Core::Step ? stepMetrics (a.cmd, nv) : measureGroup (a, nv);
                        branches (m, am, it.probPos);
                    }
                    else if (! syn3 && m.beats > 0.0)
                        m.variable = true;           // a failed roll skips the group (syntax 2)
                }
                break;
            }
            case Core::Variation:
            {
                double firstEnd = nv;
                bool first = true;
                for (const auto& part : it.parts)
                {
                    double inner = nv;
                    const Metrics pm = measure (part, inner);
                    if (first)
                    {
                        m = pm;
                        firstEnd = inner;
                        first = false;
                    }
                    else
                    {
                        if (std::fabs (pm.beats - m.beats) > 1.0e-9)
                        {
                            if (syn3 && ! reportedVariation)
                            {
                                out.diagnostics.push_back ({ it.start, "the parts of a variation should last the same", true });
                                reportedVariation = true;
                            }
                            m.variable = true;
                            m.beats = std::max (m.beats, pm.beats);
                        }
                        m.steps = std::max (m.steps, pm.steps);
                        m.events = std::max (m.events, pm.events);
                        m.notes = std::max (m.notes, pm.notes);
                        m.variable = m.variable || pm.variable;
                        if (pm.shortest > 0.0 && (m.shortest <= 0.0 || pm.shortest < m.shortest)) m.shortest = pm.shortest;
                    }
                }
                nv = firstEnd;
                break;
            }
            case Core::Generator:
            {
                const GenSpec& s = it.gen;
                const long long n = s.steps.hi;
                m.beats = static_cast<double> (n) * nv;
                m.steps = n;
                m.shortest = nv;
                m.variable = s.steps.isRange();
                const long long hits = s.euclid ? std::min<long long> (s.hits.hi, n) : n;
                m.events = hits;
                const Item& w = it.what[0];
                long long per = 0;
                if (w.core == Core::Step) per = isPitchedCmd (w.cmd) ? 1 : 0;
                else if (w.core == Core::Chord) per = static_cast<long long> (w.chord.size());
                else if (w.core == Core::Group) per = unitNotes (w.children);
                m.notes = sat (hits * per);
                break;
            }
            case Core::Quote:
            case Core::None:
                break;
        }
        return m;
    }

    bool reportedVariation = false;
};

//==============================================================================
// Realizer

struct Pending
{
    int octave = -1;
    float velocity = -1.0f;
    int accidental = 0;
    double gate = -1.0;
    bool glide = false;
    bool forcedRest = false;
};

class Realizer
{
public:
    Realizer (const Compiled& c, const Context& ctx, Pass& pass)
        : cp (c), cx (ctx), out (pass), syn2 (c.options.syntax < 3) {}

    void run (Carry* carry)
    {
        octave = cx.baseOctave;
        noteValue = cp.options.defaultStepBeats;
        if (syn2 && carry != nullptr)
        {
            if (carry->octave >= 0) octave = carry->octave;
            velocity = carry->velocity;
            lastDegree = carry->lastDegree;
            keyScale = carry->inKeyScale;
            pend.octave = carry->pendingOctave;
            pend.velocity = carry->pendingVelocity;
            pend.accidental = carry->pendingAccidental;
            pend.forcedRest = carry->pendingForcedRest;
        }

        items (cp.items, false);
        out.lengthBeats = now;

        if (syn2 && carry != nullptr)
        {
            carry->octave = octave;
            carry->velocity = velocity;
            carry->lastDegree = lastDegree;
            carry->inKeyScale = keyScale;
            carry->pendingOctave = pend.octave;
            carry->pendingVelocity = pend.velocity;
            carry->pendingAccidental = pend.accidental;
            carry->pendingForcedRest = pend.forcedRest;
        }
    }

private:
    const Compiled& cp;
    const Context& cx;
    Pass& out;
    const bool syn2;

    // Persistent musical state
    int octave = 4;
    float velocity = -1.0f;
    double gate = 1.0;
    double noteValue = 0.25;
    int lastDegree = 0;
    bool keyScale = false;

    Pending pend;

    double now = 0.0;
    bool tiedRun = true;               // no step but tied holds so far
    int cur = -1;                      // the event that sounds
    double curHeld = 0.0, curGate = 1.0;
    std::uint64_t k = 0;
    bool full = false;

    struct Scope { int octave; float velocity; double gate; double noteValue; };
    Scope save() const noexcept { return { octave, velocity, gate, noteValue }; }
    void restore (const Scope& s) noexcept
    {
        octave = s.octave;
        velocity = s.velocity;
        gate = s.gate;
        noteValue = s.noteValue;
    }

    //--------------------------------------------------------------------------
    double nextDraw() noexcept { return draw (cx.seed, k++); }

    double exprValue (const Value& v) noexcept
    {
        if (v.expr < 0) return v.constant;
        const std::uint64_t s = mix (cx.seed ^ mix (k++));
        return cp.options.expressions != nullptr ? cp.options.expressions->evaluate (v.expr, s) : 0.0;
    }

    int pick (int n) noexcept          // 0..n-1
    {
        const int v = static_cast<int> (nextDraw() * n);
        return std::min (std::max (v, 0), n - 1);
    }

    int pickRange (const Range& r) noexcept
    {
        if (! r.isRange()) return r.lo;
        return r.lo + pick (r.hi - r.lo + 1);
    }

    bool roll (const Value& v) noexcept
    {
        double p = 1.0;
        // Syntax 3: `p0` never plays and draws nothing, so switching a step off with it
        // (Otomatotamoto's steps grid) leaves the draws of the steps after it as they were.
        if (v.kind == Value::Kind::Number && v.number == 0 && ! syn2)
            return false;
        if (v.kind == Value::Kind::Number) p = v.number * 0.1;
        else if (v.kind == Value::Kind::Expr) p = std::min (1.0, std::max (0.0, exprValue (v)));
        else return true;
        return nextDraw() < p;
    }

    int octaveTarget (const Value& v, int cur0) noexcept
    {
        int t = cur0;
        switch (v.kind)
        {
            case Value::Kind::Plus: t = std::min (7, cur0 + 1); break;
            case Value::Kind::Minus: t = std::max (0, cur0 - 1); break;
            case Value::Kind::Random: t = syn2 ? 1 + pick (9) : pick (8); break;
            case Value::Kind::Number: t = v.number; break;
            case Value::Kind::Expr: t = static_cast<int> (std::lround (exprValue (v))); break;
            default: break;
        }
        return std::min (7, std::max (0, t));
    }

    void applyPrefix (const Prefix& p) noexcept
    {
        switch (p.type)
        {
            case 'o': case 'O':
            {
                const int t = octaveTarget (p.value, pend.octave >= 0 ? pend.octave : octave);
                if (p.type == 'o') pend.octave = t; else octave = t;
                break;
            }
            case 'v': case 'V':
            {
                float nv = -1.0f;
                switch (p.value.kind)
                {
                    case Value::Kind::Plus: case Value::Kind::Minus:
                    {
                        const float base = pend.velocity >= 0.0f ? pend.velocity
                                         : velocity >= 0.0f ? velocity : cx.hostVelocity;
                        const int level = levelForVelocity (static_cast<float> (std::lround (base * 127.0f)));
                        const int l2 = p.value.kind == Value::Kind::Plus ? std::min (15, level + 1)
                                                                          : std::max (1, level - 1);
                        nv = static_cast<float> (velocityForLevel (l2)) / 127.0f;
                        break;
                    }
                    case Value::Kind::Random:
                        nv = static_cast<float> (velocityForLevel (1 + pick (15))) / 127.0f;
                        break;
                    case Value::Kind::Number:
                        nv = static_cast<float> (velocityForLevel (p.value.number)) / 127.0f;
                        break;
                    case Value::Kind::Expr:
                        nv = static_cast<float> (std::min (1.0, std::max (0.0, exprValue (p.value))));
                        break;
                    default: break;
                }
                if (nv >= 0.0f)
                {
                    if (p.type == 'v') pend.velocity = nv; else velocity = nv;
                }
                break;
            }
            case '#': pend.accidental = 1; break;
            case 'b': pend.accidental = -1; break;
            case 'g': case 'G':
            {
                double g = 1.0;
                if (p.value.kind == Value::Kind::Number) g = p.value.number * 0.1;
                else if (p.value.kind == Value::Kind::Expr) g = std::min (1.0, std::max (0.0, exprValue (p.value)));
                if (p.type == 'g') pend.gate = g; else gate = g;
                break;
            }
            case 'm': noteValue = p.beats; break;
            case '~': pend.glide = true; break;
            default: break;
        }
    }

    //--------------------------------------------------------------------------
    int randomDegree() noexcept
    {
        if (keyScale)
        {
            if (cx.keySize <= 0) return -1;
            return pick (cx.keySize);
        }
        const int n = std::min (cx.chordSize, 32);
        int present = 0;
        for (int d = 0; d < n; ++d)
            if ((cx.chordPresent >> d) & 1u) ++present;
        if (present == 0) return -1;
        int target = pick (present);
        for (int d = 0; d < n; ++d)
            if (((cx.chordPresent >> d) & 1u) && target-- == 0)
                return d;
        return -1;
    }

    // The degree a pitched command names now; -1: none.
    int degreeFor (Cmd cmd, int degree) noexcept
    {
        const int size = std::max (1, keyScale ? cx.keySize : cx.chordSize);
        switch (cmd)
        {
            case Cmd::Degree: return degree;
            case Cmd::Same: return lastDegree;
            case Cmd::Next: return (lastDegree + 1) % size;
            case Cmd::Prev: return (lastDegree + size - 1) % size;
            case Cmd::Random: return randomDegree();
            default: return -1;
        }
    }

    // Room for a step, and for the event and notes it may make: a pass never grows past what was reserved.
    bool room (bool event, std::size_t notes) const noexcept
    {
        return out.steps.size() < out.steps.capacity()
            && (! event || out.events.size() < out.events.capacity())
            && out.notes.size() + notes <= out.notes.capacity();
    }

    //--------------------------------------------------------------------------
    enum class Act { Rest, Hold, Hit, Notes };

    void finishStep (int sourceStart, int sourceEnd, double dur, int event, bool tied = false)
    {
        if (! tied) tiedRun = false;
        Step s;
        s.onset = now;
        s.duration = dur;
        s.sourceStart = sourceStart;
        s.sourceEnd = sourceEnd;
        s.event = event;
        s.tied = tied;
        out.steps.push_back (s);
        now += dur;
        pend = Pending {};
    }

    // Emits what a step does. `first`/`count`: the notes already pushed for Act::Notes.
    void emit (Act act, int firstNote, int noteCount, int sourceStart, int sourceEnd, bool muted)
    {
        const double dur = noteValue;
        if (muted || (pend.forcedRest && act != Act::Rest && act != Act::Hold))
            act = Act::Rest;
        else if (pend.forcedRest)
            act = (act == Act::Hold) ? Act::Rest : Act::Hold;

        if (act == Act::Rest || (act == Act::Hold && cur < 0))
        {
            // A hold before anything sounded in this pass belongs to the previous pass's last sound.
            const bool tied = act == Act::Hold && tiedRun;
            cur = -1;
            out.notes.resize (static_cast<std::size_t> (firstNote));
            finishStep (sourceStart, sourceEnd, dur, -1, tied);
            return;
        }
        if (act == Act::Hold)
        {
            curHeld += dur;
            out.events[uz (cur)].length = curHeld * curGate;
            finishStep (sourceStart, sourceEnd, dur, cur);
            return;
        }

        Event e;
        e.onset = now;
        const double g = pend.gate >= 0.0 ? pend.gate : gate;
        e.length = dur * g;
        e.velocity = pend.velocity >= 0.0f ? pend.velocity : velocity;
        e.glide = pend.glide;
        e.firstNote = firstNote;
        e.noteCount = act == Act::Notes ? noteCount : 0;
        e.firstStep = static_cast<int> (out.steps.size());
        out.events.push_back (e);
        cur = static_cast<int> (out.events.size()) - 1;
        curHeld = dur;
        curGate = g;
        finishStep (sourceStart, sourceEnd, dur, cur);
    }

    void step (Cmd cmd, int degree, int sourceStart, int sourceEnd, bool muted)
    {
        if (! room (cmd != Cmd::Rest && cmd != Cmd::Hold, isPitchedCmd (cmd) ? 1 : 0)) { full = true; return; }
        const int firstNote = static_cast<int> (out.notes.size());
        Act act = Act::Rest;
        int deg = -1;
        switch (cmd)
        {
            case Cmd::Rest: act = Act::Rest; break;
            case Cmd::Hold: act = Act::Hold; break;
            case Cmd::Hit: act = Act::Hit; break;
            default:
                deg = degreeFor (cmd, degree);
                act = deg < 0 ? Act::Rest : Act::Notes;
                break;
        }
        if (act == Act::Notes)
        {
            Note n;
            n.scale = keyScale ? ScaleKind::Key : ScaleKind::Chord;
            n.degree = deg;
            n.octave = pend.octave >= 0 ? pend.octave : octave;
            n.accidental = pend.accidental;
            out.notes.push_back (n);
            if (! muted && ! pend.forcedRest) lastDegree = deg;
        }
        emit (act, firstNote, act == Act::Notes ? 1 : 0, sourceStart, sourceEnd, muted);
    }

    void chord (const Item& ch, int sourceStart, int sourceEnd, bool muted)
    {
        const std::size_t count = ch.chord.size();
        if (! room (true, count)) { full = true; return; }
        const int firstNote = static_cast<int> (out.notes.size());
        int firstDegree = -1;
        int made = 0;
        for (const ChordNote& cn : ch.chord)
        {
            int oct = pend.octave >= 0 ? pend.octave : octave;
            int acc = pend.accidental;
            for (const Prefix& p : cn.prefixes)
            {
                if (p.type == 'o') oct = octaveTarget (p.value, oct);
                else if (p.type == '#') acc = 1;
                else if (p.type == 'b') acc = -1;
            }
            const int deg = degreeFor (cn.cmd, cn.degree);
            if (deg < 0) continue;
            Note n;
            n.scale = keyScale ? ScaleKind::Key : ScaleKind::Chord;
            n.degree = deg;
            n.octave = oct;
            n.accidental = acc;
            out.notes.push_back (n);
            if (firstDegree < 0) firstDegree = deg;
            if (! muted && ! pend.forcedRest) lastDegree = deg;     // `+` inside the chord goes on from the previous note
            ++made;
        }
        if (made > 0 && ! muted && ! pend.forcedRest) lastDegree = firstDegree;   // after the chord: from its first note
        emit (made > 0 ? Act::Notes : Act::Rest, firstNote, made, sourceStart, sourceEnd, muted);
    }

    //--------------------------------------------------------------------------
    void runGroup (const Item& g, bool muted)
    {
        const Scope s = save();
        for (int r = 0; r < g.repeat && ! full; ++r)
        {
            items (g.children, muted);
            restore (s);
        }
    }

    void runAlt (const Item& a, bool muted)
    {
        if (a.core == Core::Group) runGroup (a, muted);
        else step (a.cmd, a.degree, a.start, a.end, muted);
    }

    void items (const std::vector<Item>& v, bool muted)
    {
        for (const Item& it : v)
        {
            if (full) return;
            item (it, muted);
        }
    }

    static int countUnits (const std::vector<Item>& v) noexcept
    {
        int n = 0;
        for (const Item& it : v)
        {
            if (it.core == Core::Step || it.core == Core::Chord) ++n;
            else if (it.core == Core::Group) n += countUnits (it.children);
        }
        return n;
    }

    static const Item* unitAt (const std::vector<Item>& v, int& index) noexcept
    {
        for (const Item& it : v)
        {
            if (it.core == Core::Step || it.core == Core::Chord)
            {
                if (index-- == 0) return &it;
            }
            else if (it.core == Core::Group)
            {
                if (const Item* u = unitAt (it.children, index)) return u;
            }
        }
        return nullptr;
    }

    void generator (const Item& it, bool muted)
    {
        const Pending tmpl = pend;
        pend = Pending {};
        const GenSpec& s = it.gen;
        const Item& w = it.what[0];

        int n = 0, kHits = 0, rot = 0;
        long long value = 0;
        if (s.euclid)
        {
            kHits = pickRange (s.hits);
            n = pickRange (s.steps);
            rot = pickRange (s.rot);
        }
        else
        {
            n = pickRange (s.steps);
            long long lo = 0, hi = (1LL << n) - 1;
            if (s.valueGiven) { lo = s.vlo; hi = s.vhi; }
            if (hi > lo)
            {
                const long long span = hi - lo + 1;
                value = lo + std::min (span - 1, static_cast<long long> (nextDraw() * static_cast<double> (span)));
            }
            else
                value = lo;
        }

        int turn = 0;
        for (int i = 0; i < n && ! full; ++i)
        {
            bool hit = false;
            if (s.euclid)
                hit = kHits > 0 && ((static_cast<long long> ((i + rot) % n)) * kHits) % n < kHits;
            else
                hit = ((value >> (n - 1 - i)) & 1LL) != 0;

            pend = tmpl;
            if (! hit)
            {
                pend = Pending {};
                step (Cmd::Rest, 0, it.start, it.end, muted);
                continue;
            }
            for (const Prefix& p : it.prefixes)
                if (p.type == 'p' && ! roll (p.value))
                    pend.forcedRest = true;

            if (w.core == Core::Step)
                step (w.cmd, w.degree, it.start, w.end, muted);
            else if (w.core == Core::Chord)
                chord (w, it.start, w.end, muted);
            else
            {
                const int total = countUnits (w.children);
                int idx = total > 0 ? turn % total : 0;
                ++turn;
                const Item* u = total > 0 ? unitAt (w.children, idx) : nullptr;
                if (u == nullptr)
                {
                    step (Cmd::Rest, 0, it.start, it.end, muted);
                    continue;
                }
                for (const Prefix& p : u->prefixes)
                {
                    if (p.type == 'p') { if (! roll (p.value)) pend.forcedRest = true; }
                    else applyPrefix (p);
                }
                if (u->core == Core::Step) step (u->cmd, u->degree, it.start, u->end, muted);
                else chord (*u, it.start, u->end, muted);
            }
        }
        pend = Pending {};
    }

    void item (const Item& it, bool muted)
    {
        const int np = static_cast<int> (it.prefixes.size());
        bool groupRolled = false, groupOk = true;
        bool stepFailed = false;
        bool altGroup = false;

        for (int i = 0; i < np; ++i)
        {
            const Prefix& p = it.prefixes[uz (i)];
            if (p.type != 'p')
            {
                applyPrefix (p);
                continue;
            }
            if (it.core == Core::Generator)
                continue;                                  // rolled for each hit
            const bool ok = roll (p.value);
            if (it.core == Core::Group && i == np - 1)
            {
                groupRolled = true;
                groupOk = ok;
            }
            else if (! ok)
            {
                pend.forcedRest = true;
                stepFailed = true;
                if (it.core == Core::Step && ! it.alt.empty() && it.alt[0].core == Core::Group)
                {
                    altGroup = true;                       // the rest of the prefixes are skipped with the step
                    break;
                }
            }
        }

        switch (it.core)
        {
            case Core::Step:
            {
                if (altGroup)
                {
                    pend.forcedRest = false;
                    runGroup (it.alt[0], muted);
                    break;
                }
                if (stepFailed && ! it.alt.empty())
                {
                    pend.forcedRest = false;
                    step (it.alt[0].cmd, it.alt[0].degree, it.start, it.end, muted);
                    break;
                }
                step (it.cmd, it.degree, it.start, it.end, muted);
                break;
            }
            case Core::Chord:
                chord (it, it.start, it.end, muted);
                break;
            case Core::Group:
                if (groupRolled && ! groupOk)
                {
                    if (! it.alt.empty()) runAlt (it.alt[0], muted);
                    else if (syn2) pend.forcedRest = true;      // TeAr: the next step becomes a rest
                    else runGroup (it, true);                   // the same length, silent
                }
                else
                    runGroup (it, muted);
                break;
            case Core::Variation:
            {
                const int n = static_cast<int> (it.parts.size());
                if (n > 0)
                {
                    int idx = cx.passIndex % n;
                    if (idx < 0) idx += n;
                    items (it.parts[uz (idx)], muted);
                }
                break;
            }
            case Core::Generator:
                generator (it, muted);
                break;
            case Core::Quote:
                keyScale = ! keyScale;
                break;
            case Core::None:
                break;
        }
    }
};

} // namespace detail

//==============================================================================
// Program

struct Program::Impl
{
    detail::Compiled c;
};

inline Program::Program() : impl (new Impl) {}
inline Program::~Program() = default;
inline Program::Program (Program&&) noexcept = default;
inline Program& Program::operator= (Program&&) noexcept = default;

inline bool Program::ok() const noexcept { return impl->c.ok; }
inline const std::vector<Diagnostic>& Program::diagnostics() const noexcept { return impl->c.diagnostics; }
inline const Info& Program::info() const noexcept { return impl->c.info; }
inline const std::string& Program::text() const noexcept { return impl->c.text; }
inline const Options& Program::options() const noexcept { return impl->c.options; }

inline void Program::realize (const Context& context, Pass& out, Carry* carry) const noexcept
{
    const detail::Compiled& c = impl->c;
    out.clear();
    if (! c.ok)
        return;

    // A pass the host did not reserve is sized here, once (this allocates).
    if (out.events.capacity() < detail::uz (c.info.maxEventsPerPass)
        || out.notes.capacity() < detail::uz (c.info.maxNotesPerPass)
        || out.steps.capacity() < detail::uz (c.info.maxStepsPerPass))
    {
        try { out.reserve (c.info); }
        catch (...) { return; }
    }

    detail::Realizer r (c, context, out);
    r.run (carry);
}

inline Program compile (std::string_view text, const Options& options)
{
    Program prog;
    detail::Compiled& c = prog.impl->c;
    c.text.assign (text.data(), text.size());
    c.options = options;
    if (c.options.defaultStepBeats <= 0.0)
        c.options.defaultStepBeats = 0.25;

    detail::Parser (c).run();

    auto hasError = [&c]
    {
        for (const Diagnostic& d : c.diagnostics)
            if (! d.warning) return true;
        return false;
    };

    if (! hasError())
    {
        detail::Measurer measurer (c);
        const detail::Metrics m = measurer.measureAll();
        if (m.steps > detail::kMaxSteps)
            c.diagnostics.push_back ({ 0, "the pattern is too long", false });
        else
        {
            Info& i = c.info;
            i.kind = c.multi ? Info::Kind::Chords : c.pitched ? Info::Kind::Melody : Info::Kind::Rhythm;
            i.glides = c.glides;
            i.fixedLength = ! m.variable;
            i.lengthBeats = m.beats;
            i.shortestStepBeats = m.shortest;
            i.maxEventsPerPass = static_cast<int> (m.events);
            i.maxNotesPerPass = static_cast<int> (m.notes);
            i.maxStepsPerPass = static_cast<int> (m.steps);
        }
    }

    c.ok = ! hasError();
    if (! c.ok)
    {
        c.info = Info {};
        c.items.clear();
    }
    return prog;
}

} // namespace fxme::pattern
