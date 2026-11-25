#include "cli.h"
#include <algorithm>
#include <charconv>
#include <climits>
#include <cstdint>
#include <format>
#include <numeric>
#include <print>
#include <ranges>
#include <vector>

using namespace std;

namespace {

// Interprets \n, \r, \t, \\ and \0 escapes in a delimiter or pattern.
string unescapeArg(string_view s) {
    string result;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case 'n':  c = '\n'; i++; break;
                case 'r':  c = '\r'; i++; break;
                case 't':  c = '\t'; i++; break;
                case '\\': c = '\\'; i++; break;
                case '0':  c = '\0'; i++; break;
                default:   break;  // not an escape: keep the backslash
            }
        }
        result += c;
    }
    return result;
}

// A whole number from 1 to max, or nullopt.
optional<int64_t> positiveNumber(string_view s, int64_t max) {
    int64_t value = 0;
    auto [end, ec] = from_chars(s.data(), s.data() + s.size(), value);
    if (ec != errc{} || end != s.data() + s.size() || value < 1 || value > max)
        return nullopt;
    return value;
}

// Setters for options that just switch an Args field on or store their value.
template <bool Args::*Field>
string setFlag(Args& args, string_view) {
    args.*Field = true;
    return {};
}
template <string Args::*Field>
string setText(Args& args, string_view value) {
    args.*Field = value;
    return {};
}

string setTop(Args& args, string_view value) {
    auto n = positiveNumber(value, INT64_MAX);
    if (!n) return format("--top needs a positive number, not '{}'", value);
    args.popular.top_n = static_cast<size_t>(*n);
    return {};
}

string setMinLength(Args& args, string_view value) {
    auto n = positiveNumber(value, INT_MAX);
    if (!n) return format("--minlength needs a positive number, not '{}'", value);
    args.popular.min_length = static_cast<int>(*n);
    return {};
}

// The value is one of auto, always and never; parseArgs has checked.
template <When Args::*Field>
string setWhen(Args& args, string_view value) {
    args.*Field = value == "always" ? When::Always
                : value == "never"  ? When::Never
                                    : When::Auto;
    return {};
}

string addPattern(Args& args, string_view value) {
    string pattern = unescapeArg(value);
    if (pattern.empty()) return "--find needs a non-empty pattern";
    args.find_patterns.push_back(std::move(pattern));
    return {};
}

// In --help order.
const Option kOptions[] = {
    {.name = "--input", .alias = "-i", .arity = Arity::Required,
     .set = [](Args& a, string_view v) { a.input_file = v; return string(); },
     .value_name = "FILE", .help = "Read input from FILE (\"-\" for stdin)"},
    {.name = "--inline", .set = setFlag<&Args::inline_text>,
     .help = "Take INPUT as the input text instead of a file name"},
    {.name = "--output", .alias = "-o", .arity = Arity::Required,
     .set = [](Args& a, string_view v) {
         a.output_file = (v == "-") ? "" : v;  // "-" is stdout
         return string();
     },
     .value_name = "FILE", .help = "Write output to FILE (\"-\" for stdout)"},
    {.name = "--top", .alias = "-n", .arity = Arity::Required, .set = setTop,
     .value_name = "N", .help = "Print at most N repeated substrings (default: 100000)"},
    {.name = "--minlength", .arity = Arity::Required, .set = setMinLength,
     .value_name = "N", .help = "Minimum length (default: 2 bytes; with --words, 1 word)"},
    {.name = "--words",
     .set = [](Args& a, string_view) { a.popular.words = true; return string(); },
     .help = "Only whole words and phrases, in the repeats and --find"},
    {.name = "--nocount",
     .set = [](Args& a, string_view) { a.popular.show_count = false; return string(); },
     .help = "Omit occurrence counts from output"},
    {.name = "--delimiter", .arity = Arity::Required,
     .set = [](Args& a, string_view v) { a.popular.delimiter = unescapeArg(v); return string(); },
     .value_name = "STR", .help = "Separator between substring and count (default: \" \")"},
    {.name = "--entry-delimiter", .arity = Arity::Required,
     .set = [](Args& a, string_view v) { a.popular.entry_delimiter = unescapeArg(v); return string(); },
     .value_name = "STR", .help = "Separator between entries (default: \"\\n\")"},
    {.name = "--find", .arity = Arity::Required, .set = addPattern, .value_name = "PATTERN",
     .help = "Count PATTERN's occurrences instead of listing repeats (repeatable)"},
    {.name = "--json", .set = setFlag<&Args::json>,
     .help = "Print results as JSON Lines, one object per line"},
    {.name = "--escape", .arity = Arity::Optional, .set = setWhen<&Args::escape>,
     .value_name = "WHEN", .choices = "auto always never", .implicit_value = "always",
     .help = "Escape line breaks etc. in results: auto (default), always or never"},
    {.name = "--palindrome", .set = setFlag<&Args::palindrome>,
     .help = "Find palindromic substrings"},
    {.name = "--shortest-unique", .set = setFlag<&Args::shortest_unique>,
     .help = "Find the shortest substrings that occur only once"},
    {.name = "--lz77", .set = setFlag<&Args::lz77>,
     .help = "Print the greedy LZ77 factorization as TSV (instead of repeats)"},
    {.name = "--view-internal-tables", .set = setFlag<&Args::view_tables>,
     .help = "Print the SA/Rank/PLCP/LCP tables"},
    {.name = "--bwt", .set = setFlag<&Args::bwt>,
     .help = "Print the Burrows-Wheeler matrix and transform"},
    {.name = "--graph",
     .set = [](Args& a, string_view) { a.dot_file = "suffix_tree.dot"; return string(); },
     .help = "Generate suffix_tree.dot"},
    {.name = "--dotfile", .arity = Arity::Required, .set = setText<&Args::dot_file>,
     .value_name = "FILE", .help = "Generate DOT file at FILE"},
    {.name = "--force-graph", .set = setFlag<&Args::force_graph>,
     .help = "Allow --graph/--dotfile for inputs over 10 KB"},
    {.name = "--color", .arity = Arity::Optional, .set = setWhen<&Args::color>, .value_name = "WHEN",
     .choices = "auto always never", .implicit_value = "always",
     .help = "Color tables: auto (default), always or never"},
    {.name = "--no-pager",
     .set = [](Args& a, string_view) { a.use_pager = false; return string(); },
     .help = "Don't page terminal output through $PAGER (less)"},
    {.name = "--completions", .arity = Arity::Required, .set = setText<&Args::completions_shell>,
     .value_name = "SHELL", .choices = "bash zsh fish",
     .help = "Print a completion script for bash, zsh or fish"},
    {.name = "--version", .set = setFlag<&Args::show_version>, .help = "Show version"},
    {.name = "--help", .alias = "-h", .set = setFlag<&Args::show_help>, .help = "Show this help"},
};

constexpr string_view kUsageHead = R"(Usage: substrings [OPTIONS] [INPUT]

Suffix array text analysis tool.

Arguments:
  INPUT                      File to read; "-" or omitted reads stdin.
                             With --inline, the input text itself.
                             Put an INPUT that starts with "-" after "--"

Options:
)";

constexpr string_view kUsageTail = R"(
Options with a value also accept it as the next argument (--top 10).

Environment:
  NO_COLOR                   If non-empty, --color=auto doesn't color
  COLUMNS                    Width to fit tables into
  SUBSTRINGS_PAGER, PAGER        Pager for terminal output; "" or "cat" for none
)";

// "-n, --top=N" as shown in --help.
string usageLabel(const Option& option) {
    string label = option.alias.empty() ? string(option.name)
                                        : format("{}, {}", option.alias, option.name);
    if (option.arity == Arity::Required) label += format("={}", option.value_name);
    if (option.arity == Arity::Optional) label += format("[={}]", option.value_name);
    return label;
}

bool isChoice(const Option& option, string_view value) {
    return ranges::contains(option.choices | views::split(' '), value,
                            [](auto word) { return string_view(word); });
}

// "-" alone is an operand (stdin), not an option.
bool looksLikeOption(string_view arg) {
    return arg.starts_with('-') && arg != "-";
}

const Option* findOption(string_view name) {
    auto it = ranges::find_if(kOptions, [name](const Option& o) {
        return o.name == name || (!o.alias.empty() && o.alias == name);
    });
    return it == ranges::end(kOptions) ? nullptr : &*it;
}

size_t editDistance(string_view a, string_view b) {
    vector<size_t> row(b.size() + 1);  // distances from a[0, i) to each prefix of b
    iota(row.begin(), row.end(), 0);
    for (size_t i = 1; i <= a.size(); i++) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            size_t above = row[j];
            row[j] = min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] != b[j - 1])});
            diagonal = above;
        }
    }
    return row[b.size()];
}

// A known long option that `name` abbreviates or is at most 2 edits from.
optional<string_view> suggestionFor(string_view name) {
    if (!name.starts_with("--") || name.size() < 4) return nullopt;
    optional<string_view> best;
    size_t best_distance = 3;
    for (const auto& option : kOptions) {
        if (option.name.starts_with(name)) return option.name;
        size_t distance = editDistance(name, option.name);
        if (distance < best_distance) {
            best = option.name;
            best_distance = distance;
        }
    }
    return best;
}

string unknownOption(string_view name) {
    if (auto suggestion = suggestionFor(name))
        return format("unknown option '{}' (did you mean '{}'?)", name, *suggestion);
    return format("unknown option '{}' (see --help; put an INPUT that starts "
                  "with '-' after '--')", name);
}

} // namespace

span<const Option> options() {
    return kOptions;
}

string usageText() {
    string usage(kUsageHead);
    for (const auto& option : kOptions)
        format_to(back_inserter(usage), "  {:<25}  {}\n", usageLabel(option), option.help);
    return usage + string(kUsageTail);
}

void printUsage() {
    std::print("{}", usageText());
}

expected<Args, string> parseArgs(int argc, const char* const argv[]) {
    Args args;
    string error;  // the first one; deferred so --help always wins
    auto fail = [&error](string message) {
        if (error.empty()) error = std::move(message);
    };
    vector<string_view> operands;
    bool options_ended = false;  // after "--"

    for (int i = 1; i < argc; i++) {
        string_view arg = argv[i];
        if (options_ended || !looksLikeOption(arg)) {
            operands.push_back(arg);
            continue;
        }
        if (arg == "--") {
            options_ended = true;
            continue;
        }

        // Only long options take "=VALUE".
        size_t eq = arg.starts_with("--") ? arg.find('=') : string_view::npos;
        string_view name = arg.substr(0, eq);
        optional<string_view> value;
        if (eq != string_view::npos) value = arg.substr(eq + 1);

        const Option* option = findOption(name);
        if (!option) {
            fail(unknownOption(name));
            continue;
        }
        switch (option->arity) {
            case Arity::None:
                if (value) {
                    fail(format("{} doesn't take a value", name));
                    continue;
                }
                break;
            case Arity::Required:
                // The next argument is the value, unless it looks like an option.
                if (!value && i + 1 < argc && !looksLikeOption(argv[i + 1]))
                    value = argv[++i];
                if (!value) {
                    fail(format("{} needs a value", name));
                    continue;
                }
                break;
            case Arity::Optional:
                if (!value) value = option->implicit_value;
                break;
        }
        if (!option->choices.empty() && !isChoice(*option, *value)) {
            auto listed = option->choices | views::split(' ') | views::join_with(", "sv);
            fail(format("{} must be one of {}, not '{}'", name, ranges::to<string>(listed), *value));
            continue;
        }
        if (string message = option->set(args, *value); !message.empty())
            fail(std::move(message));
    }

    if (operands.size() > 1) {
        string list;
        for (string_view operand : operands)
            list += format("{}'{}'", list.empty() ? "" : ", ", operand);
        fail(format("expected one INPUT, got {}: {} (to analyze text instead of a file, "
                    "use --inline \"TEXT\")", operands.size(), list));
    } else if (operands.size() == 1 && args.input_file) {
        fail("give the input either as INPUT or with -i/--input, not both");
    } else if (args.inline_text) {
        if (operands.empty())
            fail("--inline needs the input text as INPUT: substrings --inline TEXT");
        else
            args.inline_input = string(operands[0]);  // even "-": it's text
    } else if (operands.size() == 1) {
        args.input_file = string(operands[0]);  // "-" is stdin, as with -i
    }

    if (args.json && (args.lz77 || args.bwt || args.view_tables))
        fail("--json works with the repeats, --find, --palindrome and --shortest-unique, "
             "not with --lz77, --bwt or --view-internal-tables");
    if (args.popular.words && (args.lz77 || args.bwt || args.view_tables))
        fail("--words works with the repeats and --find, "
             "not with --lz77, --bwt or --view-internal-tables");

    if (!error.empty() && !args.show_help) return unexpected(error);
    return args;
}
