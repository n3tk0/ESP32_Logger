#!/usr/bin/env python3
"""
check_web_replies.py — no web reply is an AsyncResponseStream.

WHY THIS EXISTS
---------------
An AsyncResponseStream keeps its reply in one buffer that grows by a
realloc per write. When a growth fails, every later byte tries the
allocation again and logs "Failed to allocate"; ArduinoJson writes a byte
at a time. On a C3 one such reply kept the AsyncTCP task in malloc past
the 5 s task watchdog, and opening the Overview or the Sensors page reset
the Collector (2026-10-16, design/c3-heap-design-1016.md). Even a stream
sized up front grows the moment a measured length is wrong.

Replies go through src/utils/JsonResponse.h instead (sendJsonResponse,
BlockPrint + sendBlockPrint): 512 B blocks, never past the measured length,
and a unit of the heap budget (src/core/HeapBudget.h) for as long as the
reply exists.

    python3 tools/check_web_replies.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN = ['src']
BANNED = re.compile(r'\b(beginResponseStream|AsyncResponseStream)\b')


def code_lines(text):
    """The file's lines with // and /* */ comments blanked out."""
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
    return [re.sub(r'//.*', '', l) for l in text.split('\n')]


def main():
    bad = []
    for d in SCAN:
        for dirpath, _, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if not f.endswith(('.cpp', '.h', '.ino', '.c')):
                    continue
                p = os.path.join(dirpath, f)
                with open(p, encoding='utf-8', errors='replace') as fh:
                    for n, line in enumerate(code_lines(fh.read()), 1):
                        if BANNED.search(line):
                            bad.append('%s:%d: %s' % (os.path.relpath(p, ROOT), n, line.strip()))
    if bad:
        print('A web reply built on AsyncResponseStream (use JsonResponse.h):')
        for b in bad:
            print('  ' + b)
        return 1
    print('no AsyncResponseStream in src/')
    return 0


if __name__ == '__main__':
    sys.exit(main())
