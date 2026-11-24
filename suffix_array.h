#pragma once
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "utf8.h"

// libsais is built in its 32-bit variant, so suffix positions must fit in int32_t.
inline constexpr size_t kMaxInputBytes = INT32_MAX;

struct Score {
    std::string substring;
    int64_t frequency{};
};

// A substring that occurs exactly once, as both a byte and a character range.
struct UniqueSubstring {
    int pos;       // byte offset
    int len;       // bytes
    int char_pos;  // characters before pos
    int char_len;  // characters
};

struct PopularOptions {
    size_t top_n = 100000;
    int min_length = 0;  // bytes, or words with `words`; 0: 2 bytes or 1 word
    bool words = false;  // whole words and phrases only (repeats and --find)
    bool show_count = true;
    std::string delimiter = " ";        // between substring and count
    std::string entry_delimiter = "\n"; // between entries
    ListFormat format = ListFormat::Raw;  // Json ignores show_count and the delimiters
};

// Byte-level suffix array over a text, with its inverse (rank) and LCP arrays.
class SuffixArray {
    std::string text;
    std::vector<int> sa;    // sa[i]: text position of i-th smallest suffix
    std::vector<int> rank_; // inverse of sa: rank_[sa[i]] = i
    std::vector<int> lcp;   // lcp[i]: LCP of suffixes sa[i-1] and sa[i]
    std::vector<int> plcp;  // plcp[p] = lcp[rank_[p]]

    void buildSA();
    void buildRank();
    void buildPLCP();
    void buildLCP();
    // sa[first, last): the suffixes that start with pattern.
    [[nodiscard]] std::pair<int, int> matchRange(std::string_view pattern) const;

public:
    // With show_progress, a progress bar is drawn on stderr if the build
    // takes longer than progress_delay seconds.
    explicit SuffixArray(std::string text, bool show_progress = false,
                         double progress_delay = 0.3);

    // Most frequent repeated byte strings (length >= 1), by frequency then
    // lexicographically.
    [[nodiscard]] std::vector<Score> getPopular(size_t top_n) const;
    // The same for opts: whole characters or words, at least min_length long.
    void printPopular(std::ostream& out, const PopularOptions& opts = {}) const;

    // Every shortest substring that occurs exactly once, by position. Length
    // counts UTF-8 characters, and no result splits a character.
    [[nodiscard]] std::vector<UniqueSubstring> shortestUnique() const;
    // One line per substring; at most 1000 of them.
    void printShortestUnique(std::ostream& out, ListFormat format = ListFormat::Raw) const;

    [[nodiscard]] bool contains(std::string_view pattern) const;
    [[nodiscard]] int64_t count(std::string_view pattern) const;
    // Byte offsets of every occurrence of pattern, ascending.
    [[nodiscard]] std::vector<int> occurrences(std::string_view pattern) const;

    // Each pattern with its number of occurrences (with opts.words, only
    // those that start and end at a word boundary), formatted like the
    // repeats. JSON lines also list their positions, in characters.
    void printMatches(std::ostream& out, std::span<const std::string> patterns,
                      const PopularOptions& opts = {}) const;

    [[nodiscard]] const std::vector<int>& getSA() const { return sa; }
    [[nodiscard]] const std::vector<int>& getRank() const { return rank_; }
    [[nodiscard]] const std::vector<int>& getLCP() const { return lcp; }
    [[nodiscard]] const std::vector<int>& getPLCP() const { return plcp; }
    [[nodiscard]] const std::string& getText() const { return text; }
};
