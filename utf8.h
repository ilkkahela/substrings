#pragma once
#include <algorithm>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>

// Continuation bytes (10xxxxxx) are the 2nd-4th bytes of a multi-byte character.
constexpr bool isUtf8Continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

constexpr std::string encodeUtf8(char32_t cp) {
    std::string result;
    if (cp < 0x80) {
        result += static_cast<char>(cp);
    } else if (cp < 0x800) {
        result += static_cast<char>(0xC0 | (cp >> 6));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        result += static_cast<char>(0xE0 | (cp >> 12));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        result += static_cast<char>(0xF0 | (cp >> 18));
        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return result;
}

constexpr std::string encodeUtf8(std::u32string_view codepoints) {
    std::string result;
    for (char32_t cp : codepoints) result += encodeUtf8(cp);
    return result;
}

struct Utf8Char {
    char32_t codepoint;  // U+FFFD when !valid
    int length;          // bytes used; an invalid byte counts as 1
    bool valid;

    bool operator==(const Utf8Char&) const = default;
};

// Decodes the character starting at s[i], strictly: a truncated sequence, a
// stray continuation byte, an overlong encoding, a UTF-16 surrogate or a value
// past U+10FFFF is reported as a single invalid byte.
constexpr Utf8Char decodeUtf8Char(std::string_view s, size_t i) {
    constexpr Utf8Char kInvalid{U'�', 1, false};
    auto lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) return {lead, 1, true};

    int extra;
    char32_t smallest;  // anything below this has a shorter encoding
    if (lead >= 0xC2 && lead <= 0xDF)      { extra = 1; smallest = 0x80; }
    else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; smallest = 0x800; }
    else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; smallest = 0x10000; }
    else return kInvalid;
    if (s.size() - i <= static_cast<size_t>(extra)) return kInvalid;

    char32_t cp = lead & (0x3F >> extra);
    for (int k = 1; k <= extra; k++) {
        if (!isUtf8Continuation(s[i + k])) return kInvalid;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    if (cp < smallest || cp > 0x10FFFF || surrogate) return kInvalid;
    return {cp, extra + 1, true};
}

// A sequence cut off by the end of the input decodes to U+FFFD.
constexpr std::u32string decodeUtf8(std::string_view utf8) {
    std::u32string result;
    for (size_t i = 0; i < utf8.size();) {
        auto lead = static_cast<unsigned char>(utf8[i]);
        // How many continuation bytes (10xxxxxx) follow the lead byte.
        int extra = lead < 0x80 ? 0 : lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
        if (i + extra >= utf8.size()) {
            result += U'�';
            break;
        }

        // The lead byte carries 7, 5, 4 or 3 payload bits; each continuation byte 6.
        char32_t cp = extra == 0 ? lead : lead & (0x3F >> extra);
        for (int k = 1; k <= extra; k++)
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        result += cp;
        i += extra + 1;
    }
    return result;
}

struct CodepointRange {
    char32_t first, last;
};

constexpr bool inAny(char32_t cp, std::initializer_list<CodepointRange> table) {
    return std::ranges::any_of(table, [cp](CodepointRange r) { return r.first <= cp && cp <= r.last; });
}

// Characters that are invisible or change how the terminal lays out text:
// C1 controls, soft hyphen, zero-width characters, line and paragraph
// separators, bidirectional overrides and isolates, byte order mark.
constexpr bool isInvisible(char32_t cp) {
    return inAny(cp, {{0x80, 0x9F}, {0xAD, 0xAD}, {0x200B, 0x200F}, {0x2028, 0x202E},
                      {0x2060, 0x2069}, {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB}});
}

// Escapes bytes reversibly for one tab-separated field: \t \n \r \\ as C
// escapes; other control characters, DEL and invalid or truncated UTF-8 as
// \xNN per byte; invisible characters (all below U+10000) as \uXXXX. Valid
// printable UTF-8 is kept, so plain text stays readable.
inline std::string escapeBytes(std::string_view s) {
    std::string result;
    for (size_t i = 0; i < s.size();) {
        auto ch = decodeUtf8Char(s, i);
        auto byte = static_cast<unsigned char>(s[i]);
        if (!ch.valid || ch.codepoint < 0x20 || ch.codepoint == 0x7F) {
            switch (ch.codepoint) {
                case '\t': result += "\\t"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                default:   std::format_to(std::back_inserter(result), "\\x{:02X}", byte);
            }
        } else if (ch.codepoint == '\\') {
            result += "\\\\";
        } else if (isInvisible(ch.codepoint)) {
            std::format_to(std::back_inserter(result), "\\u{:04X}",
                           static_cast<uint32_t>(ch.codepoint));
        } else {
            result += s.substr(i, ch.length);
        }
        i += ch.length;
    }
    return result;
}

// How listings (repeats, matches, palindromes, unique substrings) show text:
// as raw bytes, escaped with escapeBytes so that line breaks and control
// characters can't break up the lines, or as JSON Lines.
enum class ListFormat { Raw, Escaped, Json };

// s as a quoted JSON string. JSON strings hold only Unicode text, so invalid
// UTF-8 becomes U+FFFD; control characters become escapes.
inline std::string jsonString(std::string_view s) {
    std::string result = "\"";
    for (size_t i = 0; i < s.size();) {
        auto ch = decodeUtf8Char(s, i);
        switch (ch.codepoint) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            case '\r': result += "\\r"; break;
            default:
                if (!ch.valid)
                    result += encodeUtf8(U'\uFFFD');
                else if (ch.codepoint < 0x20)
                    std::format_to(std::back_inserter(result), "\\u{:04x}",
                                   static_cast<uint32_t>(ch.codepoint));
                else
                    result += s.substr(i, ch.length);
        }
        i += ch.length;
    }
    return result + '"';
}

// Elides the middle of a long string so it fits on one line.
constexpr std::u32string abbreviate(std::u32string_view s) {
    constexpr size_t kMaxLength = 80, kKeep = 38;
    if (s.size() <= kMaxLength) return std::u32string(s);
    return std::u32string(s.substr(0, kKeep)) + U"..." +
           std::u32string(s.substr(s.size() - kKeep));
}
