#!/usr/bin/env python3
"""Substrings that repeat in both of two files.

An example of using `substrings` from a script. It runs

    substrings --json -i FILE_A    and    substrings --json -i FILE_B

reads the "repeat" lines of each, and prints the substrings that both
outputs list, as tab-separated values:

    count in FILE_A   count in FILE_B   substring

sorted by the smaller of the two counts, most shared first. Every other
option (--words, --minlength, --top, ...) is passed through to substrings
for both files, so

    scripts/join.py --words --minlength=3 hamlet.txt macbeth.txt

lists the three-word phrases that both plays repeat.

Only substrings that occur at least twice in each file are found, since
that is what substrings lists, and --top limits each file's list before
they are joined. For passages that occur in both files once, use
scripts/compare.py.

The substrings binary is taken from $SUBSTRINGS, else the repository's
build/substrings, else PATH.
"""

import json
import os
import shutil
import subprocess
import sys


def find_substrings():
    if path := os.environ.get("SUBSTRINGS"):
        return path
    built = os.path.join(os.path.dirname(__file__), "..", "build", "substrings")
    if os.access(built, os.X_OK):
        return built
    if path := shutil.which("substrings"):
        return path
    sys.exit("join.py: substrings not found; build it or set $SUBSTRINGS")


def repeats(binary, filename, options):
    """Return {substring: count} for the repeats substrings lists for filename."""
    result = subprocess.run(
        [binary, "--json", "--no-pager", *options, "-i", filename],
        stdout=subprocess.PIPE, check=True, text=True, encoding="utf-8")
    counts = {}
    for line in result.stdout.splitlines():
        entry = json.loads(line)
        if entry["type"] == "repeat":
            counts[entry["text"]] = entry["count"]
    return counts


def escape(text):
    """Keep each result on one line of the TSV output."""
    return (text.replace("\\", "\\\\").replace("\t", "\\t")
                .replace("\n", "\\n").replace("\r", "\\r"))


def main():
    args = sys.argv[1:]
    files = [a for a in args if not a.startswith("-")]
    options = [a for a in args if a.startswith("-")]
    if "-h" in options or "--help" in options:
        print(__doc__)
        return
    if len(files) != 2:
        sys.exit("usage: join.py [SUBSTRINGS OPTIONS] FILE_A FILE_B\n"
                 "Options with a value must use '=' (--top=20).")

    binary = find_substrings()
    a = repeats(binary, files[0], options)
    b = repeats(binary, files[1], options)

    shared = sorted(a.keys() & b.keys(),
                    key=lambda s: (-min(a[s], b[s]), -(a[s] + b[s]), s))
    print(f"# {files[0]}\t{files[1]}\tsubstring")
    for s in shared:
        print(f"{a[s]}\t{b[s]}\t{escape(s)}")


if __name__ == "__main__":
    main()
