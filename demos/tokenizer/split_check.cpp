//
//  split_check.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  The ASCII fast path of next_piece against next_piece_general, the character-by-character reference it hands
//  over to, under both patterns:
//
//    - every position of the benchmark corpus (not only piece starts: next_piece must agree from any start);
//    - 300,000 random texts built from fragments chosen to sit on every decision the pattern makes: letters,
//      digits, the six ASCII spaces, quotes and contractions (upper case too), punctuation, and the non-ASCII
//      characters that change a decision - letters (é ß 中), a number (٣), spaces (U+00A0, U+2028, U+3000),
//      U+017F (folds to s), U+0085 - and bytes that are not UTF-8 (a lone continuation, 0xFF, a cut sequence);
//    - the ASCII class table against the White_Space set it is meant to hold.
//
//  Exits non-zero on the first disagreement, printing the text around it.
//
//      split_check [corpus docs.bin]
//

#include "tok/split.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace tk = snicholls::tokenizer;

namespace
{
    std::size_t checked = 0;

    bool agree_everywhere(std::string_view t, const char* what)
    {
        for (const auto p : {tk::split_pattern::gpt4, tk::split_pattern::cl100k})
            for (std::size_t i = 0; i < t.size(); ++i) {
                const std::size_t a = tk::next_piece(t, i, p), b = tk::detail::next_piece_general(t, i, p);
                ++checked;
                if (a != b) {
                    std::string ctx;
                    for (std::size_t k = i; k < t.size() && k < i + 24; ++k) {
                        const unsigned char c = static_cast<unsigned char>(t[k]);
                        char buf[8];
                        std::snprintf(buf, sizeof buf, c >= 0x20 && c < 0x7f ? "%c" : "\\x%02x", c);
                        ctx += buf;
                    }
                    std::fprintf(stderr, "%s: %s pattern, from %zu: fast %zu, reference %zu, text \"%s\"\n", what,
                                 p == tk::split_pattern::gpt4 ? "gpt4" : "cl100k", i, a, b, ctx.c_str());
                    return false;
                }
            }
        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    // The table: ASCII White_Space is exactly \t \n \v \f \r and ' '
    for (unsigned c = 0; c < 128; ++c) {
        const bool want = c == ' ' || (c >= 9 && c <= 13);
        if (tk::detail::is_ascii_space(c) != want) {
            std::fprintf(stderr, "byte 0x%02x: the table says space %d, White_Space says %d\n", c, tk::detail::is_ascii_space(c), want);
            return 1;
        }
    }

    if (argc > 1) {
        std::ifstream in(argv[1], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string raw = ss.str();
        const char* p = raw.data();
        const auto u32 = [&] { std::uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; };
        const std::uint32_t docs = u32();
        for (std::uint32_t d = 0; d < docs; ++d) {
            const std::uint32_t len = u32();
            if (!agree_everywhere(std::string_view(p, len), "corpus"))
                return 1;
            p += len;
        }
        std::printf("split_check: corpus, every position of %u documents under both patterns: agree\n", docs);
    }

    const std::vector<std::string> frag = {
        "a", "Z", "word", "MiXeD", "x", "0", "7", "12", "1234", " ", "  ", "    ", "\t", "\n", "\r\n", "\r", "\v", "\f",
        "'", "'s", "'S", "'t", "'ll", "'LL", "'ve", "'re", "'Re", "'d", "'m", "'x", "''", "\"", ".", ",", "(", ")", "_",
        "->", "==", "#", "\\", "@", "$", "~",
        "\xc3\xa9", "\xc3\x9f", "\xe4\xb8\xad", "\xd9\xa3", "\xc2\xa0", "\xe2\x80\xa8", "\xe3\x80\x80", "\xc5\xbf",
        "\xc2\x85", "\x80", "\xff", "\xe4\xb8", "\xc3",
    };
    std::mt19937_64 rng(20261111);
    for (int t = 0; t < 300000; ++t) {
        std::string s;
        const std::size_t parts = 1 + rng() % 40;
        for (std::size_t k = 0; k < parts; ++k)
            s += frag[rng() % frag.size()];
        if (!agree_everywhere(s, "random"))
            return 1;
    }
    std::printf("split_check: 300000 random texts, every position under both patterns: agree (%zu positions in all)\n", checked);
    return 0;
}
