#!/usr/bin/env python3
"""
tools/win_console.py — keep a child process from opening a console window.

Windows hands a console-subsystem child (python.exe, pio.exe, esptool) a
console window of its own whenever the parent has none — which is every step
the frozen deploy GUI runs, because it is built windowed (console=False in
deploy_gui.spec). Those windows carry no output the GUI is not already showing
in its log, so they are suppressed.

One module rather than a copy in each caller: deploy_core.py and
flash_bootloader.py both need it, the ctypes probe below is the fragile part,
and a fix applied to one copy leaves the other launching subprocesses
differently for the same reason.
"""

import sys

# The flag itself, so callers do not have to know the number.
CREATE_NO_WINDOW = 0x08000000


def has_console() -> bool:
    """True when this process is attached to a console window.

    Answers True on any error, which is the conservative direction: it means
    "assume there is a terminal", so output is never detached from one that
    exists. The cost of being wrong the other way is a stray console window;
    the cost of being wrong this way is a step whose output nobody sees.
    """
    if sys.platform != "win32":
        return True
    try:
        import ctypes
        return bool(ctypes.windll.kernel32.GetConsoleWindow())
    except Exception:
        return True


def no_window(inherits_console: bool = False) -> dict:
    """subprocess kwargs that keep a child from opening a console window.

    Withheld only for a child that writes straight to a console we are already
    attached to: there the window is the terminal being read, and detaching the
    child from it would swallow its output.
    """
    if sys.platform != "win32":
        return {}
    if inherits_console and has_console():
        return {}
    return {"creationflags": CREATE_NO_WINDOW}
