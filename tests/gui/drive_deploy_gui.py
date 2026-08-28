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

import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import customtkinter as ctk        # noqa: E402
import deploy_core as dc           # noqa: E402
import deploy_gui as g             # noqa: E402

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
