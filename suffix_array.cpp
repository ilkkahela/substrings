#include "suffix_array.h"
#include "utf8.h"
#include <libsais.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <print>
#include <ranges>
#include <stdexcept>

using namespace std;

static_assert(sizeof(int) == sizeof(int32_t),
              "libsais uses int32_t; our sa/lcp/plcp are vector<int>");

namespace {

// libsais declares its parallel _omp variants only when built with OpenMP
// (see SUBSTRINGS_USE_OPENMP in CMakeLists.txt). 0 threads lets OpenMP choose.
#ifdef LIBSAIS_OPENMP
constexpr int32_t kThreads = 0;
int32_t computeSA(const uint8_t* t, int32_t* sa, int32_t n) {
    return libsais_omp(t, sa, n, 0, nullptr, kThreads);
}
int32_t computePLCP(const uint8_t* t, const int32_t* sa, int32_t* plcp, int32_t n) {
    return libsais_plcp_omp(t, sa, plcp, n, kThreads);
}
int32_t computeLCP(const int32_t* plcp, const int32_t* sa, int32_t* lcp, int32_t n) {
    return libsais_lcp_omp(plcp, sa, lcp, n, kThreads);
}
#else
int32_t computeSA(const uint8_t* t, int32_t* sa, int32_t n) {
    return libsais(t, sa, n, 0, nullptr);
}
int32_t computePLCP(const uint8_t* t, const int32_t* sa, int32_t* plcp, int32_t n) {
    return libsais_plcp(t, sa, plcp, n);
}
int32_t computeLCP(const int32_t* plcp, const int32_t* sa, int32_t* lcp, int32_t n) {
    return libsais_lcp(plcp, sa, lcp, n);
}
#endif

// Progress bar with throughput, drawn on stderr once the build has taken
// `delay` seconds, so that quick builds print nothing.
class BuildProgress {
    bool enabled;
    bool drawn = false;     // the bar is on screen
    bool finished = false;
    size_t bytes;
    double delay;
    chrono::steady_clock::time_point start = chrono::steady_clock::now();
    static constexpr int kBarWidth = 30;

    [[nodiscard]] double elapsed() const {
        return chrono::duration<double>(chrono::steady_clock::now() - start).count();
    }
    [[nodiscard]] double mbPerSec(double secs) const {
        return secs > 0.01 ? static_cast<double>(bytes) / (1024.0 * 1024.0) / secs : 0.0;
    }

public:
    BuildProgress(bool enabled, size_t bytes, double delay)
        : enabled(enabled), bytes(bytes), delay(delay) {}
    BuildProgress(const BuildProgress&) = delete;
    BuildProgress& operator=(const BuildProgress&) = delete;

    // If construction throws (e.g. bad_alloc), end the half-drawn bar's line
    // so the error message doesn't land on it.
    ~BuildProgress() {
        if (drawn && !finished) std::println(stderr, "");
    }

    void update(int percent) {
        if (!enabled || elapsed() < delay) return;
        drawn = true;
        int filled = percent * kBarWidth / 100;
        string bar(filled, '=');
        if (filled < kBarWidth) {
            bar += '>';
            bar += string(kBarWidth - filled - 1, ' ');
        }
        std::print(stderr, "\rBuilding... [{}] {:3d}%  {:.1f} MB/s  ",
                   bar, percent, mbPerSec(elapsed()));
    }

    void done() {
        finished = true;
        double secs = elapsed();
        bool slow = enabled && secs >= delay;  // the delay may run out in the last stage
        if (!drawn && !slow) return;
        std::println(stderr, "\r[{}] 100%  {:.1f} MB/s  {:.2f}s       ",
                     string(kBarWidth, '='), mbPerSec(secs), secs);
    }
};

const uint8_t* bytesOf(const string& s) {
    return reinterpret_cast<const uint8_t*>(s.data());
}

// Whether a character starts at byte i (the end of the text counts as one).
// A stray continuation byte belongs to the character before it.
bool isCharBoundary(const string& text, int i) {
    return i == 0 || i == static_cast<int>(text.size()) || !isUtf8Continuation(text[i]);
}

// One line of a repeats-style listing: "text count" (with opts' delimiters),
// or a JSON object of the given type.
void appendListLine(string& buf, string_view type, string_view text, int64_t count,
                    const PopularOptions& opts) {
    if (opts.format == ListFormat::Json) {
        format_to(back_inserter(buf), R"({{"type":"{}","text":{},"count":{}}})" "\n",
                  type, jsonString(text), count);
        return;
    }
    if (opts.format == ListFormat::Escaped) buf += escapeBytes(text);
    else buf += text;
    if (opts.show_count) format_to(back_inserter(buf), "{}{}", opts.delimiter, count);
    buf += opts.entry_delimiter;
}

} // namespace

SuffixArray::SuffixArray(string corpus, bool show_progress, double progress_delay)
    : text(std::move(corpus)) {
    if (text.size() > kMaxInputBytes)
        throw length_error("input exceeds 2 GiB, the limit of 32-bit libsais");

    BuildProgress progress(show_progress, text.size(), progress_delay);
    progress.update(0);
    buildSA();
    progress.update(50);
    buildRank();
    progress.update(60);
    buildPLCP();
    progress.update(90);
    buildLCP();
    progress.done();
}

void SuffixArray::buildSA() {
    int n = static_cast<int>(text.size());
    sa.resize(n);
    if (n > 0 && computeSA(bytesOf(text), sa.data(), n) != 0)
        throw runtime_error("libsais failed to build the suffix array");
}

void SuffixArray::buildRank() {
    rank_.resize(sa.size());
    for (int i = 0; i < static_cast<int>(sa.size()); i++)
        rank_[sa[i]] = i;
}

void SuffixArray::buildPLCP() {
    int n = static_cast<int>(text.size());
    plcp.resize(n);
    if (n > 0 && computePLCP(bytesOf(text), sa.data(), plcp.data(), n) != 0)
        throw runtime_error("libsais failed to build the PLCP array");
}

void SuffixArray::buildLCP() {
    int n = static_cast<int>(text.size());
    lcp.resize(n);
    if (n > 0 && computeLCP(plcp.data(), sa.data(), lcp.data(), n) != 0)
        throw runtime_error("libsais failed to build the LCP array");
}

namespace {

// What printPopular counts as a repeat: any byte string (getPopular), whole
// characters only, or whole words only (--words).
enum class Units { Bytes, Chars, Words };

// --words: a word is a run of characters whose lead byte is an ASCII letter
// or digit, or starts a character from U+00C0 on outside U+2000–U+2FFF
// (punctuation, quotes, dashes and symbols). Deciding by the lead byte alone
// makes a word end depend only on the bytes before it and the one byte after,
// which collectRepeats relies on.
bool isWordLead(char byte) {
    auto b = static_cast<unsigned char>(byte);
    if (b < 0x80)
        return (b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z');
    return b >= 0xC3 && b <= 0xF4 && b != 0xE2;
}

// The lead byte of the character that ends just before byte e > 0. A stray
// continuation byte belongs to the character before it.
char leadBefore(const string& text, int e) {
    int i = e - 1;
    while (i > 0 && e - i < 4 && isUtf8Continuation(text[i])) i--;
    return text[i];
}

bool isWordStart(const string& text, int p) {
    return p < static_cast<int>(text.size()) && isWordLead(text[p])
           && (p == 0 || !isWordLead(leadBefore(text, p)));
}

bool isWordEnd(const string& text, int e) {
    if (e == 0 || !isWordLead(leadBefore(text, e))) return false;
    return e == static_cast<int>(text.size())
           || (!isUtf8Continuation(text[e]) && !isWordLead(text[e]));
}

// Whether the bytes before e end with a whole character. That depends on
// those bytes alone, so every occurrence of a substring agrees on it.
bool endsWholeChar(const string& text, int e) {
    int i = e - 1, continuations = 0;
    while (i > 0 && continuations < 3 && isUtf8Continuation(text[i])) {
        i--;
        continuations++;
    }
    auto lead = static_cast<unsigned char>(text[i]);
    int length = (lead >= 0xF0 && lead <= 0xF4) ? 4
               : (lead >= 0xE0 && lead <= 0xEF) ? 3
               : (lead >= 0xC2 && lead <= 0xDF) ? 2
                                                : 1;  // ASCII, or a byte that is no lead
    return continuations + 1 >= length;
}

// The positions 1..n where pred holds, ascending.
template <class Pred>
vector<int> positionsWhere(const string& text, Pred pred) {
    vector<int> positions;
    for (int e = 1; e <= static_cast<int>(text.size()); e++)
        if (pred(e)) positions.push_back(e);
    return positions;
}

// The text positions a repeat may end at: a sorted list of either the
// allowed positions (word ends) or the excluded ones (character middles).
// Excluding nothing allows everything.
class EndFilter {
    vector<int> listed;
    bool listed_allowed = false;

public:
    EndFilter() = default;
    EndFilter(vector<int> positions, bool allowed)
        : listed(std::move(positions)), listed_allowed(allowed) {}

    // The allowed ends in [from, to].
    [[nodiscard]] int64_t count(int from, int to) const {
        int64_t in_list = ranges::upper_bound(listed, to) - ranges::lower_bound(listed, from);
        return listed_allowed ? in_list : (to - from + 1) - in_list;
    }

    // Calls f(e) for the allowed ends in [from, to] in order, until it returns false.
    template <class F>
    void forEach(int from, int to, F f) const {
        auto it = ranges::lower_bound(listed, from);
        if (listed_allowed) {
            for (; it != listed.end() && *it <= to; ++it)
                if (!f(*it)) return;
            return;
        }
        for (int e = from; e <= to; e++) {
            if (it != listed.end() && *it == e) {
                ++it;
                continue;
            }
            if (!f(e)) return;
        }
    }

    [[nodiscard]] const vector<int>& positions() const { return listed; }
};

// The suffixes sa[first..last] share their first `depth` bytes, and no other
// suffix does; parent_depth is what they share with their neighbours.
// `ending` counts the suffixes that, by the walk's ends_at, end a repeat at
// exactly depth.
struct LcpInterval {
    int first;
    int last;
    int depth;
    int parent_depth;
    int64_t ending;
};

// Visits every LCP interval, innermost first, with a monotonic stack. Its
// children (nested intervals and single suffixes) agree on the byte at depth,
// so asking ends_at(text position) about the first suffix of each child
// counts `ending` in O(1) per child.
template <class EndsAt, class Visit>
void forEachLcpInterval(span<const int> sa, span<const int> lcp, EndsAt ends_at, Visit visit) {
    struct Open {
        int depth;
        int first;
        int child_first;  // where the current child starts
        int64_t ending;
    };
    vector<Open> stack;
    auto endChild = [&](Open& open, int child_last) {
        if (ends_at(sa[open.child_first] + open.depth))
            open.ending += child_last - open.child_first + 1;
        open.child_first = child_last + 1;
    };

    int n = static_cast<int>(sa.size());
    for (int i = 1; i <= n; i++) {
        int cur = (i < n) ? lcp[i] : 0;  // shared by sa[i-1] and sa[i]
        int first = i - 1;
        while (!stack.empty() && stack.back().depth > cur) {
            Open top = stack.back();
            stack.pop_back();
            endChild(top, i - 1);
            int parent_depth = max(cur, stack.empty() ? 0 : stack.back().depth);
            visit(LcpInterval{top.first, i - 1, top.depth, parent_depth, top.ending});
            first = top.first;
        }
        if (cur == 0) continue;
        if (stack.empty() || stack.back().depth < cur)
            stack.push_back({cur, first, first, 0});
        endChild(stack.back(), i - 1);
    }
}

// What collectRepeats walks: the suffix array (for words, only the suffixes
// at word starts) and where repeats may end. min_length counts bytes, or
// words for Units::Words.
struct RepeatSource {
    const string& text;
    span<const int> sa;
    span<const int> lcp;
    Units units;
    EndFilter ends;
    int min_length;
};

// Repeats that occur freq times: text[pos, pos + len) for each allowed len in
// [shortest, longest] (all of them if any_end). Repeats sort
// lexicographically by order + len, where order comes from the SA index of
// the first suffix they start.
struct RepeatGroup {
    int64_t freq;
    int64_t order;
    int pos;
    int shortest;
    int longest;
    bool any_end;
};

// Each repeated substring is in exactly one group: an interval's prefixes of
// lengths (parent_depth, depth] occur exactly freq = last - first + 1 times.
//
// For words, the prefixes shorter than depth end at a word end in every
// suffix of the interval or in none, since the byte after them is shared.
// Whether the one of length depth does depends on the next byte, which
// differs; the walk counts the suffixes where it does (`ending`), and that is
// its own group, with a frequency of its own.
//
// Groups with equal freq come in lexicographic order, except that a word
// group of length depth comes after the groups of the longer phrases that
// start with it. collectRepeats sorts out the ties at the cutoff frequency.
template <class Visit>
void forEachRepeatGroup(const RepeatSource& src, Visit visit) {
    const string& text = src.text;
    auto stride = static_cast<int64_t>(text.size()) + 1;  // more than any len
    bool words = src.units == Units::Words;
    auto ends_at = [&](int e) { return words && isWordEnd(text, e); };

    forEachLcpInterval(src.sa, src.lcp, ends_at, [&](const LcpInterval& v) {
        int pos = src.sa[v.first];
        int64_t freq = v.last - v.first + 1;
        int64_t order = v.first * stride;

        if (!words) {
            if (src.units == Units::Chars && isUtf8Continuation(text[pos])) return;
            int shortest = max(v.parent_depth + 1, src.min_length);
            if (shortest <= v.depth)
                visit(RepeatGroup{freq, order, pos, shortest, v.depth, src.units == Units::Bytes});
            return;
        }

        // The word ends inside the shared prefix: after pos, before pos + depth.
        const auto& word_ends = src.ends.positions();
        auto after_pos = ranges::upper_bound(word_ends, pos);
        int64_t inside = ranges::lower_bound(word_ends, pos + v.depth) - after_pos;
        if (inside >= src.min_length) {
            int shortest = max(v.parent_depth + 1, after_pos[src.min_length - 1] - pos);
            visit(RepeatGroup{freq, order, pos, shortest, v.depth - 1, false});
        }
        if (inside + 1 >= src.min_length && v.ending >= 2)
            visit(RepeatGroup{v.ending, order, pos, v.depth, v.depth, true});
    });
}

int64_t groupSize(const RepeatSource& src, const RepeatGroup& g) {
    if (g.any_end) return g.longest - g.shortest + 1;
    return src.ends.count(g.pos + g.shortest, g.pos + g.longest);
}

// The top_n repeats are every repeat more frequent than `freq`, plus the
// first `quota` (lexicographically) of those occurring exactly `freq` times.
// freq == 0 when there are at most top_n repeats.
struct Cutoff {
    int64_t freq = 0;
    int64_t quota = 0;
};

// Counts repeats per frequency, keeping only the frequencies that can still
// reach the top_n, so the map stays small.
Cutoff findCutoff(const RepeatSource& src, int64_t top_n) {
    map<int64_t, int64_t> repeats_by_freq;
    int64_t total = 0;
    auto lowest = [&] { return repeats_by_freq.begin(); };

    forEachRepeatGroup(src, [&](const RepeatGroup& g) {
        if (total >= top_n && g.freq < lowest()->first) return;
        int64_t size = groupSize(src, g);
        if (size == 0) return;
        repeats_by_freq[g.freq] += size;
        total += size;
        while (total - lowest()->second >= top_n) {
            total -= lowest()->second;
            repeats_by_freq.erase(lowest());
        }
    });

    if (total < top_n) return {};
    int64_t more_frequent = total - lowest()->second;
    return {lowest()->first, top_n - more_frequent};
}

// The suffixes that start at a word start, in SA order, with their LCP
// array: the LCP of two of them is the least LCP of the suffixes between.
void wordSuffixes(const string& text, span<const int> sa, span<const int> lcp,
                  vector<int>& word_sa, vector<int>& word_lcp) {
    int shared = INT_MAX;
    for (size_t i = 0; i < sa.size(); i++) {
        if (i > 0) shared = min(shared, lcp[i]);
        if (!isWordStart(text, sa[i])) continue;
        word_lcp.push_back(word_sa.empty() ? 0 : shared);
        word_sa.push_back(sa[i]);
        shared = INT_MAX;
    }
}

struct Repeat {
    int pos;
    int len;
    int64_t freq;
    int64_t order;  // lexicographic
};

// Generating every repeated substring would take time quadratic in n for
// repetitive text (a text repeated twice has ~n²/8 of them). Instead, a first
// pass finds the cutoff frequency of the top_n, and the second generates
// only the repeats that make it.
//
// Returns the top_n by frequency (descending), ties broken lexicographically.
vector<Repeat> collectRepeats(const string& text, span<const int> sa, span<const int> lcp,
                              size_t top_n, int min_length, Units units) {
    if (top_n == 0) return {};
    vector<int> word_sa, word_lcp;
    RepeatSource src{text, sa, lcp, units, {}, min_length};
    if (units == Units::Words) {
        wordSuffixes(text, sa, lcp, word_sa, word_lcp);
        src.sa = word_sa;
        src.lcp = word_lcp;
        src.ends = EndFilter(positionsWhere(text, [&](int e) { return isWordEnd(text, e); }), true);
    } else if (units == Units::Chars) {
        src.ends = EndFilter(positionsWhere(text, [&](int e) { return !endsWholeChar(text, e); }),
                             false);
    }

    auto limit = static_cast<int64_t>(min<size_t>(top_n, INT64_MAX));
    auto [cutoff, quota] = findCutoff(src, limit);

    vector<Repeat> repeats;
    // The lexicographically first `quota` repeats occurring `cutoff` times,
    // as a max-heap by order. They come mostly in order, so most of them
    // are turned away at once.
    vector<Repeat> tied;
    auto by_order = [](const Repeat& a, const Repeat& b) { return a.order < b.order; };

    forEachRepeatGroup(src, [&](const RepeatGroup& g) {
        if (g.freq < cutoff) return;
        // Calls f(len) for the group's lengths, shortest first, until it returns false.
        auto forEachLength = [&](auto f) {
            if (g.any_end) {
                for (int len = g.shortest; len <= g.longest; len++)
                    if (!f(len)) return;
            } else {
                src.ends.forEach(g.pos + g.shortest, g.pos + g.longest,
                                 [&](int e) { return f(e - g.pos); });
            }
        };
        if (g.freq > cutoff) {
            forEachLength([&](int len) {
                repeats.push_back({g.pos, len, g.freq, g.order + len});
                return true;
            });
            return;
        }
        forEachLength([&](int len) {
            Repeat r{g.pos, len, g.freq, g.order + len};
            if (static_cast<int64_t>(tied.size()) < quota) {
                tied.push_back(r);
                ranges::push_heap(tied, by_order);
                return true;
            }
            if (r.order >= tied.front().order) return false;  // so are the longer ones
            ranges::pop_heap(tied, by_order);
            tied.back() = r;
            ranges::push_heap(tied, by_order);
            return true;
        });
    });

    repeats.append_range(tied);
    ranges::sort(repeats, [](const Repeat& a, const Repeat& b) {
        if (a.freq != b.freq) return a.freq > b.freq;
        return a.order < b.order;
    });
    return repeats;
}

} // namespace

vector<Score> SuffixArray::getPopular(size_t top_n) const {
    vector<Score> results;
    for (const auto& r : collectRepeats(text, sa, lcp, top_n, 1, Units::Bytes))
        results.push_back({text.substr(r.pos, r.len), r.freq});
    return results;
}

void SuffixArray::printPopular(ostream& out, const PopularOptions& opts) const {
    Units units = opts.words ? Units::Words : Units::Chars;
    int min_length = opts.min_length > 0 ? opts.min_length : opts.words ? 1 : 2;
    // Format into one buffer; per-entry ostream writes dominate runtime otherwise.
    string buf;
    for (const auto& r : collectRepeats(text, sa, lcp, opts.top_n, min_length, units))
        appendListLine(buf, "repeat", string_view(text).substr(r.pos, r.len), r.freq, opts);
    out.write(buf.data(), static_cast<streamsize>(buf.size()));
}

// A substring starting at p occurs only once iff it is longer than p's LCP
// with both SA neighbours, so the shortest one there has
//     L(p) = max(lcp[r], lcp[r+1]) + 1,   r = rank_[p],
// and exists only if p + L(p) <= n. Extending it to the next character
// boundary gives the shortest whole-character one. Since UTF-8 is
// self-synchronizing, a byte string starting at a character boundary can only
// match at character boundaries, so byte uniqueness is character uniqueness.
//
// The end p + L(p) never decreases as p grows (prefixing a unique substring
// keeps it unique), so one forward pointer counts characters up to it.
vector<UniqueSubstring> SuffixArray::shortestUnique() const {
    int n = static_cast<int>(text.size());
    vector<UniqueSubstring> shortest;
    int shortest_chars = INT_MAX;
    int chars_before_p = 0;
    int end = 0, chars_before_end = 0;

    for (int p = 0; p < n; p++) {
        if (!isCharBoundary(text, p)) continue;
        int r = rank_[p];
        int len = max(lcp[r], r + 1 < n ? lcp[r + 1] : 0) + 1;
        if (p + len <= n) {
            int stop = p + len;
            while (!isCharBoundary(text, stop)) stop++;
            for (; end < stop; end++)
                if (isCharBoundary(text, end)) chars_before_end++;

            int chars = chars_before_end - chars_before_p;
            if (chars < shortest_chars) {
                shortest_chars = chars;
                shortest.clear();
            }
            if (chars == shortest_chars)
                shortest.push_back({p, stop - p, chars_before_p, chars});
        }
        chars_before_p++;
    }
    return shortest;
}

void SuffixArray::printShortestUnique(ostream& out, ListFormat format) const {
    bool json = format == ListFormat::Json;
    auto found = shortestUnique();
    constexpr size_t kMaxShown = 1000;
    size_t shown = min(found.size(), kMaxShown);
    for (const auto& u : found | views::take(shown)) {
        string s = encodeUtf8(abbreviate(decodeUtf8(string_view(text).substr(u.pos, u.len))));
        if (json)
            std::println(out, R"({{"type":"unique","text":{},"pos":{},"len":{}}})",
                         jsonString(s), u.char_pos, u.char_len);
        else
            std::println(out, "Length {} at position {}: \"{}\"", u.char_len, u.char_pos,
                         format == ListFormat::Escaped ? escapeBytes(s) : s);
    }
    size_t omitted = found.size() - shown;
    if (omitted > 0 && json)
        std::println(out, R"({{"type":"omitted","of":"unique","count":{}}})", omitted);
    else if (omitted > 0)
        std::println(out, "... and {} more shortest unique substrings.", omitted);
}

pair<int, int> SuffixArray::matchRange(string_view pattern) const {
    // Cutting every suffix down to |pattern| bytes keeps them sorted, so a
    // binary search finds the ones equal to pattern.
    auto prefixAt = [&](int pos) { return string_view(text).substr(pos, pattern.size()); };
    auto [first, last] = ranges::equal_range(sa, pattern, {}, prefixAt);
    return {static_cast<int>(first - sa.begin()), static_cast<int>(last - sa.begin())};
}

bool SuffixArray::contains(string_view pattern) const {
    return count(pattern) > 0;
}

int64_t SuffixArray::count(string_view pattern) const {
    auto [first, last] = matchRange(pattern);
    return last - first;
}

vector<int> SuffixArray::occurrences(string_view pattern) const {
    auto [first, last] = matchRange(pattern);
    vector<int> positions(sa.begin() + first, sa.begin() + last);
    ranges::sort(positions);
    return positions;
}

void SuffixArray::printMatches(ostream& out, span<const string> patterns,
                               const PopularOptions& opts) const {
    string buf;
    for (const auto& pattern : patterns) {
        if (!opts.words && opts.format != ListFormat::Json) {
            appendListLine(buf, "match", pattern, count(pattern), opts);
            continue;
        }
        auto positions = occurrences(pattern);
        if (opts.words) {
            int len = static_cast<int>(pattern.size());
            erase_if(positions, [&](int p) {
                return !isWordStart(text, p) || !isWordEnd(text, p + len);
            });
        }
        if (opts.format != ListFormat::Json) {
            appendListLine(buf, "match", pattern, ssize(positions), opts);
            continue;
        }
        format_to(back_inserter(buf), R"({{"type":"match","text":{},"count":{},"pos":[)",
                  jsonString(pattern), positions.size());
        // Character offsets, counted in one sweep over the sorted byte offsets.
        int chars = 0, byte = 0;
        for (auto [k, pos] : positions | views::enumerate) {
            for (; byte < pos; byte++)
                if (isCharBoundary(text, byte)) chars++;
            format_to(back_inserter(buf), "{}{}", k == 0 ? "" : ",", chars);
        }
        buf += "]}\n";
    }
    out.write(buf.data(), static_cast<streamsize>(buf.size()));
}
