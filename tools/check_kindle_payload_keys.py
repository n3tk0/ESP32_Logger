#!/usr/bin/env python3
"""
check_kindle_payload_keys.py — a key the collector sends must be one the
Kindle script will accept.

THE SEAM THIS GUARDS
--------------------
GET /kindle/data is a shell fragment: the firmware writes KEY="value" lines
and kindle/update_dash.sh reads them with load_kv(). That reader is not
permissive — payload_key_ok() is an allowlist, because the file is fetched
over the network and then fed to `eval`, so a key it does not recognise is
dropped rather than assigned.

Which makes the two sides a contract with no compiler between them. A key
added to the firmware and not to the allowlist is silently discarded on every
fetch: the field is simply never drawn, and nothing anywhere says why. That is
how MONTH_LABEL and YEAR came to be sent for nothing.

WHAT IS CHECKED
---------------
Every LITERAL key the firmware emits — kdShellVar(s, "KEY", …),
kdShellVarUpper(), and "KEY=" inside a print/printf — must be accepted by
payload_key_ok(), or be one of the few the script's own cache writes.

Keys the firmware builds at runtime (snprintf(key, …, "Z_%s_VALUE", zone) and
the WK%d_/FC%d_ families) are NOT checked: their names do not exist in the
source to compare, and the allowlist covers them by prefix. Checking what can
be checked beats a check that has to guess.

AND THE LAYOUT'S KEYS BOTH WAYS. kdFlowPanelKeys() in src/web/KindleFlow.h
sends LY_<name> for every value the layout moves, and the script's flow_apply()
assigns only the names in its FLOW_KEYS list. A name on one side and not the
other is a value worked out and thrown away, or one the panel waits for and
never gets — either way a section drawn where the layout file put it rather
than where the page has room. So the two lists must be the same list.

    python3 tools/check_kindle_payload_keys.py
"""
from __future__ import annotations

import fnmatch
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.join(ROOT, 'src', 'web', 'KindleDashboard.cpp')
SCRIPT = os.path.join(ROOT, 'kindle', 'update_dash.sh')
FLOW = os.path.join(ROOT, 'src', 'web', 'KindleFlow.h')

# END is read by payload_ok() with a direct `grep -q '^END=1'` on the file,
# deliberately outside load_kv() and therefore outside the allowlist: it is the
# marker that says the payload is complete, so it must be readable even when
# the rest of the parse is refused.
NOT_VIA_ALLOWLIST = {'END'}


def emitted_literal_keys(src: str) -> set:
    keys = set()
    for m in re.finditer(r'kdShellVar(?:Upper)?\s*\(\s*s\s*,\s*"([A-Za-z0-9_]+)"', src):
        keys.add(m.group(1))
    # "KEY=" inside a print/printf string, but not a %d-family template
    for m in re.finditer(r'"([A-Z][A-Z0-9_]{1,24})=', src):
        keys.add(m.group(1))
    return keys


def accept_patterns(sh: str) -> list:
    start = sh.index('payload_key_ok')
    end = sh.index('load_kv()')
    pats = []
    for line in sh[start:end].splitlines():
        m = re.match(r'^\s*([A-Za-z0-9_\[\]\|\*\-]+(?:\s*\|\s*[A-Za-z0-9_\[\]\|\*\-]+)*)\)\s*return 0',
                     line.strip())
        if m:
            pats += [p.strip() for p in m.group(1).split('|')]
    return pats


def main() -> int:
    src = open(FIRMWARE, encoding='utf-8').read()
    sh = open(SCRIPT, encoding='utf-8').read()

    keys = emitted_literal_keys(src) - NOT_VIA_ALLOWLIST
    pats = accept_patterns(sh)
    if not pats:
        print('FAIL: could not read payload_key_ok() out of kindle/update_dash.sh — '
              'this check is stale and is not checking anything.')
        return 1

    dropped = sorted(k for k in keys
                     if not any(fnmatch.fnmatchcase(k, p) for p in pats))
    if dropped:
        print('FAIL: the collector sends %d key(s) the Kindle script drops.\n' % len(dropped))
        for k in dropped:
            print('  %s — emitted by src/web/KindleDashboard.cpp, refused by '
                  'payload_key_ok()' % k)
        print('\nEither add it to the allowlist in kindle/update_dash.sh (if the\n'
              'panel should use it) or stop emitting it. A key that reaches the\n'
              'device and is thrown away is a field that never draws, with\n'
              'nothing to say why.')
        return 1

    print('OK: all %d literal payload key(s) the collector sends are accepted '
          'by the Kindle script (%d patterns).' % (len(keys), len(pats)))

    flow = open(FLOW, encoding='utf-8').read()
    body = flow[flow.index('kdFlowPanelKeys(const KdFlow'):]
    body = body[:body.index('return n;')]
    sent = set(re.findall(r'KDF_[KR]\(\s*"([A-Z0-9_]+)"', body))
    m = re.search(r'^FLOW_KEYS="([^"]*)"', sh, re.M)
    if not sent or not m:
        print('FAIL: could not read the layout keys out of KindleFlow.h or '
              'FLOW_KEYS out of update_dash.sh — this check is stale.')
        return 1
    applied = set(m.group(1).split())
    if sent != applied:
        for k in sorted(sent - applied):
            print('FAIL: LY_%s is sent by kdFlowPanelKeys() and never applied — '
                  'add it to FLOW_KEYS in kindle/update_dash.sh' % k)
        for k in sorted(applied - sent):
            print('FAIL: FLOW_KEYS applies %s, which the collector never sends — '
                  'the panel keeps the layout file\'s value for it' % k)
        return 1
    print('OK: the %d layout keys the collector sends are the %d the Kindle '
          'script applies.' % (len(sent), len(applied)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
