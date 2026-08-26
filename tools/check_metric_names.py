#!/usr/bin/env python3
"""
check_metric_names.py — fail the build on a metric name that would be truncated.

SensorReading stores its strings in fixed char arrays and SensorReading::make()
copies with strncpy(dst, src, sizeof(dst) - 1). A name one character too long is
therefore not a compile error, not a warning, and not a runtime failure: it is
silently stored short. Every later strcmp() against the full name then misses,
so the metric appears to exist (the plugin advertises it, MQTT discovery
publishes it) while nothing ever matches it.

That is not hypothetical. "humidity_ambient" is exactly 16 characters, was
stored as "humidity_ambien", and never matched anywhere from the day it shipped.
This script exists so the next one fails loudly instead.

Checked, with the limits read out of SensorTypes.h rather than hardcoded:
  - the metric / unit / type literals passed to SensorReading::make()
  - the same for the _makeReading() wrappers plugins define
  - every string literal inside a getMetrics() body

Usage:
    python3 tools/check_metric_names.py          # verify (exit 1 on a violation)
    python3 tools/check_metric_names.py --list   # print every name it extracted
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TYPES_H = "src/core/SensorTypes.h"
SCAN_DIRS = ["src/sensors", "src/modules", "src/tasks"]

# Argument index of each fixed-width field in the two factory signatures.
#   SensorReading::make(ts, id, type, metric, value, unit)
#   _makeReading(ts, metric, value, unit)          (plugin-local wrapper)
MAKE_FIELDS = {2: "sensorType", 3: "metric", 5: "unit"}
WRAP_FIELDS = {1: "metric", 3: "unit"}


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def field_limits():
    """Usable characters per field, from the struct itself: char[N] holds N-1."""
    text = read(TYPES_H)
    limits = {}
    for name, size in re.findall(r"char\s+(\w+)\[(\d+)\]", text):
        limits.setdefault(name, int(size) - 1)
    for required in ("metric", "unit", "sensorType"):
        if required not in limits:
            sys.exit("error: could not find char %s[N] in %s" % (required, TYPES_H))
    return limits


def split_args(text, start):
    """Split the argument list of a call whose '(' is at `start`.

    Returns (args, end) with parens, brackets and string literals balanced, so
    nested calls like getType() and multi-line calls both survive intact.
    """
    depth, arg, args, i = 0, [], [], start
    while i < len(text):
        c = text[i]
        if c in '"\'':
            j = i + 1
            while j < len(text) and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            arg.append(text[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
            if depth == 1 and i == start:
                i += 1
                continue
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(arg).strip())
                return args, i
        elif c == "," and depth == 1:
            args.append("".join(arg).strip())
            arg = []
            i += 1
            continue
        arg.append(c)
        i += 1
    return args, len(text)


LITERAL = re.compile(r'^"((?:[^"\\]|\\.)*)"$')


def literal(arg):
    """The contents of `arg` if it is a single string literal, else None."""
    m = LITERAL.match(arg.strip())
    return m.group(1) if m else None


def matching_brace(text, start):
    depth, i = 0, start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return len(text)


def scan_file(path, text):
    """Yield (field, name, line) for every fixed-width literal in `text`."""
    for call, fields in ((r"SensorReading::make\s*\(", MAKE_FIELDS),
                         (r"\b_makeReading\s*\(", WRAP_FIELDS)):
        for m in re.finditer(call, text):
            args, _ = split_args(text, text.index("(", m.end() - 1))
            for idx, field in fields.items():
                if idx < len(args):
                    name = literal(args[idx])
                    if name is not None:
                        yield field, name, text.count("\n", 0, m.start()) + 1

    for m in re.finditer(r"\bgetMetrics\s*\([^)]*\)[^{;]*\{", text):
        body = text[m.end() - 1:matching_brace(text, m.end() - 1) + 1]
        for lit in re.finditer(r'"((?:[^"\\]|\\.)*)"', body):
            line = text.count("\n", 0, m.end() + lit.start()) + 1
            yield "metric", lit.group(1), line


def sources():
    for d in SCAN_DIRS:
        for dirpath, _, files in os.walk(os.path.join(ROOT, d)):
            for f in sorted(files):
                if f.endswith((".cpp", ".h")):
                    full = os.path.join(dirpath, f)
                    yield os.path.relpath(full, ROOT), full


def main():
    limits = field_limits()
    listing = "--list" in sys.argv
    violations, seen = [], set()

    for rel, full in sources():
        with open(full, encoding="utf-8") as fh:
            text = fh.read()
        for field, name, line in scan_file(rel, text):
            if listing and (field, name) not in seen:
                seen.add((field, name))
                print("%-11s %2d/%2d  %s" % (field, len(name), limits[field], name))
            if len(name) > limits[field]:
                violations.append((rel, line, field, name, limits[field]))

    if violations:
        print("\nFAIL: %d name(s) longer than the field that stores them.\n"
              "strncpy() truncates silently, so every strcmp() against the full\n"
              "name misses at runtime with no error anywhere.\n" % len(violations))
        for rel, line, field, name, limit in violations:
            print('  %s:%d  %s "%s" is %d chars, %s holds %d -> stored as "%s"'
                  % (rel, line, field, name, len(name), field, limit, name[:limit]))
        return 1

    if not listing:
        print("OK: every metric/unit/type literal fits its field.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
