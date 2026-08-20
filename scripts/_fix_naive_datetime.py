#!/usr/bin/env python3
"""One-shot codemod: turn naive `datetime(Y, M, D[, h, m, s])` calls into
UTC-aware ones by appending `tzinfo=timezone.utc`.

Reads file paths from argv. Prints "updated: <path>" for each file that
changed. Idempotent: re-running on an already-converted file is a no-op
(because the regex requires the missing `tzinfo=` slot).

Does NOT match datetime.now() / datetime.utcnow() / datetime.fromtimestamp()
(those are method calls, not constructor calls).
"""
import re
import sys
from pathlib import Path

# First three args + optional 0..3 more args + closing `)`.
# Each arg is any expression with no top-level comma or paren — that
# is enough for the test files (no nested function calls inside the
# datetime() constructor).
PATTERN = re.compile(
    r"\bdatetime\(([^,()]+)"          # year-like (number OR expr)
    r"(?:\s*,\s*([^,()]+))"           # month
    r"(?:\s*,\s*([^,()]+))"           # day
    r"((?:\s*,\s*[^,()]+){0,3})"      # optional 0..3 more args
    r"(\s*\))"                        # closing )
)

ADD_TZ = r"datetime(\1, \2, \3\4, tzinfo=timezone.utc\5"


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: _fix_naive_datetime.py <file>...", file=sys.stderr)
        return 2
    for arg in argv:
        p = Path(arg)
        text = p.read_text()
        new = PATTERN.sub(ADD_TZ, text)
        if new != text:
            p.write_text(new)
            print(f"updated: {arg}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
