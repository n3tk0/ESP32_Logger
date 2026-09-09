#!/usr/bin/env python3
"""
drive_deploy_gui.py — click the deploy GUI's build-feature widgets, headless.

WHY THIS EXISTS
---------------
CI already smoke-tests that the GUI opens and stays up for twenty seconds.
That proves every widget constructed, which is worth having and is not the same
as proving any of them does what it says. A checkbox wired to the wrong
variable, a validation message that never updates, an implied feature that is
ticked in the config but not on screen — all of those open a window that stays
up perfectly.

So this builds the real window under Xvfb, drives the widgets the way a click
does, and asserts on what came out the far end: the config, the label text, and
finally the PLATFORMIO_BUILD_FLAGS string the build would actually get. The
last one is the point. A flag that reaches the config and not the compiler is
the failure this whole path could plausibly have had.

It writes to the tool's own config file (.flash_tool.json, gitignored), which
is why it saves and restores it.

    xvfb-run -a python3 tests/gui/drive_deploy_gui.py
"""
from __future__ import annotations

import builtins
import contextlib
import io
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import tkinter as tk              # noqa: E402
import customtkinter as ctk        # noqa: E402
import deploy_core as dc           # noqa: E402
import deploy_gui as g             # noqa: E402
import flash_bootloader as fb      # noqa: E402

FAILURES: list[str] = []


def check(cond: bool, what: str) -> None:
    if cond:
        print(f"  ok   {what}")
    else:
        FAILURES.append(what)
        print(f"  FAIL {what}")


def _all_label_text(widget) -> str:
    """Every piece of text painted anywhere in the window, concatenated.

    Walks the widget tree rather than reading the source that built it: the
    question is what a person sees, and a panel that stopped being packed is
    exactly the regression worth catching.
    """
    out = []
    try:
        text = widget.cget("text")
        if isinstance(text, str):
            out.append(text)
    except Exception:
        pass
    for child in getattr(widget, "winfo_children", lambda: [])():
        out.append(_all_label_text(child))
    return "\n".join(out)


def _code_only(path: Path) -> str:
    """The file's executable text: comments and string literals removed.

    A naive substring search over the source fails on the very comments that
    explain why these calls are gone — the docstring on the WiFi tab says the
    words "CTkToplevel" and "grab_set" precisely because they are no longer
    called anywhere.
    """
    import tokenize
    kept = []
    with path.open("rb") as fh:
        for tok in tokenize.tokenize(fh.readline):
            if tok.type in (tokenize.COMMENT, tokenize.STRING):
                continue
            kept.append(tok.string)
    return " ".join(kept)


def _find_button(widget, text: str):
    """The first CTkButton in the tree whose caption is `text`."""
    try:
        if isinstance(widget, ctk.CTkButton) and widget.cget("text") == text:
            return widget
    except Exception:
        pass
    for child in getattr(widget, "winfo_children", lambda: [])():
        found = _find_button(child, text)
        if found is not None:
            return found
    return None


def _hint_of(app, kind: str, key: str) -> str:
    """Rebuild the hint text the window attaches to a preset button.

    Asserting on the STRING the window builds, rather than reaching into the
    binding, keeps this readable — and the string is the whole point: the
    printed line is gone, so what the hint says is now the only place the
    sentence exists on screen.
    """
    steps = dc.PRESETS[key][1]
    return (f"{dc.PRESET_BLURBS[key]}\n\nSteps "
            f"{', '.join(str(n) for n in steps)}:\n" +
            "\n".join(f"  {n}.  {dc.step_parts(n)[0]}" for n in steps))


def _toplevels(root) -> list:
    """Every extra window the application has open, main window excluded."""
    return [w for w in root.winfo_children()
            if isinstance(w, (tk.Toplevel, ctk.CTkToplevel))]


def run_window(app) -> None:
    """The window itself: no second windows, readable type, a visible plan.

    These are the properties the window was rebuilt for, and every one of them
    is the kind that silently rots — a messagebox re-added in a hurry, a font
    tuple slipped back into a new widget, a Run button that stops saying what
    it will do. None of them stops the window opening, which is all the
    twenty-second CI smoke test can tell you.
    """
    print("The window:")

    # ── No pop-ups. Not one, from any path a person can reach ───────────────
    #
    # The old window had three: a warning box on startup, an input dialog for
    # the serial port, and a modal Toplevel for WiFi. Two of them could open
    # BEHIND the main window, and the third took a grab while doing it, which
    # is indistinguishable from a hung program.
    code = _code_only(ROOT / "tools" / "deploy_gui.py")
    for banned in ("messagebox", "CTkInputDialog", "CTkToplevel", "grab_set",
                   "simpledialog"):
        check(banned not in code, f"no {banned} in deploy_gui.py's code")

    app._refresh_ports()
    app._save_config()
    app._on_wifi()
    app._apply_preset([])
    check(_toplevels(app.root) == [],
          "rescan, save, WiFi and an empty preset open no second window")

    # ── The hover hints are a label, not a tooltip WINDOW ───────────────────
    #
    # The per-item explanations were printed under every preset, step and
    # feature, which roughly doubled the height of three panels. They hover
    # now — and the thing that appears has to be drawn INSIDE this window: a
    # tooltip window is the same object as the dialogs above, an
    # override-redirect child the window manager places, and it would put back
    # exactly what the rest of this file asserts is gone.
    balloon = app.balloon
    check(not isinstance(balloon._frame, (tk.Toplevel, ctk.CTkToplevel)),
          "the hint is a framed label, not a Toplevel")
    check(str(balloon._frame.winfo_toplevel()) == str(app.root),
          "and it belongs to the main window")

    app.tab_view.set(g.TAB_RUN)
    app.root.update()
    button = _find_button(app.root, dc.PRESETS["Q"][0])
    check(button is not None, f"the {dc.PRESETS['Q'][0]!r} button is findable")
    if button is not None:
        balloon._show(button, "hover text under test")
        app.root.update()
        check(balloon.visible, "a hint appears for the widget under the pointer")
        check(_toplevels(app.root) == [], "and opens no window to do it")
        x, y = balloon._frame.winfo_x(), balloon._frame.winfo_y()
        check(0 <= x and 0 <= y, f"it is placed inside the window ({x},{y})")
        balloon.hide()
        app.root.update()
        check(not balloon.visible, "and it goes away again")

    # Every hint still SAYS what the printed line said, so nothing was lost
    # with the pixels: the presets carry their blurb, the steps their command.
    balloon._show(button, _hint_of(app, "preset", "Q"))
    check("everyday" in balloon.text.lower() and "5, 6" in balloon.text,
          f"the preset hint keeps its sentence and names its steps: "
          f"{balloon.text.splitlines()[0][:44]!r}")
    balloon.hide()

    # ── The type is readable, and follows the scale control ─────────────────
    #
    # CTkFont sizes are PIXELS. The old window asked for ("Helvetica", 8) —
    # a POINT size, and the smallest text in it carried the ESP-NOW key
    # warning and the USB CDC pin note.
    smallest = min(f.cget("size") for f in app.fonts._fonts.values())
    check(smallest >= 14, f"nothing is smaller than 14 px (smallest {smallest})")

    body = app.fonts["body"].cget("size")
    app._apply_scale(1.4)
    bigger = app.fonts["body"].cget("size")
    check(bigger > body, f"A+ enlarges the body font ({body} → {bigger})")
    check(app.cfg.get("ui_scale") == 1.4, "and the scale is remembered")
    app._apply_scale(3.0)
    check(app.fonts.scale <= g.SCALE_MAX,
          f"an absurd scale is clamped ({app.fonts.scale})")
    app._apply_scale(1.0)

    # ── The Run button says what it is about to do ──────────────────────────
    app._apply_preset(dc.PRESETS["F"][1])
    plan = app.plan_label.cget("text")
    check("Erase chip flash" in plan,
          f"the plan names the steps it will run: {plan.splitlines()[0]!r}")
    check(str(app.run_btn.cget("state")) == "normal", "and RUN is enabled")

    app._apply_preset([])
    check("Nothing selected" in app.plan_label.cget("text"),
          "an empty selection says so instead of failing at the press")
    check(str(app.run_btn.cget("state")) == "disabled",
          "and RUN is disabled rather than opening a warning box")
    app._apply_preset(dc.PRESETS["Q"][1])

    # ── Both halves of a step name survive ──────────────────────────────────
    #
    # The sidebar used to truncate the padded catalogue string at 28
    # characters, which cut step 11 down to "Compile node firmware pio ru…".
    # Step 11 is also the one whose two halves are separated by a single
    # space, so it is the one a naive split gets wrong.
    title, detail = dc.step_parts(11)
    check(title == "Compile node firmware" and detail == "pio run -d node…",
          f"step 11 splits into its two halves: {title!r} / {detail!r}")
    check(dc.STEP_NAMES[11] == "Compile node firmware pio run -d node…",
          "and the CLI's padded catalogue line is unchanged")

    # ── The port picker is a list, not a dialog ─────────────────────────────
    check(g.AUTO_PORT in app.port_box.cget("values"),
          "the port picker offers auto-detect")
    app.port_box.set("★ /dev/ttyUSB7  —  CP2102 USB to UART")
    check(app._current_port() == "/dev/ttyUSB7",
          f"a picked label resolves to a device: {app._current_port()!r}")
    app.port_box.set("COM9")
    check(app._current_port() == "COM9", "and a typed port is taken as typed")
    app.port_box.set(g.AUTO_PORT)
    check(app._current_port() == "", "auto-detect means no pinned port")

    # ── deploy_core's output is not re-wrapped ──────────────────────────────
    #
    # The core supplies its own line endings; the GUI used to append another
    # to each, which double-spaced every message and put each inline "✓" on a
    # line under the file it was ticking.
    app._clear_logs()
    app._log_output("  ↑  /www/app.js   1234 B … ")
    app._log_output(" ✓\n")
    app.root.update()
    # The first line only: the startup summary lands in the same box.
    line = app.log_text.get("1.0", "1.end").strip()
    check(line.endswith("…  ✓"),
          f"an inline tick completes the line it belongs to: {line!r}")
    app._clear_logs()

    # ── The feature filter ──────────────────────────────────────────────────
    app.feature_filter.delete(0, "end")
    app.feature_filter.insert(0, "co2")
    app._apply_feature_filter()
    visible = [row for _hay, row, _g in app._feature_rows
               if row.winfo_manager() == "pack"]
    check(0 < len(visible) < len(app._feature_rows),
          f"filtering narrows the feature list ({len(visible)} of "
          f"{len(app._feature_rows)})")
    app._clear_feature_filter()
    app.root.update()
    check(all(row.winfo_manager() == "pack"
              for _h, row, _g in app._feature_rows),
          "and clearing it brings every feature back")
    print()


class _FakePopen:
    """A child process that never starts. Records how it was asked to."""

    def __init__(self, cmd, **kwargs):
        _FakePopen.calls.append((list(cmd), kwargs))
        self.stdout = io.StringIO("")
        self.returncode = 0

    def wait(self, timeout=None):
        return 0

    calls: list = []


class _NeverEndingPopen:
    """A child that keeps talking until somebody terminates it — step 9.

    `pio device monitor` does not finish. The GUI's worker thread sits in
    _run_cmd()'s read loop over its output for as long as it runs, which is
    for ever, and the only thing that ends it is the far end of the pipe
    closing. That is what cancel() has to make happen, and it is not
    observable with a Popen whose stdout is an empty string.
    """

    def __init__(self, cmd, **kwargs):
        _NeverEndingPopen.last = self
        self.cmd = list(cmd)
        self.terminated = threading.Event()
        self.killed = False
        self.stdout = self
        self.returncode = None

    # Read as an iterator, the way _run_cmd() does: a line a millisecond
    # until terminate() closes it.
    def __iter__(self):
        while not self.terminated.is_set():
            time.sleep(0.001)
            yield "monitoring…\n"

    def close(self):
        pass

    def terminate(self):
        self.terminated.set()
        self.returncode = -15

    def kill(self):
        self.killed = True
        self.terminate()

    def wait(self, timeout=None):
        # Never returns until terminated, exactly like the real thing.
        self.terminated.wait(timeout if timeout is not None else 10)
        if not self.terminated.is_set():
            raise subprocess.TimeoutExpired(self.cmd, timeout or 10)
        return self.returncode if self.returncode is not None else 0

    last: "Optional[_NeverEndingPopen]" = None


def run_stop(app) -> None:
    """The serial monitor, and getting the window back afterwards.

    WHY THIS EXISTS
    ---------------
    Step 9 is a monitor: it runs until stopped. The GUI had nothing that
    stopped it — RUN went grey and said "⏳ Running…", the worker thread sat
    in the read loop, and `pio device monitor` kept the serial port until the
    window was killed. Nothing else could be flashed in the meantime, and
    nothing on screen suggested that killing the window was the way out.
    """
    print("Driving the stop button:")

    mgr = dc.DeployManager(app.cfg)
    said: list[str] = []
    mgr.on_step_output = lambda line, end="\n": said.append(line)
    mgr.pio = "pio"                       # CI has no PlatformIO installed

    real_popen = dc.subprocess.Popen
    finished = threading.Event()
    result: dict = {}
    try:
        dc.subprocess.Popen = _NeverEndingPopen
        _NeverEndingPopen.last = None

        def worker():
            result["ok"] = mgr.run_steps([9])
            finished.set()

        threading.Thread(target=worker, daemon=True).start()

        # Wait for the monitor to actually be running before stopping it —
        # stopping something that has not started is a different test, below.
        deadline = time.time() + 5
        while _NeverEndingPopen.last is None and time.time() < deadline:
            time.sleep(0.01)
        check(_NeverEndingPopen.last is not None,
              "the monitor step starts a child that does not end on its own")
        check(not finished.wait(0.2),
              "and the run does not finish while it is up")

        mgr.cancel()
        check(finished.wait(5), "cancel() ends the run")
        check(_NeverEndingPopen.last.terminated.is_set(),
              "by terminating the child rather than abandoning it")
        check(not _NeverEndingPopen.last.killed,
              "and SIGTERM was enough — nothing had to be killed outright")
    finally:
        dc.subprocess.Popen = real_popen

    check(mgr.cancelled is True, "the manager says it was stopped")
    check(result.get("ok") is False,
          "and does not claim every step completed")
    log = "".join(said)
    check("Stopped" in log, f"the log says so: {log.strip().splitlines()[-1:]}")
    check("failed" not in log.lower(),
          "and does not call it a failure — somebody pressed the button")

    # ── A stop between two steps ────────────────────────────────────────────
    #
    # cancel() can land after one step's child has exited and before the next
    # one is launched. The flag is what carries it across that gap; without
    # the check at the top of the loop the next step started anyway.
    mgr2 = dc.DeployManager(app.cfg)
    mgr2.on_step_output = lambda line, end="\n": None
    mgr2.pio = "pio"
    _FakePopen.calls = []
    real_popen = dc.subprocess.Popen
    try:
        dc.subprocess.Popen = _FakePopen

        # Cancelled from the first step's own completion callback, which is
        # exactly the gap this is about: step 4's child has exited, step 5's
        # has not been launched. (Cancelling BEFORE run_steps is the next
        # case below, and pre-cancelling here would make this one test that
        # instead — step 4 would not run either.)
        def stop_after_first(step, rc):
            mgr2.cancel()
        mgr2.on_step_complete = stop_after_first
        mgr2.run_steps([4, 5, 6])
    finally:
        dc.subprocess.Popen = real_popen
    check(len(_FakePopen.calls) == 1,
          f"a stop during step 4 does not start steps 5 and 6 "
          f"({len(_FakePopen.calls)} command(s) ran)")
    check(mgr2.cancelled is True, "and the run is reported as stopped")

    # ── A stop pressed before the run has started ───────────────────────────
    #
    # The window is milliseconds wide — between the click on RUN and the
    # worker building its manager — and a button that quietly does nothing is
    # the bug being fixed, not a smaller version of it. run_steps() must
    # therefore NOT clear a flag it finds already set, which it used to.
    mgr3 = dc.DeployManager(app.cfg)
    mgr3.on_step_output = lambda line, end="\n": None
    mgr3.pio = "pio"
    _FakePopen.calls = []
    real_popen = dc.subprocess.Popen
    try:
        dc.subprocess.Popen = _FakePopen
        mgr3.cancel()
        early = mgr3.run_steps([4, 5, 6])
    finally:
        dc.subprocess.Popen = real_popen
    check(len(_FakePopen.calls) == 0,
          f"a stop before the run starts runs nothing at all "
          f"({len(_FakePopen.calls)} command(s) ran)")
    check(mgr3.cancelled is True and early is False,
          "and is still reported as stopped rather than as a clean finish")

    # ── The button itself ───────────────────────────────────────────────────
    #
    # root.update() after each click because _notify() reaches the status bar
    # through root.after(0, …) — without a turn of the event loop the label
    # still holds what it said before, and every check below would be reading
    # the previous frame.
    check(app.run_btn.cget("text") == "▶   RUN", "the button reads RUN when idle")

    app.running = True
    app.manager = None
    app._on_run_or_stop()
    app.root.update()
    check("stop requested" in app.status_label.cget("text").lower(),
          f"clicking STOP before the manager exists remembers the press "
          f"({app.status_label.cget('text')!r})")
    check(app._stop_requested is True,
          "and the worker will cancel the manager the moment it has one")
    app._stop_requested = False

    # A run in progress must not have its only stop taken away by an empty
    # step list — _refresh_plan() disables RUN when nothing is ticked, and
    # while running the button is not RUN.
    app.run_btn.configure(state="normal", text="■   STOP")
    app.cfg["steps"] = []
    app._refresh_plan()
    check(str(app.run_btn.cget("state")) == "normal",
          "unticking every step mid-run leaves STOP clickable")

    stopped: list = []
    app.manager = type("M", (), {"cancel": lambda self: stopped.append(True)})()
    app._on_run_or_stop()
    app.root.update()
    deadline = time.time() + 3
    while not stopped and time.time() < deadline:
        time.sleep(0.01)
    check(bool(stopped), "and clicking it while running cancels the manager")
    check("Stopping" in app.run_btn.cget("text"),
          f"the button says what it is doing while it does it "
          f"({app.run_btn.cget('text')!r})")

    # Back to a state the rest of the file can work from.
    app.running = False
    app.manager = None
    app.cfg["steps"] = [5, 6]
    app._refresh_plan()
    app.run_btn.configure(text="▶   RUN")
    check(str(app.run_btn.cget("state")) == "normal",
          "and once the run is over RUN comes back")
    print()


_UNANSWERED = object()


def _answer_in_window(app, ask, caption: str):
    """Ask from a worker thread, click the answer, return what the worker got.

    A real mainloop rather than update() calls, because that is the whole
    difference: _ask() reaches the window through root.after() from the
    thread running the steps, and tkinter refuses that unless the main thread
    is inside mainloop(). Everything below therefore stays on the thread it
    belongs to — the worker only calls the code under test, and the clicking
    and the quitting happen on the main thread as after() callbacks.
    """
    result = {}
    threading.Thread(target=lambda: result.setdefault("value", ask()),
                     daemon=True).start()

    def click() -> None:
        button = _find_button(app.answer_row, caption)
        if button is not None:
            button.invoke()
        else:
            app.root.after(20, click)

    def poll(deadline=time.time() + 10) -> None:
        # Leave the loop once the worker has its answer — or on the deadline,
        # so a question that never appears fails a check instead of hanging CI.
        if "value" in result or time.time() > deadline:
            app.root.quit()
        else:
            app.root.after(20, poll)

    app.root.after(20, click)
    app.root.after(20, poll)
    app.root.mainloop()
    app.root.update()
    return result.get("value", _UNANSWERED)


def run_prompts(app) -> None:
    """The two destructive steps ask HERE, and their scripts never prompt.

    tools/flash_bootloader.py asked its own "[y/N]" on stdin. A windowed
    build has no console and therefore no stdin, so the question the person
    was supposed to answer arrived as:

        EOFError: EOF when reading a line

    and step 2 failed every time it ran from this window. The question moved
    into the window — where the erase confirmation already was — and the
    script is told the answer with --yes.
    """
    print("The confirmations:")

    # ── The bootloader question is a control in this window ─────────────────
    answered = _answer_in_window(app, app._confirm_bootloader, "Flash bootloader")
    check(answered is True, "confirming the bootloader step answers True")
    check(_toplevels(app.root) == [], "and it asked without opening a window")

    answered = _answer_in_window(app, app._confirm_bootloader, "Skip this step")
    check(answered is False, "and declining it answers False")

    # ── What the step then runs ─────────────────────────────────────────────
    mgr = dc.DeployManager(app.cfg)
    said: list[str] = []
    mgr.on_step_output = lambda line, end="\n": said.append(line)
    real_popen = dc.subprocess.Popen
    _FakePopen.calls = []
    try:
        dc.subprocess.Popen = _FakePopen
        mgr.run_steps([2], confirm_bootloader_callback=lambda: True)
        ran = list(_FakePopen.calls)

        _FakePopen.calls = []
        said.clear()
        declined_ok = mgr.run_steps([2], confirm_bootloader_callback=lambda: False)
        skipped = list(_FakePopen.calls)
        declined_log = "".join(said)
    finally:
        dc.subprocess.Popen = real_popen

    check(len(ran) == 1, f"confirming runs one command ({len(ran)})")
    if ran:
        cmd, kwargs = ran[0]
        check("--yes" in cmd,
              "the step carries the answer to the script, so it cannot ask again")
        check(kwargs.get("stdin") is subprocess.DEVNULL,
              "and gives it no stdin to ask on")
    check(skipped == [], "declining runs nothing at all")

    # AND SAYS SO. "Nothing failed" and "everything ran" are different
    # answers, and run_steps() gives one bool for both — so a front end that
    # only looks at the bool prints its green "All steps completed
    # successfully" box for a deploy whose bootloader was never written.
    # Both of them used to.
    check(declined_ok is True, "declining is not a failure")
    check(mgr.skipped == [2],
          f"but the manager names the step that did not run ({mgr.skipped})")
    check("DECLINED" in declined_log,
          "and its own summary says so rather than claiming success")
    check("All steps completed successfully" not in declined_log,
          "  and does not also claim the opposite")

    # A manager that has never run anything can still be asked, and a step
    # called on its own — they are public methods — skips rather than raising.
    fresh = dc.DeployManager(app.cfg)
    fresh.on_step_output = lambda line, end="\n": None
    check(fresh.skipped == [], "a manager that has run nothing has skipped nothing")
    try:
        rc = fresh.s2_flash_bootloader(lambda: False)
        check(rc == 0 and fresh.skipped == [2],
              "and s2 called directly skips instead of raising AttributeError")
    except AttributeError as exc:
        check(False, f"s2 called directly raised: {exc}")

    # The CLI draws its own banner from the same answer. It is not callable
    # from here — it lives inside an interactive menu — so this is the one
    # thing to assert about it: that it asks.
    cli_src = (ROOT / "tools" / "deploy.py").read_text(encoding="utf-8")
    check("manager.skipped" in cli_src,
          "the CLI banner reads the same list the GUI banner does")

    # ── The script itself, when something asks it anyway ────────────────────
    real_input = builtins.input

    def _no_stdin(*_args):
        raise EOFError("EOF when reading a line")

    try:
        builtins.input = _no_stdin
        check(fb._confirm("Flash bootloader? [y/N] ") is False,
              "and run without a console it refuses instead of tracebacking")
    finally:
        builtins.input = real_input

    # ── Nothing pops a console window on Windows ────────────────────────────
    #
    # Each step is a console program (python.exe, pio.exe, esptool). Windows
    # gives one its own console window when the parent has none — which is
    # this window, built with console=False — so every step flashed up an
    # empty black rectangle that showed nothing: the output is on a pipe,
    # going into the log. Asserted here by asking the decision directly,
    # since CI runs this on Linux where the question does not arise.
    import win_console                                    # noqa: E402
    real_platform = sys.platform
    try:
        sys.platform = "win32"
        check(dc._no_window() == {"creationflags": win_console.CREATE_NO_WINDOW},
              "a piped step is launched with no console window")
        check(dc._no_window(inherits_console=True) == {},
              "the serial monitor keeps the console it was launched from")
        # The same decision reaches flash_bootloader.py, which used to carry
        # its own copy of these three functions.
        import flash_bootloader as fb2                    # noqa: E402
        check(fb2._no_window is dc._no_window,
              "and the bootloader script asks the same helper, not a copy")
    finally:
        sys.platform = real_platform


def run_web_filter(app) -> None:
    """Which copies of the pages reach the device, over BOTH routes.

    WHY THIS EXISTS
    ---------------
    The setting was applied in one place: step 8's HTTP upload loop, file by
    file. Step 7 runs `pio run -t uploadfs`, which images data/www/ exactly
    as it stands — so over USB the setting did nothing at all, and a tree with
    both copies of every page is about 970 KB of a 4 MB C3's 1088 KB LittleFS
    partition, before one row of CSV. Somebody choosing "compressed only" to
    make it fit got both copies anyway and a full filesystem.
    """
    print("Driving the web asset filter:")

    import build_web                                       # noqa: PLC0415

    def _build(dst: Path, mode: str) -> dict:
        """build_web.build(), without its per-file size report.

        Forty lines per call and six calls below is two hundred and forty
        lines of CI log saying what this test is not asking about.
        """
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            return build_web.build(ROOT / "www", dst, filter_mode=mode)

    sizes = {}
    with tempfile.TemporaryDirectory() as tmp:
        for mode in ("all", "gz", "plain"):
            dst = Path(tmp) / mode
            sizes[mode] = _build(dst, mode)["flash_bytes"]
            check(dc.www_matches_filter(mode, dst),
                  f"a tree built {mode!r} satisfies its own filter")
            for other in ("all", "gz", "plain"):
                if other == mode or other == "all":
                    continue
                check(not dc.www_matches_filter(other, dst),
                      f"and a {mode!r} tree is not mistaken for {other!r}")

        # THE NUMBER THIS IS ALL ABOUT.
        check(sizes["gz"] < sizes["all"] / 2,
              f"compressed-only is less than half the flash of both "
              f"({sizes['gz']} B vs {sizes['all']} B)")
        check(sizes["plain"] < sizes["all"],
              "and uncompressed-only is smaller than both, too")

        # Switching filter must REPLACE the tree, not add to it. Built in
        # place, one after the other, the way step 1 does on the second run.
        dst = Path(tmp) / "switch"
        _build(dst, "all")
        _build(dst, "gz")
        check(dc.www_matches_filter("gz", dst),
              "rebuilding all → gz drops the plain siblings it replaced")
        _build(dst, "all")
        check(len(list(dst.rglob("*"))) > 0 and dc.www_matches_filter("all", dst),
              "and gz → all puts them back")

        # No holes: a binary has no .gz to be kept instead of, and neither has
        # a text file whose gzip came out bigger. Dropping the plain copy of
        # either would be a tree missing a page rather than a smaller one.
        gz_tree = Path(tmp) / "gz"
        names = {p.name for p in gz_tree.rglob("*") if p.is_file()}
        check(any(n.endswith(".gz") for n in names), "the gz tree has .gz files")
        for p in (Path(tmp) / "all").rglob("*"):
            if not p.is_file() or p.suffix == ".gz":
                continue
            rel = p.relative_to(Path(tmp) / "all")
            served = (gz_tree / rel).exists() or (gz_tree / (str(rel) + ".gz")).exists()
            check(served, f"{rel} is still served by the gz tree")

    # ── And that both steps honour it ───────────────────────────────────────
    #
    # Step 1 by passing it to build_web.py, step 7 by noticing a tree that
    # does not match and rebuilding before it images anything.
    app.cfg["upload_filter"] = "gz"
    mgr = dc.DeployManager(app.cfg)
    mgr.on_step_output = lambda line, end="\n": None
    mgr.pio = "pio"
    _FakePopen.calls = []
    real_popen = dc.subprocess.Popen
    try:
        dc.subprocess.Popen = _FakePopen
        mgr.s1_build_web()
    finally:
        dc.subprocess.Popen = real_popen
    check(len(_FakePopen.calls) == 1, "step 1 runs one command")
    if _FakePopen.calls:
        cmd = _FakePopen.calls[0][0]
        check("--filter" in cmd and cmd[cmd.index("--filter") + 1] == "gz",
              f"and carries the filter to build_web.py ({cmd[-2:]})")

    src_core = (ROOT / "tools" / "deploy_core.py").read_text(encoding="utf-8")
    s7 = src_core.split("def s7_upload_fs")[1].split("def s8_")[0]
    check("www_matches_filter" in s7,
          "step 7 checks the tree against the filter before imaging it")
    check("s1_build_web" in s7,
          "  and rebuilds rather than flashing the wrong tree")
    app.cfg["upload_filter"] = "all"
    print()


def run(app) -> None:
    fv = app.feature_vars

    # START FROM NOTHING SELECTED.
    #
    # Without this the test inherits whatever .flash_tool.json happens to hold,
    # and the first version of this file did — which meant the "ticking ESP-NOW
    # also ticks FEATURE_REMOTE_NODES" check passed on a machine where
    # FEATURE_REMOTE_NODES was already selected, implication or no
    # implication. It was caught by deliberately breaking that implication and
    # watching the test stay green. A check that passes for the wrong reason is
    # worse than no check, because it gets believed.
    for var in fv.values():
        var.set(False)
    app.cfg["features"] = []
    app.cfg["espnow_lmk"] = ""
    check(len(fv) > 0, f"{len(fv)} feature checkboxes were built")
    check("FEATURE_ESPNOW_INGEST" in fv, "the ESP-NOW checkbox exists")
    if "FEATURE_ESPNOW_INGEST" not in fv:
        return

    # ── Every feature is tickable, the once-always-on ones included ─────────
    #
    # This check exists because somebody opened the window looking for BME280,
    # did not find it, and concluded the firmware had no driver for it — when
    # it was in every build ever produced. It had no checkbox because setup.h
    # defined it itself and no -D flag could remove it. FEATURE_SET_EXPLICIT
    # changed that, and this asserts the window caught up.
    from features import all_features         # noqa: PLC0415
    for macro in ("SENSOR_BME280_ENABLED", "SENSOR_SDS011_ENABLED",
                  "FEATURE_SD_STORAGE", "EXPORT_MQTT_ENABLED"):
        check(macro in fv, f"{macro} has a checkbox of its own")
    check(len(fv) == len(all_features()),
          f"all {len(all_features())} features are offered, none held back")

    # ── Selecting nothing must not take the build over ──────────────────────
    #
    # The factory config selects nothing, meaning "I have not chosen". This
    # shipped once with that meaning "I choose nothing": the tool passed
    # -DFEATURE_SET_EXPLICIT alone, setup.h suppressed every default, and the
    # #error fired on the first build anybody ran after updating. It went
    # unnoticed because this file only ever drove a NON-empty selection.
    app._features_clear()
    check(dc.DeployManager(app.cfg)._pio_env() is None,
          "an empty selection leaves the build to setup.h's defaults")

    # ── The set buttons ─────────────────────────────────────────────────────
    app._features_default()
    feats = set(app.cfg.get("features") or [])
    check("SENSOR_BME280_ENABLED" in feats and "FEATURE_SD_STORAGE" in feats,
          "Default set restores what a plain `pio run` gives")
    check("FEATURE_KINDLE_DASHBOARD" not in feats,
          "and does not quietly add an off-by-default one")

    # ── The rule that a build must be able to read something ────────────────
    #
    # setup.h refuses this with an #error. The window has to say so while
    # there is still something to click: a compiler diagnostic is a fine
    # backstop and a poor first contact.
    app._features_clear()
    painted = _all_label_text(app.root)
    check("Nothing to read from" in painted,
          "clearing everything warns that nothing can be measured")

    app._set_features(["SENSOR_BME280_ENABLED"])
    painted = _all_label_text(app.root)
    check("Nothing to read from" not in painted,
          "and one sensor clears the warning")

    # A collector that only aggregates other boards is a legitimate build, and
    # setup.h's #error accepts one — so the window must not refuse it either.
    app._set_features(["FEATURE_ESPNOW_INGEST"])
    painted = _all_label_text(app.root)
    check("Nothing to read from" not in painted,
          "a remote-node-only build is not treated as sensorless")

    # Put the boxes back where the rest of this file expects them.
    app._features_clear()

    # ── Ticking ESP-NOW must also tick what it implies ──────────────────────
    # setup.h defines FEATURE_REMOTE_NODES either way, so leaving the box
    # unticked would show a build different from the one that happens.
    fv["FEATURE_ESPNOW_INGEST"].set(True)
    app._on_feature_toggle("FEATURE_ESPNOW_INGEST")
    feats = app.cfg.get("features") or []
    check("FEATURE_ESPNOW_INGEST" in feats, "ESP-NOW reached the config")
    check("FEATURE_REMOTE_NODES" in feats, "the implied feature reached the config")
    check(fv["FEATURE_REMOTE_NODES"].get(), "the implied checkbox moved on screen")

    # ── The key field says what is wrong, before the build says it ──────────
    app.espnow_lmk_entry.delete(0, "end")
    app.espnow_lmk_entry.insert(0, "short")
    app._save_espnow_lmk()
    note = app.espnow_lmk_note.cget("text")
    check("16" in note, f"a 5-character key is reported: {note[:48]!r}")

    app.espnow_lmk_entry.delete(0, "end")
    app._save_espnow_lmk()
    note = app.espnow_lmk_note.cget("text")
    check("setup.h" in note, "an empty key warns about the placeholder default")

    app.espnow_lmk_entry.delete(0, "end")
    app.espnow_lmk_entry.insert(0, "sixteen-char-key")
    app._save_espnow_lmk()
    note = app.espnow_lmk_note.cget("text")
    check("node" in note, "a valid key points at the node it must match")

    # ── The key is generated, not invented ──────────────────────────────────
    #
    # A key somebody types is a key somebody can remember, and this one is the
    # only thing between the collector's pipeline and any ESP-NOW frame in
    # radio range. Both firmwares static_assert on exactly 16 characters, so a
    # generator that produced 15 would fail at the far end of a two-minute
    # build — asserted here instead.
    from deploy_core import NODE_PROJECTS, node_project   # noqa: PLC0415
    before = app.espnow_lmk_entry.get()
    app._generate_espnow_key()
    key = app.espnow_lmk_entry.get()
    check(len(key) == 16, f"a generated key is exactly 16 characters ({len(key)})")
    check(key != before, "and it is not the one that was already there")
    check(app.cfg.get("espnow_lmk") == key, "it reaches the config, not just the field")
    # The compiler gets it as -DESPNOW_LMK=\"...\" through a shell; a quote or
    # a backslash in it is a broken build.
    check(all(c.isalnum() for c in key),
          f"and holds nothing that would break a -D flag: {key!r}")

    # ── The node target ─────────────────────────────────────────────────────
    check(hasattr(app, "node_project_var"), "the node target has a control")
    app._on_node_project_change(NODE_PROJECTS["node"].label)
    check(app.cfg.get("node_project") == "node", "switching the node project sticks")
    check(not app.cfg.get("node_env"),
          "and clears the env, which belonged to the other chip")

    # _node_target(), not _node_cmd(): the target needs no PlatformIO to
    # answer, and this job does not install one. The first version asked for
    # the command, got [None, "run", …] back, and died in a str.join — a
    # traceback that named the join rather than the missing toolchain, which
    # is also why _node_cmd() now returns None outright.
    mgr = dc.DeployManager(app.cfg)
    target = mgr._node_target()
    check(target is not None, "the ESP8266 node resolves to a project")
    if target:
        directory, env_name = target
        check(directory.name == "node" and env_name == "nodemcuv2",
              f"and it is its own project and env: {directory.name}/{env_name}")
    # The ESP8266 node has no radio key, so it must not be handed one.
    check(mgr._node_env() is None, "and is not given an ESP-NOW key it cannot use")

    app._on_node_project_change(NODE_PROJECTS["node_espnow"].label)
    mgr = dc.DeployManager(app.cfg)
    nenv = mgr._node_env() or {}
    check(key in nenv.get("PLATFORMIO_BUILD_FLAGS", ""),
          "the battery node is built with the same key as the collector")

    # ── What the compiler would actually be given ───────────────────────────
    env = dc.DeployManager(app.cfg)._pio_env()
    flags = (env or {}).get("PLATFORMIO_BUILD_FLAGS", "")
    print(f"  ---  PLATFORMIO_BUILD_FLAGS = {flags}")
    check("-DFEATURE_ESPNOW_INGEST" in flags, "the feature reaches the build")
    check("-DFEATURE_REMOTE_NODES" in flags, "the implied feature reaches the build")
    # Against whatever key the config holds NOW, not a literal: the generator
    # check above replaced the typed one, and an assertion naming the old
    # string would fail on a page that is working perfectly.
    check(f'ESPNOW_LMK=\\"{app.cfg["espnow_lmk"]}\\"' in flags,
          "the key reaches the build")

    # ── And unticking takes it away again ───────────────────────────────────
    # FEATURE_REMOTE_NODES is deliberately LEFT: it is useful on its own, it
    # was not the box the person just clicked, and silently removing a feature
    # nobody unticked is the worse surprise of the two.
    fv["FEATURE_ESPNOW_INGEST"].set(False)
    app._on_feature_toggle("FEATURE_ESPNOW_INGEST")
    feats = app.cfg.get("features") or []
    check("FEATURE_ESPNOW_INGEST" not in feats, "unticking removes it")

    env = dc.DeployManager(app.cfg)._pio_env()
    flags = (env or {}).get("PLATFORMIO_BUILD_FLAGS", "")
    check("-DFEATURE_ESPNOW_INGEST" not in flags, "and it leaves the build too")


def main() -> int:
    cfg_file = dc.CFG_FILE
    backup = None
    if cfg_file.is_file():
        backup = Path(tempfile.mkdtemp()) / cfg_file.name
        shutil.copy2(cfg_file, backup)

    root = ctk.CTk()
    try:
        app = g.DeployerGUI(root)
        root.update()
        run_window(app)
        run_prompts(app)
        run_stop(app)
        run_web_filter(app)
        print("Driving the deploy GUI's build-feature widgets:")
        run(app)
    finally:
        try:
            root.destroy()
        except Exception:
            pass
        if backup is not None:
            shutil.copy2(backup, cfg_file)
        elif cfg_file.is_file():
            cfg_file.unlink()

    print()
    if FAILURES:
        print(f"FAIL: {len(FAILURES)} check(s) failed")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("OK: the build-feature widgets do what they say")
    return 0


if __name__ == "__main__":
    sys.exit(main())
