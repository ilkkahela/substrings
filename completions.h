#pragma once
#include <string>
#include <string_view>

// A completion script for `shell` ("bash", "zsh" or "fish"; "" for another),
// generated from the option table in cli.cpp. Options whose value is a FILE
// complete file names, options with fixed choices complete those.
[[nodiscard]] std::string completionScript(std::string_view shell);
