//
//  split.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The pre-tokenizer: text to pieces, as tiktoken's regular expression splits it, written out by hand.
//
//  A BPE tokenizer first cuts text into pieces with a regular expression and then encodes each piece on its
//  own. tiktoken runs the expression through fancy-regex, and that is most of its time. The expression is
//  small and fixed, so here it is as code: each alternative of the pattern, in the pattern's order, as a
//  branch below. Two patterns are supported, and they differ only at the end of the text:
//
//    gpt4    '(?i:[sdmt]|ll|ve|re)|[^\r\n\p{L}\p{N}]?+\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]++[\r\n]*|\s*[\r\n]|\s+(?!\S)|\s+
//            (GPT-4's published pattern as rustbpe and nanochat use it)
//    cl100k  '(?i:[sdmt]|ll|ve|re)|[^\r\n\p{L}\p{N}]?+\p{L}++|\p{N}{1,3}+| ?[^\s\p{L}\p{N}]++[\r\n]*+|\s++$|\s*[\r\n]|\s+(?!\S)|\s
//            (tiktoken's cl100k_base)
//
//  Possessive and greedy quantifiers behave the same in every alternative here, because nothing follows them
//  inside the alternative, and the closing `\s` and `\s+` match the same text: they are reached only for one
//  whitespace character followed by a non-space. So the one difference is cl100k's `\s++$`, trailing
//  whitespace as one piece.
//
//  Classes: \p{L}, \p{N} and \s (Unicode White_Space, as in Rust's regex) come from unicode_classes.hpp. The
//  (?i) in the contractions also lets 's' match U+017F LATIN SMALL LETTER LONG S, its one non-ASCII case fold
//  among those letters. Input is UTF-8; a byte that does not start a valid sequence is one "other" character,
//  which tiktoken never sees because it takes Python strings.
//
//  Splitting in parallel. `find_boundary` returns a position the whole-text split is certain to start a piece
//  at, using only bytes around it, so a text can be cut there and each part split on its own thread with the
//  same pieces as a split from the start. The proof is beside the function.
//

#ifndef ts_moveables_tokenizer_split_hpp
#define ts_moveables_tokenizer_split_hpp

#include "unicode_classes.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace snicholls::tokenizer
{

/// \brief Which split pattern: GPT-4's as rustbpe uses it, or tiktoken's cl100k_base.
enum class split_pattern
{
    gpt4,
    cl100k,
};

namespace detail
{

enum : unsigned
{
    other = 0,
    letter = 1,
    number = 2,
    space = 3
};

// The class of an ASCII byte, the common case, without the two-stage table
inline constexpr auto ascii_classes = [] {
    struct table {
        std::uint8_t v[128];
    } t{};
    for (unsigned c = 0; c < 128; ++c)
        t.v[c] = static_cast<std::uint8_t>(unicode_class(c));
    return t;
}();

// The class of the character at s[i] and its length in bytes. A byte that does not start a valid UTF-8
// sequence (or one cut short by the end) is an "other" character of one byte.
inline unsigned class_at(const unsigned char* s, std::size_t n, std::size_t i, std::size_t& len) noexcept
{
    const unsigned c = s[i];
    if (c < 0x80) {
        len = 1;
        return ascii_classes.v[c];
    }
    std::uint32_t cp;
    if (c >= 0xC2 && c < 0xE0 && i + 1 < n && (s[i + 1] & 0xC0) == 0x80) {
        cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
        len = 2;
    } else if (c >= 0xE0 && c < 0xF0 && i + 2 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) {
        cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            len = 1;
            return other;
        }
        len = 3;
    } else if (c >= 0xF0 && c < 0xF5 && i + 3 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80 &&
               (s[i + 3] & 0xC0) == 0x80) {
        cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
        if (cp < 0x10000 || cp > 0x10FFFF) {
            len = 1;
            return other;
        }
        len = 4;
    } else {
        len = 1;
        return other;
    }
    return unicode_class(cp);
}

inline constexpr bool is_ascii_letter(unsigned c) noexcept { return ((c | 0x20u) - 'a') < 26u; }

} // namespace detail

/// \brief The end of the piece that starts at `i` (< text.size()): the next match of the split pattern.
/// Lookahead may read past `i` to the end of `text`, never before `i`.
inline std::size_t next_piece(std::string_view text, std::size_t i, split_pattern pattern) noexcept
{
    using namespace detail;
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    const unsigned c = s[i];

    // Fast path for the two shapes that are most of real text, an ASCII letter run and a space before one:
    // the letter alternative, byte at a time, handing over to the general loop at the first non-ASCII byte
    // (which may be a letter). Anything else takes the general path below.
    {
        std::size_t j = i;
        if (c == ' ' && i + 1 < n && is_ascii_letter(s[i + 1]))
            j = i + 2;
        else if (is_ascii_letter(c))
            j = i + 1;
        if (j != i) {
            while (j < n && is_ascii_letter(s[j]))
                ++j;
            if (j == n || s[j] < 0x80)
                return j;
            std::size_t len;
            while (j < n && class_at(s, n, j, len) == letter)
                j += len;
            return j;
        }
    }

    // '(?i:[sdmt]|ll|ve|re)
    if (c == '\'' && i + 1 < n) {
        const unsigned a = s[i + 1] | 0x20u;
        if (is_ascii_letter(s[i + 1]) && (a == 's' || a == 'd' || a == 'm' || a == 't'))
            return i + 2;
        if (s[i + 1] == 0xC5 && i + 2 < n && s[i + 2] == 0xBF)   // U+017F folds to s
            return i + 3;
        if (i + 2 < n && is_ascii_letter(s[i + 1]) && is_ascii_letter(s[i + 2])) {
            const unsigned b = s[i + 2] | 0x20u;
            if ((a == 'l' && b == 'l') || (a == 'v' && b == 'e') || (a == 'r' && b == 'e'))
                return i + 3;
        }
    }

    std::size_t len0;
    const unsigned k0 = class_at(s, n, i, len0);
    const auto letters_from = [&](std::size_t j) {   // \p{L}+ from j, at least one already checked
        std::size_t len;
        while (j < n && class_at(s, n, j, len) == letter)
            j += len;
        return j;
    };

    // [^\r\n\p{L}\p{N}]?+\p{L}+
    if (k0 == letter)
        return letters_from(i + len0);
    if (k0 != number && c != '\r' && c != '\n' && i + len0 < n) {
        std::size_t len1;
        if (class_at(s, n, i + len0, len1) == letter)
            return letters_from(i + len0 + len1);
    }

    // \p{N}{1,3}
    if (k0 == number) {
        std::size_t j = i + len0, len;
        for (int k = 1; k < 3 && j < n && class_at(s, n, j, len) == number; ++k)
            j += len;
        return j;
    }

    // ' ?[^\s\p{L}\p{N}]++[\r\n]*'
    {
        std::size_t j = i, len;
        if (c == ' ' && i + 1 < n && class_at(s, n, i + 1, len) == other)
            j = i + 1;
        if (j < n && class_at(s, n, j, len) == other) {
            j += len;
            while (j < n && class_at(s, n, j, len) == other)
                j += len;
            while (j < n && (s[j] == '\r' || s[j] == '\n'))
                ++j;
            return j;
        }
    }

    // What is left starts with whitespace. j is the end of the run, nl just past its last \r or \n, last the
    // start of its last character.
    std::size_t j = i, nl = 0, last = i, len;
    while (j < n && class_at(s, n, j, len) == space) {
        last = j;
        if (s[j] == '\r' || s[j] == '\n')
            nl = j + 1;
        j += len;
    }
    if (pattern == split_pattern::cl100k && j == n)   // \s++$
        return n;
    if (nl != 0)   // \s*[\r\n]
        return nl;
    if (j == n)   // \s+(?!\S) at the end
        return n;
    if (last > i)   // \s+(?!\S): all but the last
        return last;
    return i + len0;   // \s (or \s+): the one character
}

/// \brief Calls `each(piece)` for every piece of `text` that starts in [begin, end), in order. `begin` must be
/// a piece start of the whole text (0, or a `find_boundary`); lookahead reads past `end` as the whole-text
/// split does, so the pieces are the whole-text split's.
template <typename Each>
void split(std::string_view text, std::size_t begin, std::size_t end, split_pattern pattern, Each&& each)
{
    std::size_t i = begin;
    while (i < end) {
        const std::size_t j = next_piece(text, i, pattern);
        each(text.substr(i, j - i));
        i = j;
    }
}

template <typename Each>
void split(std::string_view text, split_pattern pattern, Each&& each)
{
    split(text, 0, text.size(), pattern, static_cast<Each&&>(each));
}

/// \brief The first position at or after `from` where the whole-text split certainly starts a piece, found
/// from the bytes around it alone; `text.size()` if there is none.
///
/// Two shapes, and why each is a boundary whatever came before:
///
/// 1. An ASCII letter at p-1, and at p an ASCII byte that is not a letter. The piece holding the letter at
///    p-1 is a contraction or a letter run (no other alternative admits a letter). A contraction is complete
///    at p-1 or earlier; a letter run stops at the first non-letter, which is p. Either way a piece ends at p.
/// 2. A \n or \r at p-1, and at p a character that is not whitespace. The piece holding it is a whitespace
///    run cut at its last newline (\s*[\r\n]), or a punctuation run with its trailing newlines
///    ([\r\n]*); both end at the last newline before non-whitespace, p. (\s++$ and \s+(?!\S) are never reached
///    for a run with a newline and a non-space after it; the [^\r\n...] prefix excludes newlines.)
///
/// The split is deterministic and reads only forward from a piece start, so the pieces from p are the same
/// whether the split began at 0 or at p.
inline std::size_t find_boundary(std::string_view text, std::size_t from) noexcept
{
    using namespace detail;
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    for (std::size_t p = from < 1 ? 1 : from; p < n; ++p) {
        const unsigned b = s[p - 1], c = s[p];
        if (is_ascii_letter(b) && c < 0x80 && !is_ascii_letter(c))
            return p;
        if (b == '\n' || b == '\r') {
            std::size_t len;
            if (c != '\n' && c != '\r' && class_at(s, n, p, len) != space)
                return p;
        }
    }
    return n;
}

} // namespace snicholls::tokenizer

#endif // ts_moveables_tokenizer_split_hpp
