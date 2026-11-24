#pragma once
#include <string>
#include <sys/types.h>

// For --color and --escape: Auto decides by whether output goes to a terminal.
enum class When { Auto, Always, Never };

// Whether to color output. Auto colors only output to a terminal, and not if
// NO_COLOR is set to a non-empty value (https://no-color.org) or TERM is "dumb".
[[nodiscard]] bool shouldColor(When mode, bool to_terminal);

// Whether to escape line breaks and control characters in listings: by
// default only in output to a terminal, where they would garble the lines.
[[nodiscard]] bool shouldEscape(When mode, bool to_terminal);

// Columns to lay tables out in: $COLUMNS if it is a positive number, else the
// terminal's width when writing to one, else 80.
[[nodiscard]] int outputWidth(bool to_terminal);

// The pager command: $SUBSTRINGS_PAGER, else $PAGER, else "less". Empty or "cat"
// means no pager.
[[nodiscard]] std::string pagerCommand();

// While alive, stdout is piped into a pager (run with `sh -c`, LESS=FRX unless
// LESS is set, as git does: quit if one screen, keep colors, don't clear).
// The destructor closes stdout and waits for the pager to exit, so the shell
// prompt doesn't return while the pager is still showing output.
class Pager {
    pid_t pid = -1;

public:
    explicit Pager(const std::string& command);
    ~Pager();
    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    [[nodiscard]] bool active() const { return pid > 0; }
};
