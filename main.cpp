#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <optional>
#include <print>
#include <unistd.h>
#include "cli.h"
#include "completions.h"
#include "lz77.h"
#include "palindromes.h"
#include "suffix_array.h"
#include "suffix_tree.h"
#include "table_viz.h"
#include "terminal.h"

using namespace std;

namespace {

// The --inline text, else the INPUT or -i file, else stdin.
expected<string, string> readInput(const Args& args) {
    if (args.inline_input) return *args.inline_input;

    auto slurp = [](istream& in) {
        return string(istreambuf_iterator<char>(in), istreambuf_iterator<char>());
    };
    if (args.input_file && *args.input_file != "-") {
        // Reject an oversized file before reading it into memory. file_size
        // fails for non-regular files (pipes, /dev/stdin); those are read and
        // then checked by SuffixArray.
        error_code ec;
        auto size = filesystem::file_size(*args.input_file, ec);
        if (!ec && size > kMaxInputBytes)
            return unexpected(format("input file {} is {} bytes; the limit is {} bytes (2 GiB)",
                                     *args.input_file, size, kMaxInputBytes));
        ifstream file(*args.input_file, ios::binary);
        if (!file) {
            string message = "cannot open input file: " + *args.input_file;
            if (!filesystem::exists(*args.input_file, ec))
                message += " (no such file; to analyze the text itself, use --inline)";
            return unexpected(message);
        }
        return slurp(file);
    }
    if (isatty(STDIN_FILENO))
        std::println(stderr, "substrings: reading input from the terminal; end it with Ctrl-D "
                             "(see substrings --help)");
    return slurp(cin);
}

// To stdout, or to -o FILE, so that a build can generate the script files.
int writeCompletions(const Args& args) {
    string script = completionScript(args.completions_shell);
    if (args.output_file.empty()) {
        std::print("{}", script);
        return 0;
    }
    ofstream file(args.output_file, ios::binary);
    file << script;
    if (!file) {
        std::println(stderr, "Error: cannot write {}", args.output_file);
        return 1;
    }
    return 0;
}

bool writeDotFile(const string& text, const string& filename) {
    ofstream dot(filename);
    if (!dot) return false;
    auto tree = buildSuffixTree(text);
    tree.writeDot(dot);
    std::println(stderr, "Suffix tree DOT written to {} ({} nodes)", filename, tree.nodes.size());
    return true;
}

// Everything after argument handling: read, build, write.
int run(const Args& args) {
    auto input = readInput(args);
    if (!input) {
        std::println(stderr, "Error: {}", input.error());
        return 1;
    }

    if (!args.dot_file.empty() && input->size() > kMaxGraphInputBytes && !args.force_graph) {
        std::println(stderr, "Error: --graph is limited to inputs of {} bytes (this one is {}); "
                             "use --force-graph to override",
                     kMaxGraphInputBytes, input->size());
        return 1;
    }

    ofstream outfile;
    if (!args.output_file.empty()) {
        outfile.open(args.output_file, ios::binary);
        if (!outfile) {
            std::println(stderr, "Error: cannot open output file: {}", args.output_file);
            return 1;
        }
    }
    ostream& out = outfile.is_open() ? outfile : cout;
    bool to_terminal = !outfile.is_open() && isatty(STDOUT_FILENO);
    VizOptions viz{.width = outputWidth(to_terminal),
                   .color = shouldColor(args.color, to_terminal)};

    SuffixArray sa(std::move(*input), isatty(STDERR_FILENO));
    bool single_output = args.view_tables || args.bwt || args.lz77;  // ignore the other options
    if (!single_output && !args.dot_file.empty() && !writeDotFile(sa.getText(), args.dot_file)) {
        std::println(stderr, "Error: cannot open DOT file: {}", args.dot_file);
        return 1;
    }

    // Started after the build, so the progress bar and the messages above
    // stay out of the pager. Its destructor waits for the user to quit it.
    optional<Pager> pager;
    if (to_terminal && args.use_pager) pager.emplace(pagerCommand());

    if (args.view_tables) {
        printTables(out, sa, viz);
        return 0;
    }
    if (args.bwt) {
        printBwt(out, sa, viz);
        return 0;
    }
    if (args.lz77) {
        printLz77(out, sa);
        return 0;
    }
    PopularOptions popular = args.popular;
    popular.format = args.json ? ListFormat::Json
                   : shouldEscape(args.escape, to_terminal) ? ListFormat::Escaped
                                                            : ListFormat::Raw;
    if (args.palindrome)
        printPalindromes(out, sa.getText(), popular.format);
    if (args.shortest_unique)
        sa.printShortestUnique(out, popular.format);

    if (args.find_patterns.empty())
        sa.printPopular(out, popular);
    else
        sa.printMatches(out, args.find_patterns, popular);
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    auto args = parseArgs(argc, argv);
    if (!args) {
        std::println(stderr, "Error: {}", args.error());
        return 1;
    }
    if (args->show_help) {
        printUsage();
        return 0;
    }
    if (args->show_version) {
        std::println("substrings {}", kVersion);
        return 0;
    }
    if (!args->completions_shell.empty())
        return writeCompletions(*args);

    try {
        return run(*args);
    } catch (const bad_alloc&) {
        std::println(stderr, "Error: out of memory (substrings needs roughly 17 bytes of RAM "
                             "per input byte)");
    } catch (const exception& e) {
        std::println(stderr, "Error: {}", e.what());
    }
    return 1;
}
