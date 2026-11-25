// Unit tests for substrings.
//
// Sections:
//   1. Command-line parsing
//   2. Suffix array construction and search
//   3. Suffix tree structure
//   4. Popular (repeated) substrings
//   5. Shortest unique substrings
//   6. Palindromes
//   7. UTF-8 conversion
//   8. Internal tables view
//   9. Progress bar on stderr
//  10. LZ77 factorization
//  11. Burrows-Wheeler matrix

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <regex>
#include <sstream>
#include "cli.h"
#include "completions.h"
#include "lz77.h"
#include "palindromes.h"
#include "suffix_array.h"
#include "suffix_tree.h"
#include "table_viz.h"
#include "terminal.h"
#include "utf8.h"

#include <gtest/gtest.h>

using namespace std;

namespace {

// Runs parseArgs as if the user typed `substrings <flags...>`.
expected<Args, string> runParser(initializer_list<const char*> flags) {
    vector<const char*> argv{"substrings"};
    argv.insert(argv.end(), flags);
    return parseArgs(static_cast<int>(argv.size()), argv.data());
}

// Like runParser, but fails the test with the parser's message if it rejects
// the arguments.
Args parse(initializer_list<const char*> flags) {
    auto result = runParser(flags);
    EXPECT_TRUE(result.has_value()) << "parseArgs failed: " << result.error();
    return result.value_or(Args{});
}

bool parseFails(initializer_list<const char*> flags) {
    return !runParser(flags).has_value();
}

int countLeaves(const SuffixTree& tree) {
    return static_cast<int>(ranges::count_if(
        tree.nodes, [](const SuffixTreeNode& node) { return node.leaf_sa_idx >= 0; }));
}

// Every repeated substring of `text` mapped to how many times it occurs.
map<string, int64_t> repeatedSubstrings(const string& text) {
    map<string, int64_t> counts;
    for (const auto& score : SuffixArray(text).getPopular(SIZE_MAX))
        counts[score.substring] = score.frequency;
    return counts;
}

string printPopular(const string& text, const PopularOptions& opts = {}) {
    ostringstream out;
    SuffixArray(text).printPopular(out, opts);
    return out.str();
}

string shortestUniqueOf(const string& text) {
    ostringstream out;
    SuffixArray(text).printShortestUnique(out);
    return out.str();
}

// Shortest unique substrings by brute force over codepoints, as
// (character position, character length) pairs in position order.
vector<pair<int, int>> bruteForceShortestUnique(const string& utf8) {
    u32string text = decodeUtf8(utf8);
    int n = static_cast<int>(text.size());
    auto occurrences = [&](u32string_view pattern) {
        int count = 0;
        for (size_t at = text.find(pattern); at != u32string::npos; at = text.find(pattern, at + 1))
            count++;
        return count;
    };
    for (int len = 1; len <= n; len++) {
        vector<pair<int, int>> found;
        for (int pos = 0; pos + len <= n; pos++)
            if (occurrences(u32string_view(text).substr(pos, len)) == 1)
                found.emplace_back(pos, len);
        if (!found.empty()) return found;
    }
    return {};
}

string palindromesOf(string_view text) {
    ostringstream out;
    printPalindromes(out, text);
    return out.str();
}

string tablesOf(const string& text, int width = 200) {
    ostringstream out;
    printTables(out, SuffixArray(text), {.width = width});
    return out.str();
}

// The "Sorted Suffix" column of the internal tables, colors removed.
vector<string> tableSuffixes(const string& text, int width = 200) {
    static const regex kAnsiColor("\x1b\\[[0-9;]*m");
    istringstream lines(regex_replace(tablesOf(text, width), kAnsiColor, ""));
    string header, line;
    getline(lines, header);
    size_t column = header.find("Sorted Suffix");  // the columns before it are ASCII
    vector<string> suffixes;
    while (getline(lines, line))
        suffixes.push_back(line.substr(column));
    return suffixes;
}

// Everything `action` writes to stderr.
string stderrDuring(const function<void()>& action) {
    int pipe_fds[2];
    EXPECT_EQ(pipe(pipe_fds), 0);
    int saved_stderr = dup(STDERR_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[1]);

    action();

    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    // Non-blocking so an empty pipe doesn't hang the read.
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    string captured;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipe_fds[0], buf, sizeof(buf))) > 0)
        captured.append(buf, n);
    close(pipe_fds[0]);
    return captured;
}

// Builds a suffix array and returns everything it wrote to stderr.
string stderrWhileBuilding(const string& text, bool show_progress, double delay = 0) {
    return stderrDuring([&] { SuffixArray sa(text, show_progress, delay); });
}

}  // namespace

// ============================================================================
// 1. Command-line parsing
// ============================================================================

TEST(ArgParsing, DefaultsWhenNoArguments) {
    auto args = parse({});
    EXPECT_FALSE(args.inline_input);
    EXPECT_FALSE(args.input_file);
    EXPECT_TRUE(args.output_file.empty());
    EXPECT_TRUE(args.dot_file.empty());
    EXPECT_FALSE(args.view_tables);
    EXPECT_FALSE(args.palindrome);

    EXPECT_TRUE(args.popular.show_count);
    EXPECT_EQ(args.popular.min_length, 0);  // the default: 2 bytes, or 1 word
    EXPECT_EQ(args.popular.delimiter, " ");
    EXPECT_EQ(args.popular.entry_delimiter, "\n");
}

TEST(ArgParsing, Lz77Flag) {
    EXPECT_FALSE(parse({}).lz77);
    EXPECT_TRUE(parse({"--lz77", "banana"}).lz77);
}

TEST(ArgParsing, BwtFlag) {
    EXPECT_FALSE(parse({}).bwt);
    EXPECT_TRUE(parse({"--bwt", "banana"}).bwt);
}

TEST(ArgParsing, BareArgumentIsAFile) {
    auto args = parse({"banana.txt"});
    EXPECT_EQ(args.input_file, "banana.txt");
    EXPECT_FALSE(args.inline_input);
}

TEST(ArgParsing, InlineMakesInputTheText) {
    EXPECT_FALSE(parse({}).inline_text);
    auto args = parse({"--inline", "banana"});
    EXPECT_EQ(args.inline_input, "banana");
    EXPECT_FALSE(args.input_file);
    EXPECT_EQ(parse({"banana", "--inline"}).inline_input, "banana");  // in any order
    EXPECT_EQ(parse({"--inline", ""}).inline_input, "");
    EXPECT_EQ(parse({"--inline", "-"}).inline_input, "-");  // text, not stdin
    EXPECT_EQ(parse({"--inline", "--", "-x-x-"}).inline_input, "-x-x-");
}

TEST(ArgParsing, InlineWithoutTextIsAnError) {
    auto result = runParser({"--inline"});
    ASSERT_FALSE(result);
    EXPECT_TRUE(result.error().contains("--inline TEXT")) << result.error();
    EXPECT_TRUE(parseFails({"--inline", "-i", "in.txt"}));
}

TEST(ArgParsing, DashIReadsFromFile) {
    auto args = parse({"-i", "pg100.txt"});
    EXPECT_EQ(args.input_file, "pg100.txt");
    EXPECT_FALSE(args.inline_input);
}

TEST(ArgParsing, DashOWritesToFile) {
    EXPECT_EQ(parse({"-o", "out.txt"}).output_file, "out.txt");
}

TEST(ArgParsing, HelpAndVersion) {
    EXPECT_TRUE(parse({"--help"}).show_help);
    EXPECT_TRUE(parse({"-h"}).show_help);
    EXPECT_TRUE(parse({"--version"}).show_version);
}

TEST(ArgParsing, ModeFlags) {
    EXPECT_TRUE(parse({"--view-internal-tables"}).view_tables);
    EXPECT_TRUE(parse({"--palindrome"}).palindrome);
    EXPECT_TRUE(parse({"--shortest-unique"}).shortest_unique);
    EXPECT_FALSE(parse({}).shortest_unique);
}

TEST(ArgParsing, DotfileSetsGraphOutputPath) {
    EXPECT_EQ(parse({"--dotfile=tree.dot"}).dot_file, "tree.dot");
}

TEST(ArgParsing, LegacyGraphFlagUsesDefaultDotfile) {
    EXPECT_EQ(parse({"--graph"}).dot_file, "suffix_tree.dot");
}

TEST(ArgParsing, ForceGraphLiftsTheSizeLimit) {
    EXPECT_FALSE(parse({"--graph"}).force_graph);
    EXPECT_TRUE(parse({"--graph", "--force-graph"}).force_graph);
}

TEST(ArgParsing, DotfileOverridesLegacyGraphFlag) {
    EXPECT_EQ(parse({"--graph", "--dotfile=custom.dot"}).dot_file, "custom.dot");
}

TEST(ArgParsing, AllFlagsTogether) {
    auto args = parse({"-i", "in.txt", "-o", "out.txt", "--view-internal-tables",
                       "--palindrome", "--dotfile=tree.dot"});
    EXPECT_EQ(args.input_file, "in.txt");
    EXPECT_EQ(args.output_file, "out.txt");
    EXPECT_TRUE(args.view_tables);
    EXPECT_TRUE(args.palindrome);
    EXPECT_EQ(args.dot_file, "tree.dot");
    EXPECT_FALSE(args.inline_input);
}

TEST(ArgParsing, InputAfterFlags) {
    auto args = parse({"--view-internal-tables", "--palindrome", "hello.txt"});
    EXPECT_TRUE(args.view_tables);
    EXPECT_TRUE(args.palindrome);
    EXPECT_EQ(args.input_file, "hello.txt");
    EXPECT_FALSE(args.inline_input);
}

TEST(ArgParsing, FlagOrderDoesNotMatter) {
    auto first = parse({"-o", "out.txt", "-i", "in.txt", "--palindrome"});
    auto second = parse({"--palindrome", "-i", "in.txt", "-o", "out.txt"});
    EXPECT_EQ(first.input_file, second.input_file);
    EXPECT_EQ(first.output_file, second.output_file);
    EXPECT_EQ(first.palindrome, second.palindrome);
}

// "-i" or "-o" as the last argument has no value to take.
TEST(ArgParsing, DanglingDashIIsAnError) {
    EXPECT_TRUE(parseFails({"-i"}));
}

TEST(ArgParsing, DanglingDashOIsAnError) {
    EXPECT_TRUE(parseFails({"-o"}));
}

TEST(ArgParsing, ValueIsNotTakenFromTheNextOption) {
    EXPECT_TRUE(parseFails({"-o", "--lz77"}));
    EXPECT_TRUE(parseFails({"--top", "--nocount"}));
}

TEST(ArgParsing, UnknownOptionIsAnError) {
    EXPECT_TRUE(parseFails({"--palindrom", "-i", "in.txt"}));
    EXPECT_TRUE(parseFails({"-x"}));
}

TEST(ArgParsing, UnknownOptionSuggestsACloseOne) {
    EXPECT_TRUE(runParser({"--palindrom"}).error().contains("'--palindrome'"));
    EXPECT_TRUE(runParser({"--min-length=3"}).error().contains("'--minlength'"));
    EXPECT_TRUE(runParser({"--shortest"}).error().contains("'--shortest-unique'"));
    EXPECT_FALSE(runParser({"--frobnicate"}).error().contains("did you mean"));
}

TEST(ArgParsing, FlagWithValueIsAnError) {
    EXPECT_TRUE(parseFails({"--lz77=yes"}));
}

TEST(ArgParsing, DoubleDashEndsOptions) {
    EXPECT_EQ(parse({"--", "--lz77"}).input_file, "--lz77");
    EXPECT_EQ(parse({"--inline", "--", "--lz77"}).inline_input, "--lz77");
    EXPECT_FALSE(parse({"--", "--lz77"}).lz77);
}

TEST(ArgParsing, DashReadsStdin) {
    EXPECT_FALSE(parse({"-"}).inline_input);
    EXPECT_EQ(parse({"-"}).input_file, "-");
    EXPECT_EQ(parse({"--", "-"}).input_file, "-");
    EXPECT_EQ(parse({"-i", "-"}).input_file, "-");
}

TEST(ArgParsing, DashOutputIsStdout) {
    EXPECT_TRUE(parse({"-o", "-"}).output_file.empty());
}

TEST(ArgParsing, MoreThanOneInputIsAnError) {
    auto result = runParser({"hello", "world"});
    ASSERT_FALSE(result);
    EXPECT_TRUE(result.error().contains("'hello', 'world'"));
    EXPECT_TRUE(result.error().contains("--inline"));  // the likely intent
    EXPECT_TRUE(parseFails({"--inline", "hello", "world"}));
}

TEST(ArgParsing, InputAndDashITogetherIsAnError) {
    EXPECT_TRUE(parseFails({"-i", "in.txt", "other.txt"}));
    EXPECT_TRUE(parseFails({"-i", "in.txt", "-"}));
    EXPECT_TRUE(parseFails({"-i", "in.txt", "--inline", "banana"}));
}

TEST(ArgParsing, ValuesMayFollowAsTheNextArgument) {
    auto args = parse({"--minlength", "5", "--top", "7", "--dotfile", "t.dot", "--delimiter", ";"});
    EXPECT_EQ(args.popular.min_length, 5);
    EXPECT_EQ(args.popular.top_n, 7u);
    EXPECT_EQ(args.dot_file, "t.dot");
    EXPECT_EQ(args.popular.delimiter, ";");
}

TEST(ArgParsing, ShortOptionsDontSplitAtEquals) {
    EXPECT_EQ(parse({"-i", "a=b.txt"}).input_file, "a=b.txt");
    EXPECT_TRUE(parseFails({"-i=a.txt"}));
}

TEST(ArgParsing, TopSetsTheNumberOfRepeats) {
    EXPECT_EQ(parse({}).popular.top_n, 100000u);
    EXPECT_EQ(parse({"--top=10"}).popular.top_n, 10u);
    EXPECT_EQ(parse({"-n", "3"}).popular.top_n, 3u);
    EXPECT_TRUE(parseFails({"--top=0"}));
    EXPECT_TRUE(parseFails({"--top=-1"}));
    EXPECT_TRUE(parseFails({"--top=12abc"}));
}

TEST(ArgParsing, ColorModes) {
    EXPECT_EQ(parse({}).color, When::Auto);
    EXPECT_EQ(parse({"--color"}).color, When::Always);
    EXPECT_EQ(parse({"--color=always"}).color, When::Always);
    EXPECT_EQ(parse({"--color=never"}).color, When::Never);
    EXPECT_EQ(parse({"--color=never", "--color=auto"}).color, When::Auto);
    EXPECT_TRUE(parseFails({"--color=sometimes"}));
    // --color takes its value only after "=", so this is INPUT.
    EXPECT_EQ(parse({"--color", "banana.txt"}).input_file, "banana.txt");
}

TEST(ArgParsing, NoPager) {
    EXPECT_TRUE(parse({}).use_pager);
    EXPECT_FALSE(parse({"--no-pager"}).use_pager);
}

TEST(ArgParsing, LongFormsOfInputAndOutput) {
    auto args = parse({"--input=in.txt", "--output", "out.txt"});
    EXPECT_EQ(args.input_file, "in.txt");
    EXPECT_EQ(args.output_file, "out.txt");
}

TEST(ArgParsing, EscapeModes) {
    EXPECT_EQ(parse({}).escape, When::Auto);
    EXPECT_EQ(parse({"--escape"}).escape, When::Always);
    EXPECT_EQ(parse({"--escape=never"}).escape, When::Never);
    EXPECT_TRUE(parseFails({"--escape=maybe"}));
}

TEST(ArgParsing, FindIsRepeatableAndUnescaped) {
    EXPECT_TRUE(parse({}).find_patterns.empty());
    auto args = parse({"--find=the", "--find", R"(\r\n)", "--json"});
    EXPECT_EQ(args.find_patterns, (vector<string>{"the", "\r\n"}));
    EXPECT_TRUE(parseFails({"--find="}));
}

TEST(ArgParsing, JsonFlag) {
    EXPECT_FALSE(parse({}).json);
    EXPECT_TRUE(parse({"--json", "--palindrome"}).json);
}

TEST(ArgParsing, JsonDoesNotCombineWithTableModes) {
    EXPECT_TRUE(parseFails({"--json", "--lz77"}));
    EXPECT_TRUE(parseFails({"--json", "--bwt"}));
    EXPECT_TRUE(parseFails({"--json", "--view-internal-tables"}));
}

TEST(ArgParsing, WordsFlag) {
    EXPECT_FALSE(parse({}).popular.words);
    Args args = parse({"--words", "--minlength=3"});
    EXPECT_TRUE(args.popular.words);
    EXPECT_EQ(args.popular.min_length, 3);  // words, with --words
    EXPECT_TRUE(parseFails({"--words", "--lz77"}));
    EXPECT_TRUE(parseFails({"--words", "--bwt"}));
    EXPECT_TRUE(parseFails({"--words", "--view-internal-tables"}));
}

TEST(ArgParsing, CompletionsTakesAKnownShell) {
    EXPECT_EQ(parse({"--completions=zsh"}).completions_shell, "zsh");
    EXPECT_EQ(parse({"--completions", "fish"}).completions_shell, "fish");
    auto result = runParser({"--completions=tcsh"});
    ASSERT_FALSE(result);
    EXPECT_TRUE(result.error().contains("bash, zsh, fish")) << result.error();
}

TEST(ArgParsing, EveryOptionIsInTheUsage) {
    string usage = usageText();
    for (const auto& option : options()) {
        EXPECT_TRUE(usage.contains(option.name)) << option.name;
        EXPECT_FALSE(option.help.empty()) << option.name;
    }
}

TEST(ArgParsing, NonNumericMinlengthIsAnError) {
    EXPECT_TRUE(parseFails({"--minlength=abc"}));
    EXPECT_TRUE(parseFails({"--minlength=5x"}));
    EXPECT_TRUE(parseFails({"--minlength=0"}));
}

TEST(ArgParsing, HelpWinsOverOtherErrors) {
    EXPECT_TRUE(parse({"--minlength=abc", "--help"}).show_help);
    EXPECT_TRUE(parse({"--bogus", "hello", "world", "--help"}).show_help);
}

// --- Options for the popular-substring listing ---

TEST(ArgParsing, NocountHidesCounts) {
    EXPECT_FALSE(parse({"--nocount"}).popular.show_count);
}

TEST(ArgParsing, MinlengthSetsMinimumLength) {
    EXPECT_EQ(parse({"--minlength=5"}).popular.min_length, 5);
}

TEST(ArgParsing, DelimiterSeparatesSubstringAndCount) {
    EXPECT_EQ(parse({"--delimiter=;"}).popular.delimiter, ";");
}

TEST(ArgParsing, EntryDelimiterSeparatesEntries) {
    EXPECT_EQ(parse({"--entry-delimiter=, "}).popular.entry_delimiter, ", ");
}

// Delimiters accept backslash escapes, so `--delimiter='\t'` on a shell
// gives a real tab.
TEST(ArgParsing, DelimitersUnderstandEscapes) {
    EXPECT_EQ(parse({R"(--delimiter=\t)"}).popular.delimiter, "\t");
    EXPECT_EQ(parse({R"(--delimiter=\r\n)"}).popular.delimiter, "\r\n");
    EXPECT_EQ(parse({R"(--delimiter=\\)"}).popular.delimiter, "\\");
    EXPECT_EQ(parse({R"(--entry-delimiter=\n\n)"}).popular.entry_delimiter, "\n\n");
}

// ============================================================================
// 2. Suffix array construction and search
// ============================================================================

// Sorted suffixes of "banana":
//
//   rank  pos  suffix   lcp with previous
//     0    5   a        0
//     1    3   ana      1  (a)
//     2    1   anana    3  (ana)
//     3    0   banana   0
//     4    4   na       0
//     5    2   nana     2  (na)
TEST(SuffixArray, BananaTables) {
    SuffixArray sa("banana");
    EXPECT_EQ(sa.getSA(),   (vector<int>{5, 3, 1, 0, 4, 2}));
    EXPECT_EQ(sa.getRank(), (vector<int>{3, 2, 5, 1, 4, 0}));
    EXPECT_EQ(sa.getLCP(),  (vector<int>{0, 1, 3, 0, 0, 2}));
    // The same LCP values, indexed by text position instead of rank.
    EXPECT_EQ(sa.getPLCP(), (vector<int>{0, 3, 2, 1, 0, 0}));
}

// Sorted suffixes of "mississippi":
//
//   rank  pos  suffix        lcp with previous
//     0   10   i             0
//     1    7   ippi          1  (i)
//     2    4   issippi       1  (i)
//     3    1   ississippi    4  (issi)
//     4    0   mississippi   0
//     5    9   pi            0
//     6    8   ppi           1  (p)
//     7    6   sippi         0
//     8    3   sissippi      2  (si)
//     9    5   ssippi        1  (s)
//    10    2   ssissippi     3  (ssi)
TEST(SuffixArray, MississippiTables) {
    SuffixArray sa("mississippi");
    EXPECT_EQ(sa.getSA(),   (vector<int>{10, 7, 4, 1, 0, 9, 8, 6, 3, 5, 2}));
    EXPECT_EQ(sa.getRank(), (vector<int>{4, 3, 10, 8, 2, 9, 7, 1, 6, 5, 0}));
    EXPECT_EQ(sa.getLCP(),  (vector<int>{0, 1, 1, 4, 0, 0, 1, 0, 2, 1, 3}));
    EXPECT_EQ(sa.getPLCP(), (vector<int>{0, 4, 3, 2, 1, 1, 0, 1, 1, 0, 0}));
}

TEST(SuffixArray, SingleCharacterTables) {
    SuffixArray sa("y");
    EXPECT_EQ(sa.getSA(),   vector<int>{0});
    EXPECT_EQ(sa.getRank(), vector<int>{0});
    EXPECT_EQ(sa.getLCP(),  vector<int>{0});
    EXPECT_EQ(sa.getPLCP(), vector<int>{0});
}

TEST(SuffixArray, EmptyTextHasEmptyTables) {
    SuffixArray sa("");
    EXPECT_TRUE(sa.getSA().empty());
    EXPECT_TRUE(sa.getRank().empty());
    EXPECT_TRUE(sa.getLCP().empty());
    EXPECT_TRUE(sa.getPLCP().empty());
}

TEST(SuffixArray, ContainsFindsSubstrings) {
    SuffixArray banana("banana");
    EXPECT_TRUE(banana.contains("ban"));
    EXPECT_TRUE(banana.contains("ana"));
    EXPECT_TRUE(banana.contains("banana"));

    SuffixArray mississippi("mississippi");
    EXPECT_TRUE(mississippi.contains("iss"));
    EXPECT_TRUE(mississippi.contains("issi"));
    EXPECT_TRUE(mississippi.contains("mississippi"));

    EXPECT_TRUE(SuffixArray("y").contains("y"));
}

TEST(SuffixArray, ContainsRejectsNonSubstrings) {
    SuffixArray banana("banana");
    EXPECT_FALSE(banana.contains("banan!"));
    EXPECT_FALSE(banana.contains("z"));

    SuffixArray mississippi("mississippi");
    EXPECT_FALSE(mississippi.contains("blergh"));
    EXPECT_FALSE(mississippi.contains("z"));

    EXPECT_FALSE(SuffixArray("y").contains("yy"));
    EXPECT_FALSE(SuffixArray("").contains("a"));
}

// ============================================================================
// 3. Suffix tree structure
//
// buildSuffixTree appends a sentinel, so a text of length n gives n + 1
// suffixes and therefore n + 1 leaves.
// ============================================================================

TEST(SuffixTree, OneLeafPerSuffix) {
    EXPECT_EQ(countLeaves(buildSuffixTree("a")), 2);
    EXPECT_EQ(countLeaves(buildSuffixTree("banana")), 7);
    EXPECT_EQ(countLeaves(buildSuffixTree("aaaaaa")), 7);
}

TEST(SuffixTree, RootIsNodeZero) {
    auto tree = buildSuffixTree("banana");
    const auto& root = tree.nodes[0];
    EXPECT_EQ(root.parent, -1);
    EXPECT_EQ(root.depth, 0);
    EXPECT_EQ(root.leaf_sa_idx, -1) << "root should not be a leaf";
}

TEST(SuffixTree, EveryNodeIsInItsParentsChildren) {
    auto tree = buildSuffixTree("mississippi");
    for (int i = 1; i < ssize(tree.nodes); i++) {
        int parent = tree.nodes[i].parent;
        ASSERT_GE(parent, 0) << "node " << i << " has no parent";
        const auto& siblings = tree.nodes[parent].children;
        EXPECT_TRUE(ranges::contains(siblings, i))
            << "node " << i << " is missing from the children of its parent " << parent;
    }
}

TEST(SuffixTree, EverySuffixMapsToALeaf) {
    auto tree = buildSuffixTree("banana");
    for (int rank = 0; rank < ssize(tree.sa); rank++) {
        int node = tree.leaf_node[rank];
        ASSERT_GE(node, 0) << "suffix of rank " << rank << " has no leaf";
        EXPECT_GE(tree.nodes[node].leaf_sa_idx, 0)
            << "suffix of rank " << rank << " maps to internal node " << node;
    }
}

// Regression: internal nodes used to get any_leaf_pos = -1, which crashed
// code that read the text at that position.
TEST(SuffixTree, EveryNodeHasALeafPosition) {
    for (const char* text : {"banana", "mississippi", "abcabc", "aaaaaa"}) {
        auto tree = buildSuffixTree(text);
        for (int i = 0; i < ssize(tree.nodes); i++)
            EXPECT_GE(tree.any_leaf_pos[i], 0) << "node " << i << " of \"" << text << '"';
    }
}

// A suffix link goes from the node for "xα" to the node for "α", which is
// exactly one character shallower.
TEST(SuffixTree, SuffixLinksDropOneCharacter) {
    for (const char* text : {"banana", "mississippi", "abcabxabcd"}) {
        auto tree = buildSuffixTree(text);
        for (int i = 1; i < ssize(tree.nodes); i++) {
            const auto& node = tree.nodes[i];
            if (node.leaf_sa_idx >= 0) continue;  // only internal nodes have links
            ASSERT_GE(node.suffix_link, 0) << "node " << i << " of \"" << text << '"';
            EXPECT_EQ(tree.nodes[node.suffix_link].depth, node.depth - 1)
                << "node " << i << " of \"" << text << '"';
        }
    }
}

TEST(SuffixTree, WritesGraphvizDot) {
    ostringstream out;
    buildSuffixTree("banana").writeDot(out);
    auto dot = out.str();
    EXPECT_TRUE(dot.starts_with("digraph suffix_tree {"));
    EXPECT_TRUE(dot.contains(R"(label="na")")) << "edge labels are missing";
    EXPECT_TRUE(dot.contains("style=dashed")) << "suffix links are missing";
}

// Every edge label in a DOT file, still escaped.
vector<string> dotEdgeLabels(const string& dot) {
    static const regex kLabel(R"re(-> n\d+ \[label="((?:[^"\\]|\\.)*)"\])re");
    vector<string> labels;
    for (sregex_iterator it(dot.begin(), dot.end(), kLabel), end; it != end; ++it)
        labels.push_back((*it)[1]);
    return labels;
}

string dotOf(const string& text) {
    ostringstream out;
    buildSuffixTree(text).writeDot(out);
    return out.str();
}

TEST(SuffixTree, ShortEdgeLabelsAreKeptWhole) {
    auto labels = dotEdgeLabels(dotOf("banana"));
    EXPECT_TRUE(ranges::contains(labels, "banana$"));
    EXPECT_TRUE(ranges::contains(labels, "na$"));
}

// A leaf's edge label runs to the end of the text; unshortened, the DOT file
// for a 1 MB input would be gigabytes.
TEST(SuffixTree, LongEdgeLabelsAreShortened) {
    string text;
    for (int i = 0; i < 2000; i++) text += static_cast<char>('a' + (i * 7 + i / 26) % 26);
    auto labels = dotEdgeLabels(dotOf(text));
    ASSERT_FALSE(labels.empty());
    for (const auto& label : labels)
        EXPECT_LE(label.size(), 16u + 3u) << label;
    EXPECT_TRUE(ranges::contains(labels, text.substr(0, 16) + "..."));
}

// The tree works on bytes, so an edge may branch inside a character (the
// tree can't help that), but the "..." cut never leaves one unfinished.
TEST(SuffixTree, ShortenedLabelsEndOnACharacterBoundary) {
    string text = "x";
    for (int i = 0; i < 30; i++) text += "ä";  // C3 A4
    int shortened = 0;
    for (const auto& label : dotEdgeLabels(dotOf(text))) {
        if (!label.ends_with("...")) continue;
        shortened++;
        EXPECT_FALSE(label.ends_with("\xC3...")) << "half of an ä in " << label;
    }
    EXPECT_GT(shortened, 0);
}

// ============================================================================
// 4. Popular (repeated) substrings
//
// getPopular returns every substring that occurs at least twice, of any
// length. printPopular formats them, by default only those of length >= 2.
// ============================================================================

TEST(GetPopular, Banana) {
    map<string, int64_t> expected{
        {"a", 3}, {"n", 2}, {"an", 2}, {"na", 2}, {"ana", 2},
    };
    EXPECT_EQ(repeatedSubstrings("banana"), expected);
}

TEST(GetPopular, Mississippi) {
    map<string, int64_t> expected{
        {"i", 4}, {"s", 4}, {"p", 2},
        {"is", 2}, {"si", 2}, {"ss", 2},
        {"iss", 2}, {"ssi", 2}, {"issi", 2},
    };
    EXPECT_EQ(repeatedSubstrings("mississippi"), expected);
}

// Occurrences may overlap: "hehe" is found at positions 0 and 2.
TEST(GetPopular, OverlappingOccurrencesCount) {
    map<string, int64_t> expected{
        {"e", 3}, {"h", 3}, {"he", 3},
        {"eh", 2}, {"ehe", 2}, {"heh", 2}, {"hehe", 2},
    };
    EXPECT_EQ(repeatedSubstrings("hehehe"), expected);
}

TEST(GetPopular, RunOfOneCharacter) {
    map<string, int64_t> expected{{"a", 4}, {"aa", 3}, {"aaa", 2}};
    EXPECT_EQ(repeatedSubstrings("aaaa"), expected);
}

TEST(GetPopular, NothingRepeats) {
    EXPECT_TRUE(repeatedSubstrings("abcde").empty());
}

// Asking for only the top N must give the first N of the full ranking
// (most frequent first, then lexicographically). The candidate list is pruned
// along the way, which once dropped entries that belonged in the top N.
TEST(GetPopular, TopNIsAPrefixOfTheFullRanking) {
    mt19937 rng(1);
    for (int round = 0; round < 300; round++) {
        string text(20 + rng() % 400, ' ');
        char alphabet_size = static_cast<char>(2 + rng() % 6);
        for (char& c : text) c = static_cast<char>('a' + rng() % alphabet_size);
        size_t top_n = 1 + rng() % 30;

        SuffixArray sa(text);
        auto all = sa.getPopular(SIZE_MAX);
        auto top = sa.getPopular(top_n);
        all.resize(min(all.size(), top_n));

        ASSERT_EQ(top.size(), all.size()) << text;
        for (size_t i = 0; i < top.size(); i++) {
            ASSERT_EQ(top[i].substring, all[i].substring) << "top_n=" << top_n << " text=" << text;
            ASSERT_EQ(top[i].frequency, all[i].frequency) << "top_n=" << top_n << " text=" << text;
        }
    }
}

TEST(PrintPopular, DefaultFormatIsOnePerLineWithCount) {
    EXPECT_EQ(printPopular("banana"), "an 2\nana 2\nna 2\n");
}

TEST(PrintPopular, RespectsOptions) {
    PopularOptions opts{.min_length = 3, .show_count = false, .entry_delimiter = ","};
    EXPECT_EQ(printPopular("banana", opts), "ana,");
}

// "ä" is two bytes in UTF-8 (C3 A4), so "äää" repeats byte sequences such as
// A4 C3 that cut a character in half. Those must not be printed.
TEST(PrintPopular, NeverSplitsUtf8Characters) {
    EXPECT_EQ(printPopular("äää"), "ä 3\nää 2\n");
}

namespace {

// The full ranking by brute force: every substring occurring at least twice
// (overlaps count), by frequency descending, then lexicographically.
vector<pair<string, int64_t>> bruteForceRanking(const string& text, int min_length) {
    map<string, int64_t> counts;
    for (size_t pos = 0; pos < text.size(); pos++)
        for (size_t len = min_length; pos + len <= text.size(); len++)
            counts[text.substr(pos, len)]++;
    vector<pair<string, int64_t>> ranking;
    for (const auto& [substring, count] : counts)
        if (count >= 2) ranking.emplace_back(substring, count);
    ranges::sort(ranking, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    return ranking;
}

}  // namespace

// The two-pass top_n selection must pick exactly the brute-force top_n,
// including how ties at the cutoff frequency are broken.
TEST(GetPopular, TopNMatchesBruteForce) {
    mt19937 rng(7);
    for (int trial = 0; trial < 300; trial++) {
        string text;
        int length = uniform_int_distribution(1, 40)(rng);
        int alphabet = uniform_int_distribution(1, 4)(rng);
        for (int k = 0; k < length; k++)
            text += static_cast<char>('a' + uniform_int_distribution(0, alphabet - 1)(rng));
        if (trial % 3 == 0) text = text + text + text.substr(0, length / 2);  // repetitive

        auto expected = bruteForceRanking(text, 1);
        for (size_t top_n : {size_t{1}, size_t{2}, size_t{5}, size_t{17}, expected.size(), SIZE_MAX}) {
            vector<pair<string, int64_t>> got;
            for (const auto& score : SuffixArray(text).getPopular(top_n))
                got.emplace_back(score.substring, score.frequency);
            auto want = expected;
            if (want.size() > top_n) want.resize(top_n);
            ASSERT_EQ(got, want) << "text=" << text << " top_n=" << top_n;
        }
    }
}

TEST(PrintPopular, MinLengthAndTopNMatchBruteForce) {
    mt19937 rng(11);
    for (int trial = 0; trial < 100; trial++) {
        string text;
        for (int k = uniform_int_distribution(1, 30)(rng); k > 0; k--)
            text += static_cast<char>('a' + uniform_int_distribution(0, 2)(rng));
        text += text;
        int min_length = uniform_int_distribution(1, 6)(rng);
        size_t top_n = uniform_int_distribution(1, 30)(rng);

        string want;
        auto ranking = bruteForceRanking(text, min_length);
        for (const auto& [substring, count] : ranking | views::take(top_n))
            want += format("{} {}\n", substring, count);
        EXPECT_EQ(printPopular(text, {.top_n = top_n, .min_length = min_length}), want)
            << "text=" << text << " min_length=" << min_length << " top_n=" << top_n;
    }
}

// A text repeated several times has about n²/8 distinct repeated substrings
// per repetition; generating all of them made this take hours.
TEST(PrintPopular, RepetitiveTextIsFast) {
    mt19937 rng(3);
    string block;
    for (int k = 0; k < 50'000; k++) block += static_cast<char>('a' + rng() % 4);
    string text = block + block + block + block;

    auto start = chrono::steady_clock::now();
    auto top = SuffixArray(text).getPopular(100'000);
    double secs = chrono::duration<double>(chrono::steady_clock::now() - start).count();

    ASSERT_EQ(top.size(), 100'000u);
    EXPECT_EQ(top[0].substring.size(), 1u);  // a single letter is the most frequent
    EXPECT_LT(secs, 10.0);
}

// --escape: line breaks and control characters can't break up a listing.
TEST(PrintPopular, EscapedFormat) {
    PopularOptions opts{.min_length = 3, .format = ListFormat::Escaped};
    // "a\nb" occurs twice; nothing else of 3+ bytes repeats.
    EXPECT_EQ(printPopular("a\nb a\nb", opts), "a\\nb 2\n");
    // A backslash is escaped too, so the escaping can be undone.
    EXPECT_EQ(printPopular("a\\b-a\\b", opts), "a\\\\b 2\n");
    EXPECT_EQ(printPopular("\t\x01z \t\x01z", opts), "\\t\\x01z 2\n");
}

namespace {

// The characters of a valid UTF-8 text, each as its bytes.
vector<string> utf8Chars(const string& text) {
    vector<string> chars;
    for (size_t i = 0; i < text.size();) {
        int length = decodeUtf8Char(text, i).length;
        chars.push_back(text.substr(i, length));
        i += length;
    }
    return chars;
}

string joined(const vector<string>& chars, size_t from, size_t to) {
    string s;
    for (size_t k = from; k < to; k++) s += chars[k];
    return s;
}

// Random strings of `length` pieces drawn from `pieces`.
string randomText(mt19937& rng, const vector<string>& pieces, int length) {
    uniform_int_distribution<size_t> pick(0, pieces.size() - 1);
    string text;
    for (int i = 0; i < length; i++) text += pieces[pick(rng)];
    return text;
}

vector<pair<string, int64_t>> ranked(const map<string, int64_t>& counts) {
    vector<pair<string, int64_t>> ranking;
    for (const auto& [substring, count] : counts)
        if (count >= 2) ranking.emplace_back(substring, count);
    ranges::sort(ranking, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    return ranking;
}

string listing(const vector<pair<string, int64_t>>& ranking, size_t top_n) {
    string s;
    for (const auto& [substring, count] : ranking | views::take(top_n))
        s += format("{} {}\n", substring, count);
    return s;
}

}  // namespace

// Substrings that split a character are left out before the top_n are
// chosen, so --top=N still prints N lines when there are that many.
TEST(PrintPopular, NonAsciiTopNMatchesBruteForce) {
    mt19937 rng(13);
    for (int trial = 0; trial < 200; trial++) {
        string text = randomText(rng, {"a", "b", "ä", "€", "ää"}, uniform_int_distribution(1, 25)(rng));
        if (trial % 2 == 0) text += text;
        int min_length = uniform_int_distribution(1, 5)(rng);
        size_t top_n = uniform_int_distribution(1, 30)(rng);

        auto chars = utf8Chars(text);
        map<string, int64_t> counts;
        for (size_t from = 0; from < chars.size(); from++)
            for (size_t to = from + 1; to <= chars.size(); to++)
                if (string s = joined(chars, from, to); ssize(s) >= min_length) counts[s]++;

        EXPECT_EQ(printPopular(text, {.top_n = top_n, .min_length = min_length}),
                  listing(ranked(counts), top_n))
            << "text=" << text << " min_length=" << min_length << " top_n=" << top_n;
    }
}

// --words: only phrases that start and end at a word boundary, counted only
// where they do. "the" in "other" doesn't count.
TEST(PrintPopular, Words) {
    EXPECT_EQ(printPopular("the cat, the cat; other", {.words = true}),
              "cat 2\nthe 2\nthe cat 2\n");
    // --minlength counts words.
    EXPECT_EQ(printPopular("the cat, the cat; other", {.min_length = 2, .words = true}),
              "the cat 2\n");
    // An apostrophe separates words, but phrases keep it.
    EXPECT_EQ(printPopular("don't stop, don't", {.words = true}), "don 2\ndon't 2\nt 2\n");
    // Letters beyond ASCII are word characters; a dash or a quote is not.
    EXPECT_EQ(printPopular("«äö»—äö", {.words = true}), "äö 2\n");
    EXPECT_EQ(printPopular("", {.words = true}), "");
    EXPECT_EQ(printPopular(" , ", {.words = true}), "");
}

namespace {

// --words' word characters, by code point.
bool isWordChar(char32_t cp) {
    if (cp < 0x80) return isalnum(static_cast<int>(cp));
    return cp >= 0xC0 && !(cp >= 0x2000 && cp <= 0x2FFF);
}

}  // namespace

TEST(PrintPopular, WordsMatchBruteForce) {
    mt19937 rng(17);
    vector<string> pieces{"a", "b", "ab", "ä", " ", " ", ", ", "—", "'", "1"};
    for (int trial = 0; trial < 300; trial++) {
        string text = randomText(rng, pieces, uniform_int_distribution(1, 30)(rng));
        if (trial % 3 == 0) text = text + " " + text;
        int min_words = uniform_int_distribution(1, 3)(rng);
        size_t top_n = uniform_int_distribution(1, 25)(rng);

        auto chars = utf8Chars(text);
        auto word = [&](size_t k) { return isWordChar(decodeUtf8Char(chars[k], 0).codepoint); };
        size_t n = chars.size();
        vector<bool> starts(n + 1), ends(n + 1);
        for (size_t k = 0; k <= n; k++) {
            starts[k] = k < n && word(k) && (k == 0 || !word(k - 1));
            ends[k] = k > 0 && word(k - 1) && (k == n || !word(k));
        }
        map<string, int64_t> counts;
        for (size_t from = 0; from < n; from++) {
            if (!starts[from]) continue;
            int words = 0;
            for (size_t to = from + 1; to <= n; to++) {
                if (!ends[to]) continue;
                if (++words >= min_words) counts[joined(chars, from, to)]++;
            }
        }

        EXPECT_EQ(printPopular(text, {.top_n = top_n, .min_length = min_words, .words = true}),
                  listing(ranked(counts), top_n))
            << "text=" << text << " min_words=" << min_words << " top_n=" << top_n;
    }
}

// The counting pass still walks all ~n² phrases of a repetitive text, so it
// must count them by the group. (A large top_n is no test here: the
// lexicographically first phrases that occur 4 times are whole copies, so
// the output alone would be gigabytes.)
TEST(PrintPopular, RepetitiveWordsAreFast) {
    mt19937 rng(19);
    vector<string> vocabulary{"to", "be", "or", "not", "that", "is", "the", "question"};
    string block;
    for (int k = 0; k < 50'000; k++) block += vocabulary[rng() % vocabulary.size()] + " ";
    string text = block + block + block + block;

    auto start = chrono::steady_clock::now();
    string out = printPopular(text, {.top_n = 1000, .words = true});
    double secs = chrono::duration<double>(chrono::steady_clock::now() - start).count();

    EXPECT_EQ(ranges::count(out, '\n'), 1000);
    EXPECT_LT(secs, 10.0);
}

// ============================================================================
// 5. Shortest unique substrings
//
// Every shortest substring that occurs exactly once, by position. Lengths and
// positions count characters, not bytes.
// ============================================================================

TEST(ShortestUnique, SingleRareCharacter) {
    EXPECT_EQ(shortestUniqueOf("banana"), "Length 1 at position 0: \"b\"\n");
    EXPECT_EQ(shortestUniqueOf("mississippi"), "Length 1 at position 0: \"m\"\n");
}

TEST(ShortestUnique, ListsEveryTieByPosition) {
    EXPECT_EQ(shortestUniqueOf("abcb"),
              "Length 1 at position 0: \"a\"\n"
              "Length 1 at position 2: \"c\"\n");
}

TEST(ShortestUnique, LongerThanOneCharacter) {
    EXPECT_EQ(shortestUniqueOf("abab"), "Length 2 at position 1: \"ba\"\n");
}

TEST(ShortestUnique, RunOfOneCharacterIsUniqueOnlyAsAWhole) {
    EXPECT_EQ(shortestUniqueOf("aaaa"), "Length 4 at position 0: \"aaaa\"\n");
}

TEST(ShortestUnique, EmptyTextHasNone) {
    EXPECT_TRUE(SuffixArray("").shortestUnique().empty());
    EXPECT_EQ(shortestUniqueOf(""), "");
}

// The single byte E2 already identifies "€" at byte level; the result must
// still be the whole character.
TEST(ShortestUnique, NeverSplitsUtf8Characters) {
    EXPECT_EQ(shortestUniqueOf("€aaa"), "Length 1 at position 0: \"€\"\n");
    EXPECT_EQ(shortestUniqueOf("äää"), "Length 3 at position 0: \"äää\"\n");
}

TEST(ShortestUnique, CountsCharactersNotBytes) {
    // "ä€" is 2 characters but 5 bytes; "x" is at byte 10, character 4.
    EXPECT_EQ(shortestUniqueOf("ä€ä€x"), "Length 1 at position 4: \"x\"\n");
    // A 1-character, 3-byte answer beats 2-character, 2-byte candidates.
    auto found = SuffixArray("abab€").shortestUnique();
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].pos, 4);
    EXPECT_EQ(found[0].len, 3);
    EXPECT_EQ(found[0].char_pos, 4);
    EXPECT_EQ(found[0].char_len, 1);
}

TEST(ShortestUnique, LongOnesAreAbbreviated) {
    string text(100, 'a');
    EXPECT_EQ(shortestUniqueOf(text),
              "Length 100 at position 0: \"" + string(38, 'a') + "..." + string(38, 'a') + "\"\n");
}

TEST(ShortestUnique, MatchesBruteForce) {
    const vector<string> alphabet{"a", "b", "ä", "€"};
    mt19937 rng(7);
    for (int round = 0; round < 500; round++) {
        string text;
        size_t chars = rng() % 30;
        size_t alphabet_size = 1 + rng() % alphabet.size();
        for (size_t i = 0; i < chars; i++) text += alphabet[rng() % alphabet_size];

        vector<pair<int, int>> actual;
        for (const auto& u : SuffixArray(text).shortestUnique()) {
            ASSERT_EQ(decodeUtf8(text.substr(u.pos, u.len)).size(), static_cast<size_t>(u.char_len))
                << text;
            actual.emplace_back(u.char_pos, u.char_len);
        }
        ASSERT_EQ(actual, bruteForceShortestUnique(text)) << text;
    }
}

// ============================================================================
// 6. Palindromes
//
// Only maximal palindromes are listed (the longest one around each centre),
// each distinct one once, longest first and then by position. Positions and
// lengths count characters, not bytes.
// ============================================================================

TEST(Palindromes, ListsLongestFirstThenByPosition) {
    EXPECT_EQ(palindromesOf("racecar level"),
              "Length 7 at position 0: \"racecar\"\n"
              "Length 5 at position 8: \"level\"\n");
}

TEST(Palindromes, FindsEvenLengthPalindromes) {
    EXPECT_EQ(palindromesOf("xabbay"), "Length 4 at position 1: \"abba\"\n");
}

TEST(Palindromes, RepeatedPalindromeIsListedOnceAtFirstPosition) {
    EXPECT_EQ(palindromesOf("noon, noon"), "Length 4 at position 0: \"noon\"\n");
}

TEST(Palindromes, SingleCharactersDoNotCount) {
    EXPECT_EQ(palindromesOf("abc"), "No palindromes found.\n");
    EXPECT_EQ(palindromesOf(""), "");
}

TEST(Palindromes, CountsCharactersNotBytes) {
    EXPECT_EQ(palindromesOf("xäöäy"), "Length 3 at position 1: \"äöä\"\n");
}

TEST(Palindromes, LongOnesAreAbbreviatedWithoutSplittingCharacters) {
    string half;
    for (int i = 0; i < 50; i++) half += "ä";
    auto output = palindromesOf(half + "ö" + half);
    auto first_line = output.substr(0, output.find('\n') + 1);
    EXPECT_EQ(first_line, "Length 101 at position 0: \"" + half.substr(0, 2 * 38) + "..." +
                          half.substr(0, 2 * 38) + "\"\n");
}

// ============================================================================
// 7. UTF-8 conversion
// ============================================================================

TEST(Utf8, RoundTripsEveryEncodedLength) {
    // 1, 2, 3 and 4 bytes per character.
    string text = "a\u00e4\u20ac\U0001F600";
    EXPECT_EQ(decodeUtf8(text), U"a\u00e4\u20ac\U0001F600");
    EXPECT_EQ(encodeUtf8(decodeUtf8(text)), text);
}

TEST(Utf8, TruncatedSequenceBecomesReplacementCharacter) {
    EXPECT_EQ(decodeUtf8("a\xE2\x82"), U"a\uFFFD");
}

TEST(Utf8, DecodesOneCharacterAtATime) {
    EXPECT_EQ(decodeUtf8Char("a", 0), (Utf8Char{U'a', 1, true}));
    EXPECT_EQ(decodeUtf8Char("x\xC3\xA4", 1), (Utf8Char{U'\u00e4', 2, true}));
    EXPECT_EQ(decodeUtf8Char("\xF0\x9F\x98\x80", 0), (Utf8Char{U'\U0001F600', 4, true}));
}

// Each of these is reported as one invalid byte, so decoding resumes right
// after it instead of swallowing the characters that follow.
TEST(Utf8, StrictDecodingRejectsMalformedSequences) {
    const Utf8Char invalid{U'\uFFFD', 1, false};
    EXPECT_EQ(decodeUtf8Char("\xA4", 0), invalid) << "stray continuation byte";
    EXPECT_EQ(decodeUtf8Char("\xC3" "b", 0), invalid) << "lead byte without continuation";
    EXPECT_EQ(decodeUtf8Char("\xE2\x82", 0), invalid) << "truncated by end of input";
    EXPECT_EQ(decodeUtf8Char("\xC0\xAF", 0), invalid) << "overlong '/'";
    EXPECT_EQ(decodeUtf8Char("\xE0\x80\xAF", 0), invalid) << "overlong '/'";
    EXPECT_EQ(decodeUtf8Char("\xED\xA0\x80", 0), invalid) << "UTF-16 surrogate";
    EXPECT_EQ(decodeUtf8Char("\xF4\x90\x80\x80", 0), invalid) << "past U+10FFFF";
    EXPECT_EQ(decodeUtf8Char("\xFF", 0), invalid);
}

// ============================================================================
// 8. Internal tables view
//
// The suffixes are printed for a person to read on a terminal, so nothing in
// the input may reach the terminal as a control sequence.
// ============================================================================

// Sorted by byte: "\n" < ESC < 'a' < 'b'.
TEST(Tables, ControlCharactersAreShownAsSymbols) {
    EXPECT_EQ(tableSuffixes("a\nb\x1b"), (vector<string>{"␊b␛", "␛", "a␊b␛", "b␛"}));
}

TEST(Tables, EscapeSequencesInTheInputDoNotReachTheTerminal) {
    EXPECT_FALSE(tablesOf("ab\x1b[2Jab").contains("\x1b[2J"));
}

// The table lists every byte position, so suffixes starting inside a
// character begin with a stray continuation byte.
TEST(Tables, InvalidBytesAreShownInHex) {
    EXPECT_EQ(tableSuffixes("ä"), (vector<string>{"\\xA4", "ä"}));
    EXPECT_EQ(tableSuffixes("\xFF"), (vector<string>{"\\xFF"}));
}

TEST(Tables, InvisibleAndReorderingCharactersAreEscaped) {
    auto suffixes = tableSuffixes("a\u202Eb\u200Bc");  // right-to-left override, zero-width space
    EXPECT_TRUE(ranges::contains(suffixes, "a\\u202Eb\\u200Bc"));
}

// Suffixes are cut by terminal columns, not bytes, and never mid-character.
// A width of 1 leaves the minimum of 5 columns for the suffix.
TEST(Tables, SuffixesAreCutByColumns) {
    EXPECT_TRUE(ranges::contains(tableSuffixes("ääääääää", 1), "äääää"));
    EXPECT_TRUE(ranges::contains(tableSuffixes("日本語の本", 1), "日本"));  // 2 columns each
    EXPECT_TRUE(ranges::contains(tableSuffixes("e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301", 1),
                                 "e\u0301e\u0301e\u0301e\u0301e\u0301"));  // accents take none
}

// "ö" (C3 B6) shares only its first byte with the "ä" (C3 A4) before it, so
// nothing of it is bold.
TEST(Tables, BoldPrefixDoesNotSplitACharacter) {
    EXPECT_TRUE(tablesOf("äö").contains("\033[1m\033[0mö"));
}

// ============================================================================
// 9. Progress bar on stderr
// ============================================================================

TEST(ProgressBar, SilentByDefault) {
    EXPECT_EQ(stderrWhileBuilding("banana", false), "");
}

TEST(ProgressBar, ShowsPercentageAndThroughput) {
    auto output = stderrWhileBuilding("banana", true);
    EXPECT_TRUE(output.contains("MB/s")) << "stderr was: " << output;
    EXPECT_TRUE(output.contains("100%")) << "stderr was: " << output;
}

TEST(ProgressBar, QuickBuildsDrawNothing) {
    EXPECT_EQ(stderrWhileBuilding("banana", true, 10.0), "");
}

TEST(ProgressBar, EmptyInputStillFinishes) {
    auto output = stderrWhileBuilding("", true);
    EXPECT_TRUE(output.contains("100%")) << "stderr was: " << output;
}

// ============================================================================
// 10. LZ77 factorization
// ============================================================================

namespace {

vector<Lz77Phrase> factorize(const string& text) {
    return lz77Factorize(SuffixArray(text));
}

// Decodes like an LZ77 decoder would, copying byte by byte so a phrase can
// overlap its own source. Literal bytes come from `text`, as the TSV's text
// column provides them.
string decodeLz77(const vector<Lz77Phrase>& phrases, const string& text) {
    string out;
    for (const auto& phrase : phrases) {
        EXPECT_EQ(phrase.pos, static_cast<int>(out.size()));
        if (phrase.src < 0) {
            EXPECT_EQ(phrase.len, 1);
            out += text[phrase.pos];
        } else {
            EXPECT_LT(phrase.src, phrase.pos);
            for (int k = 0; k < phrase.len; k++) out += out[phrase.src + k];
        }
    }
    return out;
}

// O(n^2) reference: the longest prefix of text[p..] that starts earlier.
int longestEarlierMatch(const string& text, int p) {
    int n = static_cast<int>(text.size()), best = 0;
    for (int q = 0; q < p; q++) {
        int len = 0;
        while (p + len < n && text[q + len] == text[p + len]) len++;
        best = max(best, len);
    }
    return best;
}

string lz77Of(const string& text) {
    ostringstream out;
    printLz77(out, SuffixArray(text));
    return out.str();
}

} // namespace

TEST(Lz77, Banana) {
    vector<Lz77Phrase> expected{{0, 1, -1}, {1, 1, -1}, {2, 1, -1}, {3, 3, 1}};
    EXPECT_EQ(factorize("banana"), expected);
}

TEST(Lz77, CopyOverlapsItsSource) {
    vector<Lz77Phrase> expected{{0, 1, -1}, {1, 7, 0}};
    EXPECT_EQ(factorize("aaaaaaaa"), expected);
    EXPECT_EQ(decodeLz77(factorize("aaaaaaaa"), "aaaaaaaa"), "aaaaaaaa");
}

TEST(Lz77, EmptyAndSingleByte) {
    EXPECT_TRUE(factorize("").empty());
    vector<Lz77Phrase> expected{{0, 1, -1}};
    EXPECT_EQ(factorize("x"), expected);
}

// Every phrase must be exactly as long as the longest earlier match (or a
// literal when there is none), its source must really match, and decoding
// must give the text back. Sources aren't compared: ties allow several.
TEST(Lz77, MatchesBruteForceOnRandomTexts) {
    mt19937 rng(77);
    const vector<vector<string>> alphabets{
        {"a", "b"}, {"a", "b", "c", "d"}, {"a", "é", "€", "😀"}, {"ab", "ba", "aab"}};
    for (const auto& pieces : alphabets) {
        for (int round = 0; round < 60; round++) {
            string text = randomText(rng, pieces, round * 3);
            auto phrases = factorize(text);
            for (const auto& phrase : phrases) {
                int expected_len = longestEarlierMatch(text, phrase.pos);
                if (expected_len == 0) {
                    EXPECT_EQ(phrase.src, -1) << text << " at " << phrase.pos;
                    EXPECT_EQ(phrase.len, 1);
                } else {
                    EXPECT_EQ(phrase.len, expected_len) << text << " at " << phrase.pos;
                    EXPECT_GE(phrase.src, 0);
                    EXPECT_EQ(text.compare(phrase.src, phrase.len, text, phrase.pos, phrase.len), 0);
                }
            }
            EXPECT_EQ(decodeLz77(phrases, text), text);
        }
    }
}

TEST(Lz77, PrintsTsvWithSummary) {
    EXPECT_EQ(lz77Of("banana"),
              "pos\tlen\tsrc\ttext\n"
              "0\t1\t-\tb\n"
              "1\t1\t-\ta\n"
              "2\t1\t-\tn\n"
              "3\t3\t1\tana\n"
              "# n=6 z=4 avg_len=1.50 literals=3 longest=3\n");
}

TEST(Lz77, EmptyInputPrintsHeaderAndSummary) {
    EXPECT_EQ(lz77Of(""), "pos\tlen\tsrc\ttext\n# n=0 z=0 avg_len=0.00 literals=0 longest=0\n");
}

// A tab or newline in a phrase must not break the TSV line.
TEST(Lz77, ControlCharactersAreEscapedInOutput) {
    auto output = lz77Of("a\tb\na\tb\n");
    EXPECT_TRUE(output.contains("4\t4\t0\ta\\tb\\n\n")) << output;
}

TEST(Utf8, EscapeBytesKeepsPrintableText) {
    EXPECT_EQ(escapeBytes("plain ääkköset 日本"), "plain ääkköset 日本");
}

TEST(Utf8, EscapeBytesEscapesControlsAndBackslash) {
    EXPECT_EQ(escapeBytes("a\tb\nc\rd\\e"), "a\\tb\\nc\\rd\\\\e");
    EXPECT_EQ(escapeBytes(string("\x00\x1b\x7f", 3)), "\\x00\\x1B\\x7F");
}

TEST(Utf8, EscapeBytesEscapesInvisibleAndInvalid) {
    EXPECT_EQ(escapeBytes("x\u202Ey"), "x\\u202Ey");   // right-to-left override
    EXPECT_EQ(escapeBytes("\u0085"), "\\u0085");       // C1 control (next line)
    EXPECT_EQ(escapeBytes("\xC3"), "\\xC3");           // first byte of "ä" only
    EXPECT_EQ(escapeBytes("\xA4z"), "\\xA4z");         // its last byte only
}

// ============================================================================
// 11. Burrows-Wheeler matrix
// ============================================================================

namespace {

// printBwt's output with colors removed, as lines.
vector<string> bwtLines(const string& text, int width = 200) {
    static const regex kAnsiColor("\x1b\\[[0-9;]*m");
    ostringstream out;
    printBwt(out, SuffixArray(text), {.width = width});
    istringstream lines(regex_replace(out.str(), kAnsiColor, ""));
    vector<string> result;
    for (string line; getline(lines, line);) result.push_back(line);
    return result;
}

// The value after a footer label ("BWT", "Index", "Runs"), or "" if absent.
string bwtFooter(const vector<string>& lines, const string& label) {
    for (const auto& line : lines)
        if (line.starts_with(label + " ")) return line.substr(line.find_first_not_of(' ', label.size()));
    return "";
}

// Reference: sort every rotation of the codepoints plus '$' (which is below
// all the letters used) and take the last character of each.
string naiveBwt(const string& utf8) {
    u32string s = decodeUtf8(utf8) + U'$';
    vector<u32string> rotations;
    for (size_t i = 0; i < s.size(); i++) rotations.push_back(s.substr(i) + s.substr(0, i));
    ranges::sort(rotations);
    u32string last;
    for (const auto& r : rotations) last += r.back();
    return encodeUtf8(last);
}

} // namespace

TEST(Bwt, BananaMatrix) {
    auto lines = bwtLines("banana");
    vector<string> rows(lines.begin() + 1, lines.begin() + 8);
    EXPECT_EQ(rows, (vector<string>{
        "0  6    $  banan  a",
        "1  5    a  $bana  n",
        "2  3    a  na$ba  n",
        "3  1    a  nana$  b",
        "4  0    b  anana  $ ◀",
        "5  4    n  a$ban  a",
        "6  2    n  ana$b  a",
    }));
    EXPECT_EQ(bwtFooter(lines, "BWT"), "annb$aa");
    EXPECT_EQ(bwtFooter(lines, "Index"), "4 (the row of the original text)");
    EXPECT_EQ(bwtFooter(lines, "Runs"), "r=5 in n=7 characters, counting $ (n/r = 1.40)");
}

TEST(Bwt, Mississippi) {
    EXPECT_EQ(bwtFooter(bwtLines("mississippi"), "BWT"), "ipssm$pissii");
}

TEST(Bwt, EmptyInputIsJustTheSentinel) {
    auto lines = bwtLines("");
    EXPECT_EQ(bwtFooter(lines, "BWT"), "$");
    EXPECT_EQ(bwtFooter(lines, "Index"), "0 (the row of the original text)");
}

// Multi-byte characters are single cells and sort by codepoint.
TEST(Bwt, MatchesNaiveRotationSortOnRandomTexts) {
    mt19937 rng(42);
    const vector<vector<string>> alphabets{{"a", "b"}, {"a", "b", "c"}, {"a", "é", "€", "😀"}};
    for (const auto& pieces : alphabets) {
        for (int length = 1; length < 40; length++) {
            string text = randomText(rng, pieces, length);
            EXPECT_EQ(bwtFooter(bwtLines(text), "BWT"), naiveBwt(text)) << text;
        }
    }
}

TEST(Bwt, ControlCharactersAreShownVisibly) {
    auto lines = bwtLines("a\nb\x1b");
    // "$" is the sentinel, below every byte; ␊ (0x0A) sorts before ␛ (0x1B).
    EXPECT_EQ(bwtFooter(lines, "BWT"), "␛ab$␊");
    for (const auto& line : lines) EXPECT_FALSE(line.contains('\x1b')) << line;
}

// A narrow terminal elides the middle of each rotation but keeps F and L.
TEST(Bwt, ElidesMiddleToFitWidth) {
    auto lines = bwtLines("abcdefghijklmnopqrstuvwxyz", 40);
    EXPECT_TRUE(lines[1].contains("…")) << lines[1];
    EXPECT_TRUE(lines[1].ends_with("z")) << lines[1];   // the "$" row: L is the last letter
    for (size_t i = 1; i <= 27; i++) EXPECT_LE(lines[i].size(), 40u + 4) << lines[i];  // + "…" and "◀" bytes
}

TEST(Bwt, LargeInputPrintsOnlySummary) {
    vector<string> lines;
    string note = stderrDuring([&] { lines = bwtLines(string(kMaxBwtMatrixChars + 1, 'a')); });
    EXPECT_TRUE(note.starts_with("Note: the BWT matrix is drawn only")) << note;
    EXPECT_EQ(bwtFooter(lines, "BWT"), "");
    // L is 1001 a's, then the "$" of the original text's row, which sorts last.
    EXPECT_EQ(bwtFooter(lines, "Runs"), "r=2 in n=1002 characters, counting $ (n/r = 501.00)");
}

// ============================================================================
// 12. Terminal: color, width, pager command
// ============================================================================

namespace {

// Sets an environment variable for one scope, restoring it afterwards.
class ScopedEnv {
    string name;
    optional<string> saved;

public:
    ScopedEnv(string name, const char* value) : name(std::move(name)) {
        if (const char* old = getenv(this->name.c_str())) saved = old;
        if (value) setenv(this->name.c_str(), value, 1);
        else unsetenv(this->name.c_str());
    }
    ~ScopedEnv() {
        if (saved) setenv(name.c_str(), saved->c_str(), 1);
        else unsetenv(name.c_str());
    }
};

}  // namespace

TEST(Terminal, AlwaysAndNeverIgnoreTheEnvironment) {
    ScopedEnv no_color("NO_COLOR", "1");
    EXPECT_TRUE(shouldColor(When::Always, false));
    EXPECT_FALSE(shouldColor(When::Never, true));
}

TEST(Terminal, AutoColorsOnlyATerminal) {
    ScopedEnv no_color("NO_COLOR", nullptr);
    ScopedEnv term("TERM", "xterm-256color");
    EXPECT_TRUE(shouldColor(When::Auto, true));
    EXPECT_FALSE(shouldColor(When::Auto, false));
}

TEST(Terminal, AutoRespectsNoColorAndDumbTerminals) {
    ScopedEnv term("TERM", "xterm");
    {
        ScopedEnv no_color("NO_COLOR", "1");
        EXPECT_FALSE(shouldColor(When::Auto, true));
    }
    {
        ScopedEnv no_color("NO_COLOR", "");  // empty means unset
        EXPECT_TRUE(shouldColor(When::Auto, true));
    }
    ScopedEnv dumb("TERM", "dumb");
    EXPECT_FALSE(shouldColor(When::Auto, true));
}

TEST(Terminal, EscapeByDefaultOnlyOnATerminal) {
    EXPECT_TRUE(shouldEscape(When::Auto, true));
    EXPECT_FALSE(shouldEscape(When::Auto, false));
    EXPECT_TRUE(shouldEscape(When::Always, false));
    EXPECT_FALSE(shouldEscape(When::Never, true));
}

TEST(Terminal, ColumnsOverridesTheWidth) {
    {
        ScopedEnv columns("COLUMNS", "132");
        EXPECT_EQ(outputWidth(false), 132);
    }
    for (const char* bad : {"", "0", "-5", "12x"}) {
        ScopedEnv columns("COLUMNS", bad);
        EXPECT_EQ(outputWidth(false), 80) << bad;
    }
}

TEST(Terminal, PagerCommandPrefersSubstringsPager) {
    ScopedEnv pager("PAGER", "more");
    {
        ScopedEnv substrings_pager("SUBSTRINGS_PAGER", "less -S");
        EXPECT_EQ(pagerCommand(), "less -S");
    }
    {
        ScopedEnv substrings_pager("SUBSTRINGS_PAGER", "");  // set but empty: no pager
        EXPECT_EQ(pagerCommand(), "");
    }
    ScopedEnv substrings_pager("SUBSTRINGS_PAGER", nullptr);
    EXPECT_EQ(pagerCommand(), "more");
    ScopedEnv no_pager("PAGER", nullptr);
    EXPECT_EQ(pagerCommand(), "less");
}

TEST(Terminal, PlainTablesHaveNoEscapes) {
    for (string text : {"banana", "a\nb\x1b", "äö"}) {
        ostringstream tables, bwt;
        SuffixArray sa(text);
        printTables(tables, sa, {.width = 80, .color = false});
        printBwt(bwt, sa, {.width = 80, .color = false});
        EXPECT_FALSE(tables.str().contains('\x1b')) << tables.str();
        EXPECT_FALSE(bwt.str().contains('\x1b')) << bwt.str();
    }
}

TEST(Terminal, PlainTablesMatchColoredOnesWithoutEscapes) {
    static const regex kAnsiColor("\x1b\\[[0-9;]*m");
    ostringstream plain, colored;
    SuffixArray sa("mississippi");
    printTables(plain, sa, {.width = 80, .color = false});
    printTables(colored, sa, {.width = 80, .color = true});
    EXPECT_EQ(plain.str(), regex_replace(colored.str(), kAnsiColor, ""));
}

// ============================================================================
// 13. Shell completions and the man page
// ============================================================================

TEST(Completions, EveryOptionIsInEveryScript) {
    for (string_view shell : {"bash", "zsh", "fish"}) {
        string script = completionScript(shell);
        for (const auto& option : options()) {
            // fish names long options without their dashes ("-l top").
            string_view name = shell == "fish" ? option.name.substr(2) : option.name;
            EXPECT_TRUE(script.contains(name)) << shell << ": " << option.name;
        }
    }
}

TEST(Completions, UnknownShellGivesNothing) {
    EXPECT_EQ(completionScript("tcsh"), "");
}

TEST(Completions, FileOptionsCompleteFileNames) {
    EXPECT_TRUE(completionScript("bash").contains("--input=|--input|-i|"));
    EXPECT_TRUE(completionScript("zsh").contains("--input=[Read input from FILE"));
    EXPECT_TRUE(completionScript("zsh").contains(":FILE:_files'"));
    EXPECT_TRUE(completionScript("fish").contains("-l input -s i -r -F"));
}

TEST(Completions, ChoicesAreOffered) {
    EXPECT_TRUE(completionScript("bash").contains(R"(compgen -W "auto always never")"));
    EXPECT_TRUE(completionScript("zsh").contains(":WHEN:(auto always never)'"));
    EXPECT_TRUE(completionScript("fish").contains("-a 'bash zsh fish'"));
}

TEST(ManPage, DocumentsEveryOption) {
    ifstream file(SUBSTRINGS_SOURCE_DIR "/substrings.1");
    ASSERT_TRUE(file) << "cannot read substrings.1";
    string page((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
    for (const auto& option : options()) {
        // mdoc writes --top as "Fl -top" and -n as "Fl n".
        for (string_view name : {option.name, option.alias}) {
            if (name.empty()) continue;
            EXPECT_TRUE(page.contains("Fl " + string(name.substr(1)))) << name;
        }
    }
}

// ============================================================================
// 14. JSON output
// ============================================================================

TEST(Json, StringEscapes) {
    EXPECT_EQ(jsonString("plain"), R"("plain")");
    EXPECT_EQ(jsonString("a\"b\\c"), R"("a\"b\\c")");
    EXPECT_EQ(jsonString("\n\t\r"), R"("\n\t\r")");
    EXPECT_EQ(jsonString(string("\x00\x1b", 2)), R"("\u0000\u001b")");
    EXPECT_EQ(jsonString("äö€😀"), "\"äö€😀\"");
}

TEST(Json, InvalidUtf8BecomesReplacementCharacter) {
    EXPECT_EQ(jsonString("a\xff" "b"), "\"a�" "b\"");
    EXPECT_EQ(jsonString("\xe2\x82"), "\"��\"");
}

TEST(Json, RepeatsAreJsonLines) {
    EXPECT_EQ(printPopular("banana", {.format = ListFormat::Json}),
              R"({"type":"repeat","text":"an","count":2})" "\n"
              R"({"type":"repeat","text":"ana","count":2})" "\n"
              R"({"type":"repeat","text":"na","count":2})" "\n");
}

TEST(Json, RepeatsIgnoreTheTextOptions) {
    PopularOptions opts{.top_n = 1, .show_count = false, .delimiter = ";",
                        .entry_delimiter = ",", .format = ListFormat::Json};
    EXPECT_EQ(printPopular("banana", opts), R"({"type":"repeat","text":"an","count":2})" "\n");
}

TEST(Json, Palindromes) {
    ostringstream out;
    printPalindromes(out, "xäbäy", ListFormat::Json);
    EXPECT_EQ(out.str(), R"({"type":"palindrome","text":"äbä","pos":1,"len":3})" "\n");
}

TEST(Json, NoPalindromesPrintsNothing) {
    ostringstream out;
    printPalindromes(out, "abc", ListFormat::Json);
    EXPECT_EQ(out.str(), "");
}

TEST(Json, ShortestUnique) {
    ostringstream out;
    SuffixArray("banana").printShortestUnique(out, ListFormat::Json);
    EXPECT_EQ(out.str(), R"({"type":"unique","text":"b","pos":0,"len":1})" "\n");
}

TEST(Json, OmittedResultsAreCounted) {
    // 1200 distinct palindromes "aXa", only 1000 of which are printed.
    string text;
    for (int i = 0; i < 1200; i++) text += "a" + encodeUtf8(static_cast<char32_t>(0x4E00 + i));
    text += "a";
    ostringstream out;
    printPalindromes(out, text, ListFormat::Json);
    string last = out.str().substr(out.str().rfind('{'));
    EXPECT_EQ(last, R"({"type":"omitted","of":"palindrome","count":200})" "\n");
}

// ============================================================================
// 15. Pattern search (--find)
// ============================================================================

TEST(Find, CountsAndPositionsMatchBruteForce) {
    mt19937 rng(5);
    for (int trial = 0; trial < 200; trial++) {
        string text;
        for (int k = uniform_int_distribution(0, 40)(rng); k > 0; k--)
            text += static_cast<char>('a' + uniform_int_distribution(0, 2)(rng));
        string pattern;
        for (int k = uniform_int_distribution(1, 4)(rng); k > 0; k--)
            pattern += static_cast<char>('a' + uniform_int_distribution(0, 2)(rng));

        vector<int> want;
        for (size_t at = text.find(pattern); at != string::npos; at = text.find(pattern, at + 1))
            want.push_back(static_cast<int>(at));
        SuffixArray sa(text);
        EXPECT_EQ(sa.occurrences(pattern), want) << text << " / " << pattern;
        EXPECT_EQ(sa.count(pattern), static_cast<int64_t>(want.size()));
        EXPECT_EQ(sa.contains(pattern), !want.empty());
    }
}

TEST(Find, PrintsLikeTheRepeats) {
    ostringstream out;
    vector<string> patterns{"ana", "x", "a\nb"};
    SuffixArray("banana a\nb").printMatches(out, patterns, {.delimiter = "\t"});
    EXPECT_EQ(out.str(), "ana\t2\nx\t0\na\nb\t1\n");
}

TEST(Find, EscapedPatterns) {
    ostringstream out;
    vector<string> patterns{"a\nb"};
    SuffixArray("a\nb").printMatches(out, patterns, {.format = ListFormat::Escaped});
    EXPECT_EQ(out.str(), "a\\nb 1\n");
}

TEST(Find, JsonListsCharacterPositions) {
    ostringstream out;
    vector<string> patterns{"ö", "zz"};
    SuffixArray("äöäö").printMatches(out, patterns, {.format = ListFormat::Json});
    EXPECT_EQ(out.str(), R"({"type":"match","text":"ö","count":2,"pos":[1,3]})" "\n"
                         R"({"type":"match","text":"zz","count":0,"pos":[]})" "\n");
}

TEST(Find, WholeWordsOnlyWithWords) {
    vector<string> patterns{"the", "the cat", " the"};
    ostringstream plain, words, json;
    SuffixArray sa("the other theme, the cat");
    sa.printMatches(plain, patterns);
    sa.printMatches(words, patterns, {.words = true});
    sa.printMatches(json, span(patterns).first(1), {.words = true, .format = ListFormat::Json});
    EXPECT_EQ(plain.str(), "the 4\nthe cat 1\n the 2\n");
    EXPECT_EQ(words.str(), "the 2\nthe cat 1\n the 0\n");
    EXPECT_EQ(json.str(), R"({"type":"match","text":"the","count":2,"pos":[0,17]})" "\n");
}

TEST(Palindromes, EscapedFormat) {
    ostringstream out;
    printPalindromes(out, "x\na\nx", ListFormat::Escaped);
    EXPECT_TRUE(out.str().starts_with("Length 5 at position 0: \"x\\na\\nx\"\n")) << out.str();
}
