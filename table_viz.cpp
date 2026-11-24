#include "table_viz.h"
#include "utf8.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace std;

namespace {

// ANSI escape codes, or nothing when not coloring.
struct Palette {
    string_view reset, bold, reverse, cyan, green, yellow, magenta;
};
constexpr Palette kAnsi{"\033[0m", "\033[1m", "\033[7m", "\033[36m",
                        "\033[32m", "\033[33m", "\033[35m"};
constexpr Palette kPlain{};

const Palette& paletteFor(const VizOptions& opts) {
    return opts.color ? kAnsi : kPlain;
}

constexpr string_view kGap = "  ";  // between columns

int digitsOf(int value) {
    return static_cast<int>(to_string(value).size());
}

// Terminal columns taken by a printable character. A locale-independent
// approximation of wcwidth(): combining marks take 0, East Asian wide
// characters and most emoji 2, everything else 1.
int columnsOf(char32_t cp) {
    if (inAny(cp, {{0x0300, 0x036F}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF},
                   {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xE0100, 0xE01EF}}))
        return 0;
    if (inAny(cp, {{0x1100, 0x115F}, {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF},
                   {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF},
                   {0xFE30, 0xFE4F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F},
                   {0x1F900, 0x1F9FF}, {0x20000, 0x3FFFD}}))
        return 2;
    return 1;
}

// One character of a suffix as it is shown in the table.
struct Glyph {
    string text;
    int columns;
};

// Control characters are shown as Unicode control pictures (␊ for a line
// feed, ␛ for escape), invisible characters and invalid bytes as escapes, so
// the input can't move the cursor, recolor the terminal or hide text.
Glyph glyphFor(Utf8Char ch, string_view bytes) {
    char32_t cp = ch.codepoint;
    if (!ch.valid) return {format("\\x{:02X}", static_cast<unsigned char>(bytes[0])), 4};
    if (cp < 0x20) return {encodeUtf8(static_cast<char32_t>(0x2400 + cp)), 1};
    if (cp == 0x7F) return {encodeUtf8(U'␡'), 1};
    if (isInvisible(cp)) return {format("\\u{:04X}", static_cast<uint32_t>(cp)), 6};
    return {string(bytes), columnsOf(cp)};
}

// The start of a suffix, cut to max_columns without splitting a character.
// The first `shared` bytes (the LCP with the previous suffix) are bold; a
// character only partly inside them is not.
string renderSuffix(string_view suffix, size_t shared, int max_columns, const Palette& c) {
    string bold, rest;
    int used = 0;
    for (size_t i = 0; i < suffix.size();) {
        auto ch = decodeUtf8Char(suffix, i);
        auto glyph = glyphFor(ch, suffix.substr(i, ch.length));
        if (used + glyph.columns > max_columns) break;
        used += glyph.columns;
        i += ch.length;
        (i <= shared ? bold : rest) += glyph.text;
    }
    return format("{}{}{}{}", c.bold, bold, c.reset, rest);
}

struct Column {
    string_view header;
    string_view color;
    const vector<int>& values;
    int width = 0;
};

} // namespace

void printTables(ostream& out, const SuffixArray& suffix_array, const VizOptions& opts) {
    const Palette& c = paletteFor(opts);
    const auto& sa = suffix_array.getSA();
    const auto& text = suffix_array.getText();
    int n = static_cast<int>(text.size());
    if (n == 0) return;

    vector<Column> columns{
        {"SA[i]", c.cyan, sa},
        {"Rank[i]", c.green, suffix_array.getRank()},
        {"PLCP[i]", c.magenta, suffix_array.getPLCP()},
        {"LCP[i]", c.yellow, suffix_array.getLCP()},
    };

    // Each column is as wide as its header or its largest value (all < n).
    int value_width = digitsOf(n - 1);
    int index_width = value_width;
    int used_width = index_width;
    for (auto& column : columns) {
        column.width = max(static_cast<int>(column.header.size()), value_width);
        used_width += static_cast<int>(kGap.size()) + column.width;
    }
    int suffix_width = max(5, opts.width - used_width - static_cast<int>(kGap.size()));

    std::print(out, "{}{}{:<{}}{}", c.bold, c.reverse, "i", index_width, c.reset);
    for (const auto& column : columns)
        std::print(out, "{}{}{:<{}}{}", kGap, column.color, column.header, column.width, c.reset);
    std::println(out, "{}{}Sorted Suffix{}", kGap, c.bold, c.reset);

    for (int i = 0; i < n; i++) {
        std::print(out, "{:<{}}", i, index_width);
        for (const auto& column : columns)
            std::print(out, "{}{}{:<{}}{}", kGap, column.color, column.values[i], column.width, c.reset);

        // A visible character takes at most 4 bytes per column, so this is
        // enough text to fill the row without scanning the whole suffix.
        auto suffix = string_view(text).substr(sa[i], 4 * static_cast<size_t>(suffix_width));
        std::println(out, "{}{}", kGap, renderSuffix(suffix, suffix_array.getLCP()[i], suffix_width, c));
    }
}

namespace {

constexpr string_view kSentinel = "$";

// Whether a character starts at byte i (the end of the text counts as one).
bool isCharStart(string_view text, size_t i) {
    return i == 0 || i == text.size() || !isUtf8Continuation(text[i]);
}

// Rotations of text + "$" sort like the suffixes of text + "$", which (as "$"
// is smallest) is the SA order with the "$" suffix first. Keeping only
// character starts gives the character-level order, because UTF-8's byte
// order is its codepoint order. Returns the byte offset of each sorted
// rotation; the "$" rotation has offset n.
vector<int> rotationStarts(const SuffixArray& suffix_array) {
    const auto& text = suffix_array.getText();
    vector<int> starts{static_cast<int>(text.size())};
    for (int p : suffix_array.getSA())
        if (isCharStart(text, p)) starts.push_back(p);
    return starts;
}

// The L column entry of the rotation starting at byte s: the character ending
// at s, or the empty view for "$" (when s == 0).
string_view charBefore(string_view text, int s) {
    if (s == 0) return {};
    int q = s - 1;
    while (q > 0 && !isCharStart(text, q)) q--;
    return text.substr(q, s - q);
}

// One character as shown in a matrix cell (an invalid byte sequence is drawn
// byte by byte, so a cell can hold several glyphs).
Glyph cellFor(string_view ch, const Palette& c) {
    if (ch.empty()) return {format("{}{}{}", c.magenta, kSentinel, c.reset), 1};
    Glyph cell{"", 0};
    for (size_t i = 0; i < ch.size();) {
        auto decoded = decodeUtf8Char(ch, i);
        auto glyph = glyphFor(decoded, ch.substr(i, decoded.length));
        cell.text += glyph.text;
        cell.columns += glyph.columns;
        i += decoded.length;
    }
    return cell;
}

} // namespace

void printBwt(ostream& out, const SuffixArray& suffix_array, const VizOptions& opts) {
    const Palette& c = paletteFor(opts);
    string_view text = suffix_array.getText();
    auto starts = rotationStarts(suffix_array);
    int rows = static_cast<int>(starts.size());  // characters + 1 for "$"
    int original_row = static_cast<int>(ranges::find(starts, 0) - starts.begin());

    int64_t runs = 0;
    for (int r = 0; r < rows; r++)
        if (r == 0 || charBefore(text, starts[r]) != charBefore(text, starts[r - 1])) runs++;

    auto printSummary = [&](bool with_bwt) {
        if (with_bwt) {
            string bwt;
            for (int s : starts) bwt += cellFor(charBefore(text, s), c).text;
            std::println(out, "{}BWT{}    {}", c.bold, c.reset, bwt);
        }
        std::println(out, "{}Index{}  {} (the row of the original text)", c.bold, c.reset, original_row);
        std::println(out, "{}Runs{}   r={} in n={} characters, counting $ (n/r = {:.2f})",
                     c.bold, c.reset, runs, rows, static_cast<double>(rows) / runs);
    };

    if (rows - 1 > kMaxBwtMatrixChars) {
        std::println(stderr, "Note: the BWT matrix is drawn only for inputs up to {} characters",
                     kMaxBwtMatrixChars);
        printSummary(false);
        return;
    }

    // Character k of the text is cells[k]; cells[m] is "$". Rotation r is
    // cells[(k + j) % (m + 1)] for j = 0..m, where k is its first character.
    vector<Glyph> cells;
    vector<int> char_index(text.size() + 1);
    for (size_t p = 0, k = 0; p <= text.size(); p++) {
        if (!isCharStart(text, p)) continue;
        char_index[p] = static_cast<int>(k++);
        if (p < text.size()) {
            size_t end = p + 1;
            while (!isCharStart(text, end)) end++;
            cells.push_back(cellFor(text.substr(p, end - p), c));
        }
    }
    cells.push_back(cellFor({}, c));

    // Every cell is as wide as the widest one, so the columns line up.
    int cell_width = ranges::max(cells | views::transform(&Glyph::columns));
    auto padded = [cell_width](const Glyph& g) { return g.text + string(cell_width - g.columns, ' '); };

    int gap = static_cast<int>(kGap.size());
    int index_width = max(1, digitsOf(rows - 1));
    int pos_width = max(3, digitsOf(rows - 1));
    int width = opts.width;
    // Prefix columns, F, L, and the " ◀" marker after L.
    int fixed = index_width + gap + pos_width + gap + cell_width + gap + gap + cell_width + 2;
    int middle = max(0, rows - 2);
    int shown = middle;
    bool elided = false;
    if (middle * cell_width > width - fixed) {
        shown = max(0, (width - fixed - 1) / cell_width);  // 1 column for "…"
        elided = true;
    }
    int middle_columns = shown * cell_width + (elided ? 1 : 0);

    auto headerCell = [](string_view title, int columns) {
        return format("{:<{}}{}", title, columns, kGap);
    };
    string_view label = middle_columns >= 8 ? "Rotation" : "";
    std::println(out, "{}{}{}{}{}{}L{}", c.bold, c.reverse,
                 headerCell("i", index_width), headerCell("Pos", pos_width),
                 headerCell("F", cell_width), headerCell(label, middle_columns), c.reset);

    for (int r = 0; r < rows; r++) {
        int k = char_index[starts[r]];
        auto cellAt = [&](int j) -> const Glyph& { return cells[(k + j) % rows]; };

        string row = format("{:<{}}{}{}{:<{}}{}{}", r, index_width, kGap,
                            c.cyan, k, pos_width, c.reset, kGap);
        row += format("{}{}{}{}{}", c.bold, c.green, padded(cellAt(0)), c.reset, kGap);
        for (int j = 1; j <= shown; j++) row += padded(cellAt(j));
        if (elided) row += "…";
        row += format("{}{}{}{}{}", kGap, c.bold, c.yellow, padded(cellAt(rows - 1)), c.reset);
        if (r == original_row) row += " ◀";
        std::println(out, "{}", row);
    }
    std::println(out, "");
    printSummary(true);
}
