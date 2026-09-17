#!/usr/bin/env python3
"""
check_rtc_mutex.py — a file that talks to the DS1302 has to take rtcMutex.

WHY THIS EXISTS
---------------
The DS1302 is bit-banged. ThreeWire drives CE and SCLK and turns the IO line
around by hand, one edge at a time; there is no peripheral, no FIFO and no
arbitration. Five contexts reach it in this firmware:

  * pipelineNowEpoch()      — SensorTask, SlowSensorTask and StorageTask, and
                              on a device with no network that is the only
                              clock, so it runs on every tick of all three
  * the deferred /set_time write and backupBootCount() — loop()
  * getRtcDateTimeString() and the hardware status block — the AsyncTCP worker
  * syncTimeFromNTP()       — whichever task ran the sync

None of them used to take anything. Two interleaved transactions on a
bit-banged bus do not fail cleanly: they shift each other's bits, so a read
comes back as a plausible WRONG time (BCD garbage decodes in range more often
than not) and a read landing inside a SetMemory/SetDateTime can write a
register the caller never addressed — the boot count, or the time itself.

Nothing catches that. It compiles, it links, it works on a bench where only
one task is alive, and in the field it is an occasional wrong timestamp and an
occasional corrupted RTC.

So: a file that dereferences `Rtc->` must also mention rtcMutex. The check is
coarse on purpose — it cannot tell whether the lock is held at the right
moment, only whether the file has reckoned with the rule, which is the
difference between the bug above and every correct call site in the tree.

    python3 tools/check_rtc_mutex.py
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN = ['src', '.']

# A DS1302 transaction: any method call through the global Rtc pointer.
USE_RE = re.compile(r'\bRtc\s*->\s*(\w+)\s*\(')

# Saying this counts as having reckoned with the rule. COMMENTS COUNT, for the
# same reason they do in check_fs_mutex.py: a file whose reads run inside a
# caller's lock says so in a comment and must not take a non-recursive mutex
# twice.
AWARE_RE = re.compile(r'rtcMutex')

BLOCK_COMMENT = re.compile(r'/\*.*?\*/', re.S)
LINE_COMMENT = re.compile(r'//[^\n]*')

# The driver itself IS the bus. It is what the lock protects, not a caller.
EXEMPT = {os.path.join('src', 'drivers', 'DS1302_Mini.h')}


def strip_noise(src: str) -> str:
    return LINE_COMMENT.sub(' ', BLOCK_COMMENT.sub(' ', src))


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
                if name.endswith(('.cpp', '.ino', '.h')):
                    p = os.path.join(dirpath, name)
                    yield os.path.relpath(p, ROOT), p


def main() -> int:
    problems = []
    checked = 0
    for rel, path in sources():
        if rel in EXEMPT:
            continue
        with open(path, encoding='utf-8') as fh:
            raw = fh.read()
        hits = sorted({m.group(1) for m in USE_RE.finditer(strip_noise(raw))})
        if not hits:
            continue
        checked += 1
        if AWARE_RE.search(raw):
            continue
        shown = ', '.join('Rtc->%s()' % h for h in hits[:3])
        problems.append(
            '%s: talks to the DS1302 (%s%s) and never mentions rtcMutex'
            % (rel, shown, ', …' if len(hits) > 3 else ''))

    if problems:
        print('FAIL: %d file(s) drive the RTC bus without the lock.\n' % len(problems))
        for p in problems:
            print('  ' + p)
        print('\nThe DS1302 bus is bit-banged and shared by five contexts —\n'
              'take rtcMutex around the transaction:\n'
              '    MutexGuard rg(rtcMutex, pdMS_TO_TICKS(200));\n'
              '    if (rtcMutex && !rg.isLocked()) return;   // or skip\n'
              'and release it before taking any other lock: rtcMutex is the\n'
              'innermost lock in this firmware (see src/pipeline/DataPipeline.h).\n'
              'A null handle means "before TaskManager::init, nothing to guard\n'
              'against" — only a FAILED take is fatal.')
        return 1

    print('OK: all %d file(s) that drive the RTC bus know about rtcMutex.' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
