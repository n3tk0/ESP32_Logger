#!/usr/bin/env python3
"""
check_tempobject_cleanup.py — what is parked in _tempObject gets freed by the
framework, so every parking site needs a cleaner, and a sentinel needs it first.

WHY THIS EXISTS
---------------
`AsyncWebServerRequest::_tempObject` is a bare `void*` the library hands us for
per-request state, and `~AsyncWebServerRequest()` does:

    if (_tempObject != NULL) free(_tempObject);

That one line is the whole reason this check exists. It runs on EVERY way a
request can end, including the ones our code never sees — a browser tab closed
mid-body, an AP that dropped, a client's own timeout firing. And it is a bare
`free()`: it does not run a destructor, and it cannot tell a heap block from
anything else that fits in a pointer.

So two rules, and this firmware has broken both:

  1. A file that parks something in `_tempObject` must register an
     `onDisconnect` cleaner. Without one, an abort before the request callback
     leaks whatever the object owned — `free()` reclaims the block but not a
     String's buffer or a File handle. (PR #89, found by review.)

  2. A file that parks a SENTINEL — a small integer cast to `void*`, used to
     flag a state when there was no memory to allocate a real object — must
     register that cleaner BEFORE the allocation it is flagging the failure of.
     `/import_settings` registered it after, so it covered the success path and
     not the out-of-memory one, and an abort on that path left
     `free((void*)1)` to run: not a leak, not a wrong answer, but heap
     corruption, on the path taken when the device is already short of memory.

Rule 2 is checked by line order within a window, because the thing that went
wrong is an ORDER: going back from the sentinel, the allocation should come
first and the cleaner before it. Registering the cleaner up there costs
nothing at runtime — a lambda capturing one pointer lives inside
std::function, not on the heap — which is why "before" is always available.

    python3 tools/check_tempobject_cleanup.py
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN = ['src']

PARK_RE = re.compile(r'_tempObject\s*=\s*(?!nullptr|NULL|0\s*;)')
SENTINEL_RE = re.compile(r'_tempObject\s*=\s*reinterpret_cast\s*<\s*void\s*\*\s*>\s*\(')
# A CALL, through a request pointer — `->onDisconnect(`, not any identifier
# that ends in those letters. Written this way after the first draft of this
# checker passed its own mutation proof: it matched `_onDisconnect()` inside a
# COMMENT in the file it was checking, which is why comments are stripped
# below as well. A guard that reads prose proves nothing.
CLEANER_RE = re.compile(r'->\s*onDisconnect\s*\(')
ALLOC_RE = re.compile(r'new\s*\(\s*std::nothrow\s*\)')

# How far above a sentinel to look for the allocation and the cleaner. The
# handler that holds both is far smaller than this; the bound only stops the
# search from wandering into a neighbouring handler.
WINDOW = 60


def strip_comments_keep_lines(src: str) -> str:
    """Blank out comments while preserving line numbering.

    Line numbers are the whole mechanism of the order check below, so a
    stripper that removed lines would break it.
    """
    out = []
    i, n = 0, len(src)
    in_line = in_block = in_str = in_chr = False
    while i < n:
        c = src[i]
        two = src[i:i + 2]
        if in_line:
            if c == '\n':
                in_line = False
                out.append(c)
            else:
                out.append(' ')
            i += 1
        elif in_block:
            if two == '*/':
                in_block = False
                out.append('  ')
                i += 2
            else:
                out.append('\n' if c == '\n' else ' ')
                i += 1
        elif in_str or in_chr:
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if (in_str and c == '"') or (in_chr and c == "'"):
                in_str = in_chr = False
            i += 1
        elif two == '//':
            in_line = True
            out.append('  ')
            i += 2
        elif two == '/*':
            in_block = True
            out.append('  ')
            i += 2
        else:
            if c == '"':
                in_str = True
            elif c == "'":
                in_chr = True
            out.append(c)
            i += 1
    return ''.join(out)


def sources():
    for rel in SCAN:
        for dirpath, _dirs, names in os.walk(os.path.join(ROOT, rel)):
            for name in sorted(names):
                if name.endswith(('.cpp', '.h')):
                    p = os.path.join(dirpath, name)
                    yield os.path.relpath(p, ROOT), p


def main() -> int:
    problems = []
    checked = 0
    for rel, path in sources():
        with open(path, encoding='utf-8') as fh:
            lines = strip_comments_keep_lines(fh.read()).splitlines()

        parks = [i for i, l in enumerate(lines) if PARK_RE.search(l)]
        if not parks:
            continue
        checked += 1

        cleaners = [i for i, l in enumerate(lines) if CLEANER_RE.search(l)]

        # Rule 1: something is parked, so a cleaner has to exist.
        if not cleaners:
            problems.append(
                '%s:%d parks state in _tempObject and never registers an '
                'onDisconnect cleaner' % (rel, parks[0] + 1))
            continue

        # Rule 2: a sentinel needs the cleaner registered above the allocation
        # whose failure it reports.
        for i in [i for i in parks if SENTINEL_RE.search(lines[i])]:
            lo = max(0, i - WINDOW)
            allocs = [j for j in range(lo, i) if ALLOC_RE.search(lines[j])]
            near = [j for j in cleaners if lo <= j < i]
            if not near:
                problems.append(
                    '%s:%d sets a _tempObject sentinel with no onDisconnect '
                    'cleaner registered above it' % (rel, i + 1))
                continue
            if allocs and max(near) > min(allocs):
                problems.append(
                    '%s:%d sets a _tempObject sentinel, but the cleaner at '
                    'line %d is registered AFTER the allocation at line %d — '
                    'so it does not cover the failure the sentinel reports'
                    % (rel, i + 1, max(near) + 1, min(allocs) + 1))

    if problems:
        print('FAIL: %d _tempObject site(s) the framework could free badly.\n'
              % len(problems))
        for p in problems:
            print('  ' + p)
        print('\n~AsyncWebServerRequest() runs `free(_tempObject)` on every way\n'
              'a request can end, including a client that just went away. So:\n'
              '  • register the cleaner:\n'
              '        req->onDisconnect([req]() {\n'
              '            delete static_cast<T*>(req->_tempObject);\n'
              '            req->_tempObject = nullptr;\n'
              '        });\n'
              '  • and register it BEFORE the allocation, so it also covers the\n'
              '    out-of-memory path. It allocates nothing itself: a lambda\n'
              '    capturing one pointer is stored inside std::function.\n'
              'A sentinel left behind is not a leak — free() on a value that was\n'
              'never malloc()ed corrupts the heap.')
        return 1

    print('OK: all %d file(s) parking state in _tempObject clean it up, and '
          'every sentinel is covered.' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
