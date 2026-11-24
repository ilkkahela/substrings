#pragma once
#include <ostream>
#include "suffix_array.h"

struct VizOptions {
    int width = 80;     // terminal columns to fit rows into
    bool color = true;  // ANSI colors and bold
};

// Prints the SA, Rank, PLCP and LCP arrays alongside the sorted suffixes,
// with each suffix's LCP prefix in bold (when coloring). Suffixes are cut to
// fit opts.width columns, and control characters, invisible characters and
// invalid UTF-8 are shown visibly.
void printTables(std::ostream& out, const SuffixArray& sa, const VizOptions& opts = {});

// The full matrix is drawn only up to this many characters; above it only the
// summary is printed.
inline constexpr int kMaxBwtMatrixChars = 1000;

// Prints the Burrows–Wheeler matrix of text + "$" over characters: every
// rotation in sorted order, with the first (F) and last (L) columns
// highlighted and the original text's row marked, then the BWT string (the L
// column), that row's index and the number of runs in L. "$" sorts before
// every character. Rows are cut in the middle to fit opts.width columns;
// characters are shown as in printTables.
void printBwt(std::ostream& out, const SuffixArray& sa, const VizOptions& opts = {});
