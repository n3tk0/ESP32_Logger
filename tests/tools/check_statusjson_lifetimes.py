#!/usr/bin/env python3
"""Fail if a module's statusJson() hands ArduinoJson a pointer that dies first.

THE BUG THIS EXISTS FOR. ForecastModule::statusJson() did:

    const Data d = snapshot();          // a copy, on this stack frame
    out["summary"] = d.summary;         // char[40] -> const char*

ArduinoJson 7 LINKS a const char* rather than copying it, so the document
held a pointer into a frame that was gone by the time sendJsonResponse()
serialised it. The bytes written out were whatever had since been pushed
over that stack: garbage that differed between requests, and — when it
happened to contain a raw control byte, which ArduinoJson emits unescaped —
a response no browser could parse. That took down /api/modules entirely,
and every module with it, because the index embeds each module's status.

The rule is therefore: inside statusJson(), a string assigned into `out`
must own its bytes or outlive the response. A String does. A char buffer
belonging to a local does not. Every other module already assigned a
String; this checks that it stays that way, and that the next module added
does the same.

Scalars are fine — they are copied by value — so only string-typed members
are flagged. Assigning a string literal is fine too: static lifetime.

Run:  python3 tests/tools/check_statusjson_lifetimes.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "src" / "modules"

# A local built from a call — `const Data d = snapshot();`, `auto x = f();`.
LOCAL_FROM_CALL = re.compile(
    r"^[ \t]*(?:const\s+)?[\w:]+(?:\s*&)?\s+(\w+)\s*=\s*[\w:.]+\([^;]*\)\s*;",
    re.M)
# An assignment of one of that local's members into the JsonObject.
OUT_ASSIGN = re.compile(r"out\[[^\]]+\]\s*=\s*(\w+)\.(\w+)\s*;")
STATUS_FN = re.compile(
    r"void\s+(\w+)::statusJson\(JsonObject\s+out\)\s*const\s*\{(.*?)\n\}", re.S)


def member_is_string(header_text, member):
    """True when `member` is declared as a char buffer or pointer."""
    decl = re.search(
        r"^\s*(?:const\s+)?char\s+\*?\s*" + re.escape(member) + r"\s*(\[|;|=)",
        header_text, re.M)
    return decl is not None


def main():
    problems = []
    checked = 0
    for cpp in sorted(SRC.glob("*.cpp")):
        text = cpp.read_text(encoding="utf-8")
        header = cpp.with_suffix(".h")
        header_text = header.read_text(encoding="utf-8") if header.exists() else ""
        for cls, body in STATUS_FN.findall(text):
            checked += 1
            locals_ = set(LOCAL_FROM_CALL.findall(body))
            if not locals_:
                continue
            for var, member in OUT_ASSIGN.findall(body):
                if var not in locals_:
                    continue
                if not member_is_string(header_text, member):
                    continue          # a scalar; copied by value
                problems.append(
                    f"{cpp.relative_to(ROOT)}: {cls}::statusJson() assigns "
                    f"`{var}.{member}` — a char buffer owned by the local "
                    f"`{var}`, which dies when this function returns, while "
                    f"ArduinoJson only links it. Wrap it: String({var}.{member})")

    if not checked:
        print("check_statusjson_lifetimes: matched no statusJson() at all — "
              "the pattern has drifted, so this check is proving nothing.",
              file=sys.stderr)
        return 1

    if problems:
        print("check_statusjson_lifetimes: dangling string(s) handed to "
              "ArduinoJson:\n", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1

    print(f"check_statusjson_lifetimes: {checked} statusJson() bodies, "
          f"no local buffer linked into the response")
    return 0


if __name__ == "__main__":
    sys.exit(main())
