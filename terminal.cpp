#include "terminal.h"
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <spawn.h>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

extern char** environ;

namespace {

// The value of an environment variable, or "" if it is unset.
string_view env(const char* name) {
    const char* value = getenv(name);
    return value ? value : "";
}

} // namespace

bool shouldColor(When mode, bool to_terminal) {
    switch (mode) {
        case When::Always: return true;
        case When::Never:  return false;
        case When::Auto:   break;
    }
    return to_terminal && env("NO_COLOR").empty() && env("TERM") != "dumb";
}

bool shouldEscape(When mode, bool to_terminal) {
    return mode == When::Always || (mode == When::Auto && to_terminal);
}

int outputWidth(bool to_terminal) {
    string_view columns = env("COLUMNS");
    int width = 0;
    auto [end, ec] = from_chars(columns.data(), columns.data() + columns.size(), width);
    if (ec == errc{} && end == columns.data() + columns.size() && width > 0)
        return width;

    winsize w{};
    if (to_terminal && ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

string pagerCommand() {
    for (const char* name : {"SUBSTRINGS_PAGER", "PAGER"})
        if (getenv(name)) return string(env(name));
    return "less";
}

Pager::Pager(const string& command) {
    if (command.empty() || command == "cat") return;

    int fds[2];
    if (pipe(fds) != 0) return;
    setenv("LESS", "FRX", 0);  // only if the user hasn't set LESS

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    const char* argv[] = {"sh", "-c", command.c_str(), nullptr};
    int failed = posix_spawn(&pid, "/bin/sh", &actions, nullptr,
                             const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (failed) {
        pid = -1;
    } else {
        cout.flush();
        fflush(stdout);
        dup2(fds[1], STDOUT_FILENO);
        // Ctrl-C in the pager reaches this process too. Ignoring it keeps us
        // from exiting under a running pager; quitting the pager ends us
        // through SIGPIPE instead, which is a normal way to finish.
        signal(SIGINT, SIG_IGN);
        signal(SIGPIPE, [](int) { _exit(0); });
    }
    close(fds[0]);
    close(fds[1]);
}

Pager::~Pager() {
    if (!active()) return;
    cout.flush();
    fflush(stdout);
    close(STDOUT_FILENO);  // EOF for the pager
    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
}
