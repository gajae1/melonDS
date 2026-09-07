#!/usr/bin/env python3
"""Extract a current source definition for dependency-isolated regression tests.

This follows melonDS's unindented function-closing brace convention. It does not
parse general C++; changed formatting is deliberately an error, not a fallback
to a stale copy of the implementation.
"""
import pathlib
import sys

source, signature, output = sys.argv[1:]
text = pathlib.Path(source).read_text(encoding="utf-8")
needle = signature + "\n{\n"
if text.count(needle) != 1:
    raise SystemExit(f"Expected exactly one definition of {signature!r}")
start = text.index(needle)
end = text.index("\n}", start) + 2
body = text[start:end]
if body.count("{") != body.count("}"):
    raise SystemExit("Function extraction needs updating: unbalanced braces")
pathlib.Path(output).write_text(body + "\n", encoding="utf-8")
