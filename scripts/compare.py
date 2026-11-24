#!/usr/bin/env python3
"""Find the passages that two texts share.

Prints every maximal run of at least --min-words words (default 8) that
occurs in both files, longest first, as tab-separated values:

    words   line:col in FILE_A   line:col in FILE_B   passage

Locations list every occurrence, separated by commas. Words are compared,
not what lies between them, so a passage still matches across a different
line break, punctuation or spacing; the passage is shown as it appears first
in FILE_A, with whitespace collapsed. A word is a run of letters and digits,
so "don't" is two words, as with `substrings --words`.

A prototype for a future `substrings` feature: it uses an n-gram index, not a
suffix array, and a passage repeated many times in both files is compared
pairwise, which gets slow for heavy boilerplate.
"""

import argparse
import bisect
import json
import re
import sys
from collections import defaultdict

WORD = re.compile(r"[^\W_]+")


class Text:
    def __init__(self, name, data, ignore_case, vocabulary):
        self.name = name
        self.data = data
        self.spans = []  # (start, end) character offsets of each word
        self.ids = []    # word ids, shared with the other text
        for match in WORD.finditer(data):
            word = match.group().casefold() if ignore_case else match.group()
            self.spans.append(match.span())
            self.ids.append(vocabulary.setdefault(word, len(vocabulary)))
        self.line_starts = [0] + [m.end() for m in re.finditer("\n", data)]

    def location(self, word_index):
        """line:col (both from 1, col in characters) of a word."""
        offset = self.spans[word_index][0]
        line = bisect.bisect_right(self.line_starts, offset)
        return line, offset - self.line_starts[line - 1] + 1

    def passage(self, first, words):
        start = self.spans[first][0]
        end = self.spans[first + words - 1][1]
        return " ".join(self.data[start:end].split())


def read(path):
    if path == "-":
        return sys.stdin.buffer.read().decode("utf-8", errors="replace")
    with open(path, "rb") as f:
        return f.read().decode("utf-8", errors="replace")


def shared_passages(a, b, min_words):
    """{word ids of a passage: (positions in a, positions in b)}."""
    index = defaultdict(list)
    for i in range(len(a.ids) - min_words + 1):
        index[tuple(a.ids[i:i + min_words])].append(i)

    found = defaultdict(lambda: (set(), set()))
    for j in range(len(b.ids) - min_words + 1):
        for i in index.get(tuple(b.ids[j:j + min_words]), ()):
            # Start only where the match can't extend to the left.
            if i > 0 and j > 0 and a.ids[i - 1] == b.ids[j - 1]:
                continue
            length = min_words
            while (i + length < len(a.ids) and j + length < len(b.ids)
                   and a.ids[i + length] == b.ids[j + length]):
                length += 1
            in_a, in_b = found[tuple(a.ids[i:i + length])]
            in_a.add(i)
            in_b.add(j)
    return found


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog="Example: compare.py --min-words=12 hamlet.txt pg100.txt")
    parser.add_argument("file_a", metavar="FILE_A", help='a text file ("-" for stdin)')
    parser.add_argument("file_b", metavar="FILE_B", help="another text file")
    parser.add_argument("-w", "--min-words", type=int, default=8, metavar="N",
                        help="shortest passage to report, in words (default: 8)")
    parser.add_argument("-i", "--ignore-case", action="store_true",
                        help="compare words regardless of case")
    parser.add_argument("-n", "--top", type=int, metavar="N",
                        help="print only the N longest passages")
    parser.add_argument("--json", action="store_true",
                        help="print JSON Lines instead of tab-separated values")
    args = parser.parse_args()
    if args.min_words < 1:
        parser.error("--min-words must be at least 1")
    if args.file_a == "-" and args.file_b == "-":
        parser.error("only one of the files can be stdin")

    vocabulary = {}
    try:
        a = Text(args.file_a, read(args.file_a), args.ignore_case, vocabulary)
        b = Text(args.file_b, read(args.file_b), args.ignore_case, vocabulary)
    except OSError as e:
        sys.exit(f"compare.py: {e}")

    passages = []
    for ids, (in_a, in_b) in shared_passages(a, b, args.min_words).items():
        in_a, in_b = sorted(in_a), sorted(in_b)
        passages.append((len(ids), in_a, in_b))
    passages.sort(key=lambda p: (-p[0], p[1][0]))
    if args.top is not None:
        passages = passages[:args.top]

    out = sys.stdout
    if not args.json:
        out.write(f"# words\t{a.name}\t{b.name}\tpassage\n")
    for words, in_a, in_b in passages:
        text = a.passage(in_a[0], words)
        if args.json:
            locations = lambda t, ps: [dict(zip(("line", "col"), t.location(p))) for p in ps]
            out.write(json.dumps({"words": words, "a": locations(a, in_a),
                                  "b": locations(b, in_b), "text": text},
                                 ensure_ascii=False) + "\n")
        else:
            where = lambda t, ps: ",".join("%d:%d" % t.location(p) for p in ps)
            out.write(f"{words}\t{where(a, in_a)}\t{where(b, in_b)}\t{text}\n")


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:  # e.g. piped into head
        sys.stderr.close()
