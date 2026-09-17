#!/usr/bin/env python3
"""
check_sse_lock.py — the SSE push from loop() is only safe on a locked library.

WHY THIS EXISTS
---------------
`publishLiveEvent()` runs on loop() and walks AsyncEventSource's subscriber
list once a second. That list belongs to the AsyncTCP task: it appends on
connect and, on disconnect, both erases the entry AND destroys the client
object. Two tasks, one list, no handshake — and the losing interleaving is
loop() dereferencing a client the web task has already freed. A browser
reloading the live page is exactly the event that triggers it, so this is not
an exotic race; it is the ordinary one.

Nothing in this repository can fix that, because the list is the library's.
What closes it is WHICH library:

  * esphome/ESPAsyncWebServer-esphome  — send(), count() and _handleDisconnect()
                                         touch the list with no lock at all
  * ESP32Async/ESPAsyncWebServer       — all of them take _client_queue_lock

…and, on the second one, whether that lock is a real mutex. It sits behind
ASYNCWEBSERVER_USE_MUTEX: at 1 it is a std::recursive_mutex, at 0 it is
`null_mutex`, whose lock() has an empty body and evaporates under
optimisation. The header defaults it to 1 on ESP32, so the safety of the push
would otherwise rest on a DEFAULT in a third-party file that no build of ours
states and no reviewer sees.

So this checks the three things the push actually depends on:

  1. every lib_deps block naming a web server names ESP32Async's, >= 3.12.0
  2. ASYNCWEBSERVER_USE_MUTEX=1 is stated in the shared build_flags
  3. AsyncTCP is ESP32Async's, >= 3.5.0 — the floor that library.json requires

THE RETREAT IS DELIBERATE, NOT SILENT
-------------------------------------
If the pin ever has to be given up, the push has to go with it: the live page
already falls back to polling /api/live (www/js/pages.js, liveStartTransport).
This checker notices that retreat and stops demanding the pin — it looks for
the push itself, and says so when it is gone. What it will not allow is the
push staying while the guarantee under it leaves.

    python3 tools/check_sse_lock.py
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIO = os.path.join(ROOT, 'platformio.ini')
PUSH_SITE = os.path.join('src', 'web', 'WebServer.cpp')

# The push: an AsyncEventSource being sent to from a function, not a handler.
# Narrow on purpose — this is the one such call the firmware is allowed to have
# (see the contract above publishLiveEvent()).
PUSH_RE = re.compile(r'\bliveEvents\s*\.\s*send\s*\(')

LINE_COMMENT = re.compile(r'//[^\n]*')
BLOCK_COMMENT = re.compile(r'/\*.*?\*/', re.S)

# A lib_deps entry: "owner/Name @ ^1.2.3", "owner/Name@^1.2.3", "owner/Name".
DEP_RE = re.compile(r'^\s*([A-Za-z0-9_.\-]+)/([A-Za-z0-9_.\-]+)\s*@?\s*'
                    r'\^?~?=?\s*([0-9]+(?:\.[0-9]+)*)?\s*$')

WEB_OWNER, WEB_NAME, WEB_MIN = 'ESP32Async', 'ESPAsyncWebServer', (3, 12, 0)
TCP_OWNER, TCP_NAME, TCP_MIN = 'ESP32Async', 'AsyncTCP', (3, 5, 0)


def strip_ini_comments(src: str) -> str:
    """platformio.ini comments start with ';' (or '#') at any column."""
    out = []
    for line in src.splitlines():
        cut = len(line)
        for ch in (';', '#'):
            i = line.find(ch)
            if i != -1:
                cut = min(cut, i)
        out.append(line[:cut])
    return '\n'.join(out)


def version(text: str | None) -> tuple:
    if not text:
        return ()
    return tuple(int(p) for p in text.split('.'))


def lib_deps_blocks(ini: str):
    """Yield (section, [dependency lines]) for every lib_deps in the file."""
    section = '[none]'
    collecting = False
    block = []
    for line in ini.splitlines():
        if line.startswith('['):
            if collecting:
                yield section, block
            section, collecting, block = line.strip(), False, []
            continue
        if re.match(r'^\s*lib_deps\s*=', line):
            if collecting:
                yield section, block
            collecting, block = True, []
            rest = line.split('=', 1)[1].strip()
            if rest:
                block.append(rest)
            continue
        if collecting:
            if line[:1] in (' ', '\t') and line.strip():
                block.append(line.strip())
            elif line.strip():          # a new key ends the block
                yield section, block
                collecting, block = False, []
    if collecting:
        yield section, block


def check_dep(section, entries, name, owner, floor, problems):
    """One named library, in one lib_deps block, must be owner's and >= floor."""
    seen = [DEP_RE.match(e) for e in entries]
    hits = [m for m in seen if m and m.group(2).lower().startswith(name.lower())]
    # An esphome-style fork carries the name with a suffix; catch it either way.
    forks = [m for m in seen
             if m and name.lower() in m.group(2).lower() and m.group(1) != owner]
    for m in forks:
        problems.append(
            '%s pins %s/%s — the %s the SSE push needs is %s/%s'
            % (section, m.group(1), m.group(2), name, owner, name))
    if not hits:
        return
    for m in hits:
        if m.group(1) != owner:
            continue                    # already reported as a fork above
        if version(m.group(3)) < floor:
            problems.append(
                '%s pins %s/%s @ %s — below the %s floor this needs'
                % (section, m.group(1), m.group(2), m.group(3) or '(any)',
                   '.'.join(str(n) for n in floor)))


def main() -> int:
    with open(os.path.join(ROOT, PUSH_SITE), encoding='utf-8') as fh:
        web = fh.read()
    pushes = PUSH_RE.search(LINE_COMMENT.sub(' ', BLOCK_COMMENT.sub(' ', web)))
    if not pushes:
        print('OK: nothing pushes to the event source from outside the web\n'
              '    task any more, so the library pin is no longer load-bearing.\n'
              '    If that retreat was deliberate, delete this checker with it.')
        return 0

    with open(PIO, encoding='utf-8') as fh:
        ini = strip_ini_comments(fh.read())

    problems = []
    blocks = list(lib_deps_blocks(ini))
    if not blocks:
        problems.append('platformio.ini declares no lib_deps at all')
    for section, entries in blocks:
        check_dep(section, entries, WEB_NAME, WEB_OWNER, WEB_MIN, problems)
        check_dep(section, entries, TCP_NAME, TCP_OWNER, TCP_MIN, problems)

    if not re.search(r'-D\s*ASYNCWEBSERVER_USE_MUTEX\s*=\s*1', ini):
        problems.append(
            'no -DASYNCWEBSERVER_USE_MUTEX=1 in build_flags — the client-list '
            'lock would be whatever the library header defaults to')
    if re.search(r'-D\s*ASYNCWEBSERVER_USE_MUTEX\s*=\s*0', ini):
        problems.append(
            '-DASYNCWEBSERVER_USE_MUTEX=0 is set — that makes every lock in '
            'the web server a null_mutex with an empty lock()')

    if problems:
        print('FAIL: %d problem(s) with what the 1 Hz SSE push rests on.\n'
              % len(problems))
        for p in problems:
            print('  ' + p)
        print('\npublishLiveEvent() walks the event source\'s client list from\n'
              'loop() while the AsyncTCP task adds to and frees from it. That is\n'
              'safe only because ESP32Async/ESPAsyncWebServer takes\n'
              '_client_queue_lock in send(), count(), _addClient(),\n'
              '_handleDisconnect() and close() — and only while\n'
              'ASYNCWEBSERVER_USE_MUTEX is 1, which turns those locks into a\n'
              'std::recursive_mutex instead of a no-op.\n'
              '\nEither restore the pin, or drop the push and let the live page\n'
              'poll /api/live as it already can (www/js/pages.js).')
        return 1

    print('OK: the SSE push has a locked client list under it '
          '(%d lib_deps block(s) checked).' % len(blocks))
    return 0


if __name__ == '__main__':
    sys.exit(main())
