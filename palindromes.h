#pragma once
#include <ostream>
#include <string_view>
#include "utf8.h"

// Prints distinct maximal palindromes of a UTF-8 text, longest first; at most
// 1000 of them.
void printPalindromes(std::ostream& out, std::string_view utf8_text,
                      ListFormat format = ListFormat::Raw);
