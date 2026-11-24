#include "palindromes.h"
#include "utf8.h"
#include <algorithm>
#include <cstdint>
#include <print>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

using namespace std;

namespace {

// Double polynomial rolling hash over a codepoint string, O(1) substring hashes.
class PolyHash {
    static constexpr int64_t kMod1 = 1000000007LL;
    static constexpr int64_t kMod2 = 998244353LL;
    static constexpr int64_t kBase1 = 131;
    static constexpr int64_t kBase2 = 137;

    vector<int64_t> h1, h2, pw1, pw2;

public:
    explicit PolyHash(u32string_view s)
        : h1(s.size() + 1, 0), h2(s.size() + 1, 0),
          pw1(s.size() + 1, 1), pw2(s.size() + 1, 1) {
        for (size_t i = 0; i < s.size(); i++) {
            h1[i + 1] = (h1[i] * kBase1 + s[i]) % kMod1;
            h2[i + 1] = (h2[i] * kBase2 + s[i]) % kMod2;
            pw1[i + 1] = pw1[i] * kBase1 % kMod1;
            pw2[i + 1] = pw2[i] * kBase2 % kMod2;
        }
    }

    // Hash of s[pos, pos + len).
    [[nodiscard]] pair<int64_t, int64_t> get(int pos, int len) const {
        return {(h1[pos + len] - h1[pos] * pw1[len] % kMod1 + kMod1) % kMod1,
                (h2[pos + len] - h2[pos] * pw2[len] % kMod2 + kMod2) % kMod2};
    }
};

// Longest common extension of a[i..] and b[j..] (both of length n), by binary
// search on hashes.
int lce(const PolyHash& a, int i, const PolyHash& b, int j, int n) {
    int lo = 0, hi = min(n - i, n - j);
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (a.get(i, mid) == b.get(j, mid)) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

struct Palindrome {
    int start;
    int length;
};

} // namespace

void printPalindromes(ostream& out, string_view utf8_text, ListFormat format) {
    bool json = format == ListFormat::Json;
    // Palindrome-ness is a codepoint-level property (multi-byte UTF-8
    // sequences aren't individually reversible), so work on codepoints.
    u32string text = decodeUtf8(utf8_text);
    int n = static_cast<int>(text.size());
    if (n == 0) return;

    // Expanding outwards from a centre compares text going right with text
    // going left, i.e. the text against its reverse.
    PolyHash fwd(text);
    PolyHash rev(u32string(text.rbegin(), text.rend()));

    // The maximal palindrome around every centre. Reversed-text index n-1-k
    // corresponds to text index k.
    vector<Palindrome> found;
    for (int c = 0; c < n; c++) {
        // Odd length, centred on text[c]. Single characters don't count.
        int radius = lce(fwd, c, rev, n - 1 - c, n);  // includes text[c]
        if (radius >= 2)
            found.push_back({c - radius + 1, 2 * radius - 1});
    }
    for (int c = 1; c < n; c++) {
        // Even length, centred between text[c-1] and text[c].
        int radius = lce(fwd, c, rev, n - c, n);
        if (radius >= 1)
            found.push_back({c - radius, 2 * radius});
    }

    // Longest first, leftmost first; keep only the first occurrence of each.
    ranges::sort(found, [](const Palindrome& a, const Palindrome& b) {
        return tie(b.length, a.start) < tie(a.length, b.start);
    });
    set<tuple<int, pair<int64_t, int64_t>>> seen;
    vector<Palindrome> unique;
    for (const auto& p : found) {
        if (seen.emplace(p.length, fwd.get(p.start, p.length)).second)
            unique.push_back(p);
    }

    if (unique.empty() && !json) {
        std::println(out, "No palindromes found.");
        return;
    }

    constexpr size_t kMaxShown = 1000;
    size_t shown = min(unique.size(), kMaxShown);
    for (const auto& [start, length] : unique | views::take(shown)) {
        string pal = encodeUtf8(abbreviate(u32string_view(text).substr(start, length)));
        if (json)
            std::println(out, R"({{"type":"palindrome","text":{},"pos":{},"len":{}}})",
                         jsonString(pal), start, length);
        else
            std::println(out, "Length {} at position {}: \"{}\"", length, start,
                         format == ListFormat::Escaped ? escapeBytes(pal) : pal);
    }
    size_t omitted = unique.size() - shown;
    if (omitted > 0 && json)
        std::println(out, R"({{"type":"omitted","of":"palindrome","count":{}}})", omitted);
    else if (omitted > 0)
        std::println(out, "... and {} more unique palindromes.", omitted);
}
