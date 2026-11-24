# substrings

Suffix array text analysis tool in C++23. See README.md for usage and file layout.

## Build & test

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/substrings_tests
```

libsais is fetched via FetchContent at configure time and built with OpenMP
when the compiler has it (`SUBSTRINGS_USE_OPENMP`, falls back to single-threaded;
`suffix_array.cpp` picks the `_omp` or plain libsais calls via `LIBSAIS_OPENMP`).
All sources except `main.cpp` go into the `substrings_core` static library, which both `substrings` and
`substrings_tests` link.

## Architecture

- `SuffixArray` (suffix_array.h) works on raw bytes. Construction: libsais SA →
  rank (inverse SA) → libsais PLCP → libsais LCP. `collectRepeats()` (behind
  `getPopular()` and `printPopular()`) makes two passes of the same
  monotonic-stack walk over LCP intervals (`forEachRepeatGroup`): the first
  finds the top_n's cutoff frequency, the second generates only repeats at or
  above it; ties at the cutoff go through a small max-heap keyed by
  lexicographic order (SA index of the interval, length). Never generate
  every repeated substring: repetitive text has Θ(n²) of them, so the passes
  count each interval's lengths at once (`EndFilter`, a sorted list of
  allowed or excluded end positions). `Units` says what may be listed: any
  bytes (`getPopular`), whole characters (`printPopular`), or whole words
  (`--words`: the walk runs over the word-start suffixes only, and the
  interval's own depth is counted per child, since only the byte after it
  decides a word end). Whether a repeat may end somewhere must depend only on
  its own bytes (and for words, one byte after), or counts go wrong. The
  brute-force tests (`TopNMatchesBruteForce`, `NonAsciiTopNMatchesBruteForce`,
  `WordsMatchBruteForce`) pin down the exact ranking, ties included.
  `matchRange()` (binary search over SA) backs `--find`.
  `shortestUnique()` is a single O(n) pass over rank/LCP, counting length in
  UTF-8 characters.
- `buildSuffixTree(text)` appends a `'\x01'` sentinel, builds its own SuffixArray,
  and derives the tree topology, leaf positions and suffix links from it.
  `writeDot` shortens edge labels to 16 bytes (a leaf's full label would make
  the file quadratic), and `main` refuses `--graph` for inputs over
  `kMaxGraphInputBytes` (cli.h) unless `--force-graph` is given.
- `lz77Factorize()` is the Kärkkäinen–Kempa–Puglisi O(n) greedy LZ77: one
  stack pass over SA gives each position's previous/next smaller text position
  in SA order (the only two candidate sources), then direct byte comparison.
  Byte-level; `printLz77` escapes the text column reversibly with
  `escapeBytes` (utf8.h), which shares `isInvisible` with `printTables`.
- `printBwt` (table_viz.cpp) needs no BWT construction: rotations of
  text + "$" sort like the SA with the "$" suffix first, and keeping only
  character starts gives the character-level order (UTF-8 byte order is
  codepoint order). It reuses `printTables`' glyphs and colors.
- Palindromes work on UTF-32 codepoints and only need the text, not the SA.
- `printTables` never prints input bytes raw: control characters become
  control pictures, invisible/bidi characters and invalid UTF-8 become escapes
  (via the strict `decodeUtf8Char` in utf8.h), and widths come from a small
  built-in table instead of the locale.
- `cli.cpp` keeps every option in one `kOptions` table (name, alias, arity,
  setter, value name, choices, help). `--help` and the completion scripts
  (completions.cpp) are generated from it, so a new option only goes there,
  plus `substrings.1` (a test checks the man page mentions every option).
  Unknown options, missing values and a second INPUT are errors (deferred,
  so `--help` still works).
- Listings (repeats, `--find` matches, palindromes, unique substrings) take a
  `ListFormat` (utf8.h): Raw, Escaped (`escapeBytes`, the `--escape` default
  on a terminal) or Json. `main` resolves it from `--json`/`--escape` into
  `PopularOptions::format`; `When` (terminal.h) is the auto/always/never type
  shared by `--color` and `--escape`.
- `main` decides terminal-dependent things once, before writing: colors and
  width (`shouldColor`, `outputWidth` in terminal.h) go to the table printers
  as `VizOptions`; table_viz never looks at the terminal itself. The `Pager`
  is started after the SA build (so the progress bar stays out of it) and
  redirects fd 1, so code keeps writing to `cout`.
- Output functions take an `std::ostream&`; `main` picks `cout` or the `-o` file.
  Don't write results with bare `std::print(...)` (that goes to C stdout and
  bypasses `-o`). Diagnostics go to stderr.

`scripts/compare.py` is a standalone Python prototype (shared passages of
two files, word n-grams); `scripts/join.py` is a documented example that
intersects the `--json` repeats of two files. Neither is part of the build.

## Internal arrays

- `sa[i]`: text position of i-th lexicographically smallest suffix
- `rank_[p]`: rank of suffix at text position p (underscore avoids `std::rank`)
- `lcp[i]`: longest common prefix of suffixes sa[i-1] and sa[i]
- `plcp[p]`: lcp[rank_[p]]

## Conventions

- Headers don't use `using namespace std;`; .cpp files do.
- Classes: PascalCase. Functions/methods: camelCase. Members/fields: snake_case.
- File-local helpers go in an anonymous namespace.

## Git

- Branch: `main`. No remote.

## Test data

- `pg100.txt` (untracked): Complete Shakespeare (~5.6 MB) for large-scale runs.
