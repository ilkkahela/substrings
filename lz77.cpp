#include "lz77.h"
#include "utf8.h"
#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

using namespace std;

namespace {

// Length of the common prefix of the suffixes at p and src (src < p, or -1
// for no candidate). Reading past p is fine: the copy may overlap itself.
int matchLength(string_view text, int p, int src) {
    if (src < 0) return 0;
    int n = static_cast<int>(text.size());
    int len = 0;
    while (p + len < n && text[src + len] == text[p + len]) len++;
    return len;
}

} // namespace

// The earlier suffix sharing the longest prefix with suffix p is, in SA order,
// the nearest one above or below p that starts before p (Kärkkäinen, Kempa,
// Puglisi 2013): any earlier suffix further away shares at most as much.
// One stack pass over SA finds both for every p, as psv[p] and nsv[p]. The
// stack holds text positions, increasing from the bottom; popping p when a
// smaller one arrives gives its next smaller value, and the entry left below
// it is its previous smaller value.
//
// The greedy loop then compares the text directly with both candidates. Each
// comparison stops within len + 1 bytes of the chosen phrase and phrase
// lengths sum to n, so the whole factorization is O(n).
vector<Lz77Phrase> lz77Factorize(const SuffixArray& suffix_array) {
    const auto& sa = suffix_array.getSA();
    string_view text = suffix_array.getText();
    int n = static_cast<int>(text.size());

    vector<int> psv(n), nsv(n);
    vector<int> stack;
    for (int i = 0; i <= n; i++) {
        int pos = i < n ? sa[i] : -1;  // -1 flushes the stack at the end
        while (!stack.empty() && stack.back() > pos) {
            int top = stack.back();
            stack.pop_back();
            nsv[top] = pos;
            psv[top] = stack.empty() ? -1 : stack.back();
        }
        stack.push_back(pos);
    }

    vector<Lz77Phrase> phrases;
    for (int p = 0; p < n;) {
        int above = matchLength(text, p, psv[p]);
        int below = matchLength(text, p, nsv[p]);
        // The longer match wins; on a tie, the closer (larger) source.
        bool use_above = above > below || (above == below && psv[p] > nsv[p]);
        int len = use_above ? above : below;
        int src = use_above ? psv[p] : nsv[p];

        if (len == 0)
            phrases.push_back({p, 1, -1});  // literal
        else
            phrases.push_back({p, len, src});
        p += phrases.back().len;
    }
    return phrases;
}

void printLz77(ostream& out, const SuffixArray& suffix_array) {
    auto phrases = lz77Factorize(suffix_array);
    string_view text = suffix_array.getText();

    // Written in chunks: one buffer would hold the whole output (~n bytes),
    // and per-line ostream writes are slow.
    constexpr size_t kChunkBytes = 1 << 20;
    string buf = "pos\tlen\tsrc\ttext\n";
    auto flush = [&] {
        out.write(buf.data(), static_cast<streamsize>(buf.size()));
        buf.clear();
    };

    int64_t literals = 0;
    int longest = 0;
    for (const auto& phrase : phrases) {
        if (phrase.src < 0) {
            literals++;
            format_to(back_inserter(buf), "{}\t{}\t-\t", phrase.pos, phrase.len);
        } else {
            format_to(back_inserter(buf), "{}\t{}\t{}\t", phrase.pos, phrase.len, phrase.src);
        }
        buf += escapeBytes(text.substr(phrase.pos, phrase.len));
        buf += '\n';
        longest = max(longest, phrase.len);
        if (buf.size() >= kChunkBytes) flush();
    }

    double avg_len = phrases.empty() ? 0.0 : static_cast<double>(text.size()) / phrases.size();
    format_to(back_inserter(buf), "# n={} z={} avg_len={:.2f} literals={} longest={}\n",
              text.size(), phrases.size(), avg_len, literals, longest);
    flush();
}
