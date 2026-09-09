#!/usr/bin/env python3
"""
check_fs_mutex.py — a file that writes the filesystem has to take fsMutex.

WHY THIS EXISTS
---------------
REFACTORING_GUIDELINES Pillar 1.3: every LittleFS / SD write call site MUST
acquire fsMutex. It is an invariant with no compiler behind it — the code that
breaks it compiles, links, runs, and works on the bench.

LittleFS on this part is not re-entrant across FreeRTOS tasks. StorageTask
appends a row to the day's CSV; the async web server serves a file off the same
volume; loop() writes a snapshot. Two of them inside littlefs's metadata at the
same moment corrupts the directory or panics the core — and neither failure
names the code that caused it, or reproduces on demand, or happens on the desk.
It happens in a shed, in February, once.

Two files got this wrong in the same week, written months apart: TrendStore's
24-hour snapshot and EspNowIngest's node table. Both wrote from loop() while
StorageTask wrote CSV. Neither review caught it, because the missing thing is
invisible — there is no line to look at.

So: a file that opens a file for writing, removes, renames, or makes a
directory must also mention fsMutex or go through atomicWrite(). The check is
coarse on purpose. It cannot tell whether the lock is held at the right moment,
only whether the file has heard of it — which is the difference between the two
bugs above and every correct call site in the tree.

    python3 tools/check_fs_mutex.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN = ['src', '.']            # '.' picks up the top-level .ino

# The mutating calls, on either of the two ways the filesystem is named:
# `LittleFS.` / `SD.` directly, or through the `activeFS` pointer the platform
# layer hands around.
WRITE_RE = re.compile(
    r'(?:LittleFS|SD|SD_MMC)\s*\.\s*(remove|rename|mkdir|rmdir)\s*\(|'
    r'(?:activeFS|fs|_fs)\s*->\s*(remove|rename|mkdir|rmdir)\s*\(|'
    r'(?:LittleFS|SD|SD_MMC)\s*\.\s*open\s*\([^;]*?(?:"w"|"a"|FILE_WRITE|FILE_APPEND)|'
    r'(?:activeFS|fs|_fs)\s*->\s*open\s*\([^;]*?(?:"w"|"a"|FILE_WRITE|FILE_APPEND)')

# Saying either of these counts as having heard of the rule, and COMMENTS
# COUNT. atomicWrite() takes fsMutex itself (see src/utils/AtomicWrite.h), and
# a class whose writes run under the caller's lock — CsvLogger, FlowRunLogger,
# both called from StorageTask with the guard already held — says so in a
# comment and must not re-acquire a non-recursive mutex. Both are the file
# having reckoned with the rule, which is the whole of what this can check.
AWARE_RE = re.compile(r'fsMutex|atomicWrite')

# Comments and string literals are stripped before scanning, so a file that
# only DISCUSSES LittleFS.remove() in a comment is not a call site.
BLOCK_COMMENT = re.compile(r'/\*.*?\*/', re.S)
LINE_COMMENT = re.compile(r'//[^\n]*')


def strip_noise(src):
    """The source with its comments blanked, so a call site is a call."""
    src = BLOCK_COMMENT.sub(' ', src)
    src = LINE_COMMENT.sub(' ', src)
    return src


def sources():
    for rel in SCAN:
        base = os.path.join(ROOT, rel)
        if rel == '.':
            for name in sorted(os.listdir(base)):
                if name.endswith('.ino'):
                    yield name, os.path.join(base, name)
            continue
        for dirpath, _dirs, names in os.walk(base):
            for name in sorted(names):
                if name.endswith(('.cpp', '.ino')):
                    p = os.path.join(dirpath, name)
                    yield os.path.relpath(p, ROOT), p


def main():
    problems = []
    checked = 0
    for rel, path in sources():
        with open(path, encoding='utf-8') as fh:
            raw = fh.read()
        # Calls are looked for with the comments stripped, so a file that only
        # DISCUSSES a remove() is not a call site. Awareness is looked for in
        # the raw text, so "the caller holds fsMutex" counts.
        hits = [m.group(0).strip() for m in WRITE_RE.finditer(strip_noise(raw))]
        if not hits:
            continue
        checked += 1
        if AWARE_RE.search(raw):
            continue

        problems.append(
            '%s: writes the filesystem (%s%s) and never mentions fsMutex or '
            'atomicWrite()' % (rel, ', '.join(sorted(set(hits))[:3]),
                               ', …' if len(set(hits)) > 3 else ''))

    if problems:
        print('FAIL: %d file(s) write the filesystem without the lock.\n'
              % len(problems))
        for p in problems:
            print('  ' + p)
        print('\nPillar 1.3: take fsMutex around the write —\n'
              '    MutexGuard guard(fsMutex, pdMS_TO_TICKS(2000));\n'
              '    if (fsMutex && !guard.isLocked()) return false;\n'
              'or go through atomicWrite() (src/utils/AtomicWrite.h), which\n'
              'takes it for you. A null handle means "before TaskManager::init,\n'
              'nothing to guard against" — only a FAILED take is fatal.')
        return 1

    print('OK: all %d file(s) that write the filesystem know about fsMutex.'
          % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
