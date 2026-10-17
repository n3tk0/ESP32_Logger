#!/usr/bin/env python3
"""
check_json_char_arrays.py — a char-array struct member goes into a
JsonDocument through jsonCopy(), never as the array itself.

ArduinoJson 7.3+ takes any `const char[N]` lvalue for a string literal and
keeps only a pointer to it, no copy (Strings/Adapters/RamString.hpp,
StringAdapter<const char (&)[N]>). Through a const reference or pointer,
`r.sensorId` is exactly that type. When the struct is freed or changed before
the document is serialized, the reply carries whatever is in that memory now:
/api/latest freed its readings before sending, and its replies came out
shorter or longer than measureJson() said (2026-10-16). Through a
`const char*` (what jsonCopy() returns) the string is copied into the
document.

The check finds assignments through [], add() and set() calls whose value is
`<expr>.<field>`, `<expr>-><field>` or a bare `_<field>` (a member, inside a
method) where <field> is declared somewhere in
src/ as `char <field>[...]`, and fails unless the value goes through
jsonCopy() (src/utils/JsonCopy.h). jsonCopy() takes only an array, so a
same-named field that is a pointer or a number fails to compile; such a line,
or one that is not JSON at all, says so in a comment naming this check.
Copying a member that is not const changes nothing: it was copied already.

Usage:  python3 tools/check_json_char_arrays.py [--fix]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
EXTS = {".c", ".cpp", ".h", ".hpp", ".ino"}


def sources():
    return [p for p in SRC.rglob("*") if p.suffix in EXTS and p.is_file()]


def array_members(files):
    names = set()
    decl = re.compile(r"\bchar\s+(\w+)\s*\[[^\]]+\]\s*(?:=[^;]*|\{[^;]*\})?;")
    for p in files:
        names.update(decl.findall(p.read_text(errors="ignore")))
    return names


def pattern(names):
    alt = "|".join(sorted(names, key=len, reverse=True))
    # `[k] = x.f;`  `.add(x.f)`  `.set(x.f)` — the value with nothing around it
    return re.compile(
        r"(?P<head>(?:\]\s*=|\.add\(|\.set\()\s*)"
        r"(?P<val>(?:[A-Za-z_](?:[\w\[\]\.]|->)*(?:\.|->)(?:" + alt + r")"
        # a bare member (`_name`, by this code's convention): inside a const
        # method it is a const char[N] too
        r"|(?:" + "|".join(sorted((n for n in names if n.startswith("_")), key=len, reverse=True)) + r")))\b"
        r"(?=\s*[;)])")


def main():
    fix = "--fix" in sys.argv[1:]
    files = sources()
    pat = pattern(array_members(files))
    bad = []
    for p in files:
        text = p.read_text(errors="ignore")
        lines = text.split("\n")
        changed = False
        for i, line in enumerate(lines):
            code = line.split("//", 1)[0]
            if "check_json_char_arrays" in line[len(code):]:
                continue    # a field of that name that is not an array here
            if not pat.search(code):
                continue
            if fix:
                new = pat.sub(lambda m: m.group("head") + "jsonCopy(" + m.group("val") + ")", code)
                lines[i] = new + (line[len(code):])
                changed = True
            else:
                bad.append(f"{p.relative_to(ROOT)}:{i + 1}: {line.strip()}")
        if changed:
            p.write_text("\n".join(lines))
    if bad:
        print("FAIL: a char-array member goes into JSON uncast; ArduinoJson keeps a pointer\n"
              "to a const one instead of a copy. Write jsonCopy(x.field):")
        for b in bad:
            print("  " + b)
        return 1
    print("OK: every char-array member goes into JSON through jsonCopy()")
    return 0


if __name__ == "__main__":
    sys.exit(main())
