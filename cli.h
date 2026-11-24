#pragma once
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "suffix_array.h"
#include "terminal.h"

inline constexpr const char* kVersion = "1.0.0";

// Graphviz can't usefully lay out the suffix tree of a larger input.
inline constexpr size_t kMaxGraphInputBytes = 10'000;

struct Args {
    bool show_help = false;
    bool show_version = false;
    bool view_tables = false;
    bool palindrome = false;
    bool shortest_unique = false;
    bool lz77 = false;
    bool bwt = false;
    bool force_graph = false;  // allow --graph above kMaxGraphInputBytes
    bool use_pager = true;
    bool json = false;
    When color = When::Auto;
    When escape = When::Auto;
    PopularOptions popular;                 // popular.format is decided in main
    std::vector<std::string> find_patterns; // --find, in order
    std::optional<std::string> inline_input;
    std::optional<std::string> input_file;  // "-" for stdin
    std::string output_file;                // empty for stdout
    std::string dot_file;
    std::string completions_shell;          // --completions: print this shell's script
};

// How an option takes its value.
enum class Arity {
    None,      // a flag
    Required,  // --name=VALUE or --name VALUE
    Optional,  // --name=VALUE, or --name alone for Option::implicit_value
};

// Applies an option's value to Args; returns an error message, or "".
using Setter = std::string (*)(Args&, std::string_view value);

// One command-line option. The table of these (options()) drives parsing,
// --help and the shell completions.
struct Option {
    std::string_view name;                 // "--top"
    std::string_view alias = "";           // "-n"
    Arity arity = Arity::None;
    Setter set = nullptr;
    std::string_view value_name = "";      // in --help; "FILE" values complete as file names
    std::string_view choices = "";         // the allowed values, space-separated, if fixed
    std::string_view implicit_value = "";  // Arity::Optional given without "="
    std::string_view help = "";
};

[[nodiscard]] std::span<const Option> options();

// Rejects unknown options (suggesting a close match), missing values and more
// than one INPUT. Everything after "--" is input text, even if it starts with
// "-". Errors are deferred so that --help always works.
[[nodiscard]] std::expected<Args, std::string> parseArgs(int argc, const char* const argv[]);

[[nodiscard]] std::string usageText();
void printUsage();
