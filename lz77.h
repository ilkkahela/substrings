#pragma once
#include <ostream>
#include <vector>
#include "suffix_array.h"

// One phrase of an LZ77 factorization: either a copy of text[src, src+len),
// which may overlap the phrase itself (src < pos, but src + len can pass pos),
// or a single literal byte (src == -1, len == 1).
struct Lz77Phrase {
    int pos;
    int len;
    int src;

    bool operator==(const Lz77Phrase&) const = default;
};

// Greedy byte-level LZ77 with an unbounded window: each phrase is the longest
// prefix of the rest of the text that also starts earlier, or a literal byte.
// For this kind of parsing greedy gives the fewest phrases (z). The source is
// one of two candidates (see lz77.cpp), the closer one if they tie; it is not
// necessarily the closest earlier occurrence in the whole text.
[[nodiscard]] std::vector<Lz77Phrase> lz77Factorize(const SuffixArray& suffix_array);

// Prints the factorization as TSV (pos, len, src, escaped text; src is "-" for
// a literal) followed by a "# n=... z=..." summary line. Decoding the phrases
// in order reproduces the input.
void printLz77(std::ostream& out, const SuffixArray& suffix_array);
