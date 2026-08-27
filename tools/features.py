#!/usr/bin/env python3
"""
features.py — one place that knows which optional features this project builds.

The same reasoning as pio_envs.py, which exists because the board list had been
copied into four tools and all four had gone stale. A hand-maintained list of
compile-time features would go the same way: somebody adds a feature to
setup.h, the deploy tools do not offer it, and the only people who can turn it
on are the ones who already knew it existed.

So the answer is read out of src/setup.h itself. Add a guarded block there and
every tool picks it up; there is nothing else to remember.

    from features import optional_features, build_flags_for

    for f in optional_features():
        print(f.macro, f.group, f.summary)

Run it directly to see what the deploy tools will offer:

    python3 tools/features.py

WHAT COUNTS AS OPTIONAL, AND WHY IT IS ONLY HALF THE FILE
---------------------------------------------------------
Only the blocks that are COMMENTED OUT in setup.h — the off-by-default ones.
They can be switched on from outside with -DNAME, which is what these tools do
and what CI does, and which touches no file.

The ones that are on by default cannot be switched off the same way. setup.h
writes them as

    #ifndef FEATURE_SD_STORAGE
    #  define FEATURE_SD_STORAGE
    #endif

so passing -UFEATURE_SD_STORAGE before the include achieves nothing: the header
defines it again a line later. Turning one of those off means editing setup.h,
and that is a source change a person should make deliberately rather than
something a flash tool does behind their back. They are listed by
`always_on_features()` so the tools can SAY so instead of silently offering a
switch that would not work.
"""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    from pio_envs import _project_root  # type: ignore
except ImportError:  # running from another directory
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from pio_envs import _project_root  # type: ignore


@dataclass(frozen=True)
class Feature:
    macro: str        #: e.g. "FEATURE_ESPNOW_INGEST"
    summary: str      #: the trailing comment on the define line, may be ""
    group: str        #: "Sensors" / "Modules" / "Features" / "Exporters"
    enabled: bool     #: how src/setup.h ships it


#: Prefix → the heading a person would expect to find it under. Derived from
#: the macro rather than from setup.h's section banners, because a banner is
#: prose and gets reworded while a prefix is part of the name.
_GROUPS = (
    ("SENSOR_", "Sensors"),
    ("MODULE_", "Modules"),
    ("EXPORT_", "Exporters"),
    ("FEATURE_", "Features"),
)

#: `//#  define NAME   // trailing comment`  — the off-by-default shape, and
#: `#  define NAME     // trailing comment`  — the on-by-default one.
_DEFINE = re.compile(
    r"^(?P<off>//)?#\s*define\s+"
    r"(?P<macro>(?:SENSOR|MODULE|EXPORT|FEATURE)_[A-Z0-9_]+)\s*"
    r"(?://\s*(?P<summary>.*?))?\s*$"
)


def _group_for(macro: str) -> str:
    for prefix, name in _GROUPS:
        if macro.startswith(prefix):
            return name
    return "Other"


def _setup_h() -> Path:
    root = _project_root()
    if root is None:
        raise RuntimeError("cannot locate the project root (no platformio.ini above)")
    return root / "src" / "setup.h"


def all_features() -> list[Feature]:
    """Every toggle setup.h declares, in file order, on and off alike."""
    text = _setup_h().read_text(encoding="utf-8", errors="replace")

    out: list[Feature] = []
    seen: set[str] = set()
    for raw in text.split("\n"):
        line = raw.strip()
        m = _DEFINE.match(line)
        if not m:
            continue
        macro = m.group("macro")
        # A macro can appear twice: FEATURE_REMOTE_NODES is both its own
        # optional block and a line inside the FEATURE_ESPNOW_INGEST implication
        # that turns it on. The first appearance is the declaration; the second
        # is a consequence and must not overwrite it or the feature would read
        # as always-on.
        if macro in seen:
            continue
        # A macro with a value (EXPORT_BATCH_SIZE 20) is a tuning constant, not
        # a toggle. The regex already refuses those by requiring the name to be
        # the last thing on the line before any comment.
        seen.add(macro)
        out.append(Feature(
            macro=macro,
            summary=(m.group("summary") or "").strip(),
            group=_group_for(macro),
            enabled=m.group("off") is None,
        ))
    return out


def optional_features() -> list[Feature]:
    """The ones a build flag can switch ON. See the note at the top of the file."""
    return [f for f in all_features() if not f.enabled]


def always_on_features() -> list[Feature]:
    """The ones setup.h enables by default, which -D cannot switch off."""
    return [f for f in all_features() if f.enabled]


def grouped(features: list[Feature]) -> dict[str, list[Feature]]:
    """Same features, bucketed by group, groups in _GROUPS order."""
    out: dict[str, list[Feature]] = {}
    for _, name in _GROUPS:
        rows = [f for f in features if f.group == name]
        if rows:
            out[name] = rows
    other = [f for f in features if f.group == "Other"]
    if other:
        out["Other"] = other
    return out


def is_known(macro: str) -> bool:
    return any(f.macro == macro for f in optional_features())


def build_flags_for(macros, extra: dict[str, str] | None = None) -> str:
    """The PLATFORMIO_BUILD_FLAGS string that turns `macros` on.

    An env var and not an edit to setup.h, deliberately. The USB CDC toggle in
    deploy_core.py has to rewrite platformio.ini because the flag it changes
    lives there, and the comments on that function are a catalogue of what goes
    wrong when a tool edits the project's own source: sections that run to the
    end of the file, duplicated flags appended on every run, a build comment
    rewritten into nonsense. Nothing here touches a file, so a failed or
    abandoned deploy leaves the checkout exactly as it was.

    `extra` carries string-valued flags such as the ESP-NOW key, quoted the way
    the compiler needs.
    """
    parts = [f"-D{m}" for m in macros if is_known(m)]
    for key, value in (extra or {}).items():
        if value:
            parts.append(f'-D{key}=\\"{value}\\"')
    return " ".join(parts)


def main() -> int:
    opt = optional_features()
    print(f"Optional — a build flag can switch these ON ({len(opt)}):\n")
    for group, rows in grouped(opt).items():
        print(f"  {group}")
        for f in rows:
            print(f"    {f.macro:<32} {f.summary}")
        print()

    always = always_on_features()
    print(f"On by default — setup.h must be edited to remove these ({len(always)}):\n")
    for group, rows in grouped(always).items():
        names = ", ".join(f.macro for f in rows)
        print(f"  {group:<12} {names}")
    return 0


if __name__ == "__main__":
    # `features.py | grep -q NAME` closes the pipe as soon as it matches, and
    # without this Python turns that into a BrokenPipeError traceback on
    # stderr. The CI check does exactly that, and a tool that prints a stack
    # trace when it worked is a tool people stop trusting.
    try:
        sys.exit(main())
    except BrokenPipeError:
        try:
            sys.stdout.close()
        finally:
            sys.exit(0)
