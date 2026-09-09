#!/usr/bin/env python3
"""
check_body_handlers.py — a POST body cap above one TCP segment has to accumulate.

WHY THIS EXISTS
---------------
ESPAsyncWebServer delivers a request body to its onBody callback ONE TCP
SEGMENT AT A TIME. The ESP32's MSS is about 1460 bytes, so any body bigger
than that arrives in pieces — always, on every client, every time.

A handler that answers `index != 0 || len != total` with a 413 is therefore
correct only for as long as its own size cap stays under a segment. Raise the
cap and the check does not start failing: it starts rejecting exactly the
bodies the raise was for, and nothing else in the tree notices. /api/ingest
went to 4 KB so a node could hand over a buffered outage, and every batch large
enough to need the new cap was answered "body must arrive in one chunk". The
node holds a batch on any non-200, so a node that fell far enough behind to
send a large batch could never send anything again — the feature failing
precisely when it was needed.

So: a file that declares a body cap above one segment must also accumulate
through `_tempObject`, which is what /api/firstrun and /api/kindle/slots do.

    python3 tools/check_body_handlers.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN = ['src/web']

# One Ethernet MSS on this part. A cap at or below it can be served by a single
# callback in practice; above it, it certainly cannot.
MSS = 1460

# `static constexpr size_t X_MAX_BODY = 4096;`, `constexpr size_t MAX_BODY = ...`,
# `#define X_MAX_BODY 4096`, and the `4 * 1024` spelling.
CAP_RE = re.compile(
    r'(?:constexpr\s+size_t|#\s*define)\s+(\w*MAX_BODY\w*|\w*_MAX_BYTES)\s*=?\s*'
    r'([0-9]+(?:\s*\*\s*[0-9]+)*)\s*;?')

SINGLE_CHUNK_RE = re.compile(r'len\s*!=\s*total|index\s*!=\s*0\s*\|\|')


def value_of(expr):
    n = 1
    for part in expr.split('*'):
        n *= int(part.strip())
    return n


def main():
    problems = []
    for rel in SCAN:
        d = os.path.join(ROOT, rel)
        for name in sorted(os.listdir(d)):
            if not name.endswith('.cpp'):
                continue
            path = os.path.join(d, name)
            src = open(path, encoding='utf-8').read()
            if 'onBody' not in src and 'HTTP_POST' not in src:
                continue

            caps = [(m.group(1), value_of(m.group(2)))
                    for m in CAP_RE.finditer(src)]
            big = [(n, v) for n, v in caps if v > MSS]
            if not big:
                continue

            accumulates = '_tempObject' in src
            refuses = bool(SINGLE_CHUNK_RE.search(src))

            if not accumulates:
                for n, v in big:
                    problems.append(
                        '%s/%s: %s is %d bytes, past the %d-byte segment, but the '
                        'file never accumulates through _tempObject — a body that '
                        'large always arrives split'
                        % (rel, name, n, v, MSS))
            elif refuses:
                for n, v in big:
                    problems.append(
                        '%s/%s: %s is %d bytes AND the file still refuses a body '
                        'that arrives in more than one segment — the two cannot '
                        'both be right' % (rel, name, n, v))

    if problems:
        print('FAIL: %d body handler(s) that cannot receive what they allow.\n'
              % len(problems))
        for p in problems:
            print('  ' + p)
        print('\nSee /api/firstrun and /api/kindle/slots for the shape: allocate '
              'a String on\nindex 0, register onDisconnect immediately, concat '
              'each segment, act on the last.')
        return 1

    print('OK: every POST body cap above one TCP segment accumulates its chunks.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
