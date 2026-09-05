#!/usr/bin/env python3
"""
tools/deploy_gui.py — Desktop front end for the ESP32 Logger deploy tools.

CustomTkinter window over deploy_core.py, so every step it runs is the same
code path deploy.py (the CLI) runs. Three rules shape what is here:

  ONE WINDOW.
    No message boxes, no modal dialogs, no input pop-ups. Everything the tool
    has to SAY appears in the status bar along the bottom or in the log;
    everything it has to ASK — the erase and bootloader confirmations, WiFi
    credentials, which of five serial ports — is a control in the window
    itself. A tkinter dialog
    that opens behind its parent (which is what happens under several Linux
    window managers, and on Windows when the app is not focused) is
    indistinguishable from a hung program, and the erase confirmation was
    exactly that: a build stopped dead on a question nobody could see.

  READABLE AT REST.
    Nothing is smaller than 15 px, against 8–10 pt before — the small print
    was the part carrying the warnings. The whole interface also scales from
    the header (A− / A+, or Ctrl+- / Ctrl++), and the scale is remembered per
    person in .flash_tool.json.

  A SHORT PATH THAT DOES NOT REMOVE THE LONG ONE.
    Board → port → job → Run is the whole common route, in that order down the
    left side, in the words of somebody who has not memorised the step
    numbers. Every switch that used to be here is still here — individual
    steps under "Customise", the rest one tab to the right.
"""

import sys
import threading
import tkinter as tk
from pathlib import Path
from typing import Optional

# Try to import customtkinter
try:
    import customtkinter as ctk
except ImportError:
    print("Error: customtkinter is not installed.")
    print("Install it with: pip install customtkinter")
    sys.exit(1)

# Gracefully handle missing pyserial
try:
    import serial.tools.list_ports  # noqa: F401
    HAS_PYSERIAL = True
except ImportError:
    HAS_PYSERIAL = False

# Add tools dir to path for imports
TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from deploy_core import (
    DeployManager,
    NODE_PROJECTS,
    PRESET_BLURBS,
    generate_espnow_key,
    node_project,
    env_defaults_note,
    load_cfg,
    save_cfg,
    step_parts,
    STEP_NAMES,
    PRESETS,
    detect_env,
    _UPLOAD_FILTERS,
    _UPLOAD_FILTER_LABELS,
)
from features import (default_on_features, grouped,
                      has_a_reading_source, is_known, optional_features)
from pio_envs import (
    INI, NO_PROJECT, chip_for, defaults_for, env_info, environments, env_names,
    ports_for, usb_pins,
)

# ── Theme configuration ─────────────────────────────────────────────────────
ctk.set_default_color_theme("blue")

#: Text colours as (light mode, dark mode) pairs. CustomTkinter picks the half
#: that matches the current appearance mode, which is the only reason the light
#: theme is usable at all — a fixed "#ffd93d" warning is invisible on white,
#: and the warnings are the lines that most need reading.
COLORS = {
    "normal":  ("gray10", "gray90"),
    "muted":   ("gray38", "gray68"),
    "info":    ("#1864ab", "#74c0fc"),
    "success": ("#2b8a3e", "#69db7c"),
    "warning": ("#b26a00", "#ffd43b"),
    "error":   ("#c92a2a", "#ff8787"),
}

#: Log tag colours, per appearance mode. tk.Text tags take one colour, not a
#: CustomTkinter pair, so these are re-applied when the theme changes.
LOG_COLORS = {
    "dark": {
        "error": "#ff8787", "success": "#69db7c",
        "warning": "#ffd43b", "info": "#c5c5c5",
    },
    "light": {
        "error": "#c92a2a", "success": "#2b8a3e",
        "warning": "#b26a00", "info": "#3b3b3b",
    },
}

#: The port picker's "let deploy_core work it out" entry.
AUTO_PORT = "Auto-detect"

# Tab titles, named once because switching to a tab needs the exact string.
TAB_RUN, TAB_SETTINGS, TAB_BUILD = "▶  Run", "⚙  Settings", "🧩  Build"
TAB_WIFI, TAB_HELP = "📡  WiFi", "?  Help"

SCALE_MIN, SCALE_MAX, SCALE_STEP = 0.85, 1.80, 0.10

# A monospace family that exists on the platform. Falling through to whatever
# tkinter substitutes for "Courier" gave a bitmap font on Linux that looked
# broken next to everything else in the window.
_MONO = {"win32": "Consolas", "darwin": "Menlo"}.get(sys.platform, "DejaVu Sans Mono")


class Fonts:
    """Every font in the window, in one place, resizable while it runs.

    CTkFont sizes are in PIXELS, not points — which is how the old window
    ended up as small as it was: ("Helvetica", 8) is a point size, and the
    labels carrying the ESP-NOW key warning and the USB CDC pin note were set
    in it. The numbers below are pixels and start where 10–14 pt lands.

    Widgets hold a reference to the font object, so re-configuring one here
    re-renders every widget using it. Widget HEIGHTS do not follow along, so
    DeployerGUI.px() scales those separately.
    """

    _ROLES = {                      # role: (px, weight, monospace)
        "title":  (26, "bold",   False),
        "h1":     (20, "bold",   False),
        "h2":     (17, "bold",   False),
        "body":   (16, "normal", False),
        "bodyb":  (16, "bold",   False),
        "small":  (15, "normal", False),
        "button": (16, "bold",   False),
        "mono":   (15, "normal", True),
    }

    def __init__(self, scale: float = 1.0) -> None:
        self.scale = clamp_scale(scale)
        self._fonts = {
            role: ctk.CTkFont(family=_MONO if mono else None,
                              size=self._px(px), weight=weight)
            for role, (px, weight, mono) in self._ROLES.items()
        }

    def _px(self, base: int) -> int:
        return max(9, int(round(base * self.scale)))

    def __getitem__(self, role: str) -> ctk.CTkFont:
        return self._fonts[role]

    def set_scale(self, scale: float) -> None:
        self.scale = clamp_scale(scale)
        for role, (px, _w, _m) in self._ROLES.items():
            self._fonts[role].configure(size=self._px(px))


def clamp_scale(scale: float) -> float:
    try:
        scale = float(scale)
    except (TypeError, ValueError):
        scale = 1.0
    return max(SCALE_MIN, min(SCALE_MAX, round(scale, 2)))


class Balloon:
    """The hint that appears under the pointer, and the window it is NOT.

    Every per-item explanation in this window used to be a grey line printed
    under the thing it explained: a sentence under each of six preset buttons,
    under each of twelve steps, under each of thirty features. All of it true,
    all of it read once, and together it roughly doubled the height of three
    panels — the step list and the feature list were twice as long as the
    number of things in them.

    So they hover now. The important part is what this is made of: **one
    CTkLabel placed over the main window**, not a `Toplevel`. A tooltip window
    is the same object as the dialogs this tool deliberately does not use — an
    override-redirect child that the window manager places, can show behind
    its parent, and on some systems steals focus from it. A placed label
    cannot: it is inside the window, it is clipped by it, it takes no focus,
    and it disappears the moment the pointer leaves.

    The cost is that a hint cannot extend past the window edge, so `_show()`
    clamps it and flips it above the widget when there is no room below.
    """

    SHOW_DELAY_MS = 350
    #: Hiding is delayed too, and that is not a nicety. CustomTkinter forwards
    #: bind() to the internal canvas AND the internal text label, so moving the
    #: pointer across one button emits Leave/Enter pairs. Hiding immediately
    #: made the hint flicker on every crossing.
    HIDE_DELAY_MS = 120

    def __init__(self, app: "DeployerGUI") -> None:
        self.app = app
        self.root = app.root
        self._show_job = None
        self._hide_job = None
        # A frame around the label purely for the border: CTkLabel has no
        # border of its own, and without one the hint dissolved into whatever
        # card it happened to float over — it has to read as being ON TOP of
        # the window, not part of it.
        self._frame = ctk.CTkFrame(
            self.root, corner_radius=6, border_width=1,
            fg_color=("#ffffff", "#15181c"),
            border_color=("#b9bfc9", "#565b63"))
        self._label = ctk.CTkLabel(
            self._frame, text="", font=app.fonts["small"],
            justify="left", anchor="w",
            text_color=("gray10", "gray90"),
            wraplength=app.px(330))
        self._label.pack(padx=11, pady=8)
        # Scrolling moves the widget out from under a hint that is already up.
        for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            self.root.bind(seq, lambda _e: self.hide(), add="+")

    # ── attaching ───────────────────────────────────────────────────────────
    def attach(self, widget, text) -> None:
        """Give `widget` a hint. `text` may be a string or a callable.

        A callable is re-read on every hover, which is what the board and port
        lines need: their hint is the detail their one visible line dropped,
        and that detail changes with the selected environment.
        """
        if not text:
            return
        widget.bind("<Enter>", lambda _e: self._enter(widget, text), add="+")
        widget.bind("<Leave>", lambda _e: self._leave(), add="+")
        # A click means the pointer is busy doing something else.
        widget.bind("<Button-1>", lambda _e: self.hide(), add="+")

    # ── scheduling ──────────────────────────────────────────────────────────
    def _enter(self, widget, text) -> None:
        self._cancel(self._hide_job)
        self._hide_job = None
        self._cancel(self._show_job)
        self._show_job = self.root.after(
            self.SHOW_DELAY_MS, lambda: self._show(widget, text))

    def _leave(self) -> None:
        self._cancel(self._show_job)
        self._show_job = None
        self._cancel(self._hide_job)
        self._hide_job = self.root.after(self.HIDE_DELAY_MS, self.hide)

    def _cancel(self, job) -> None:
        if job is not None:
            try:
                self.root.after_cancel(job)
            except Exception:
                pass

    # ── painting ────────────────────────────────────────────────────────────
    def _show(self, widget, text) -> None:
        self._show_job = None
        body = text() if callable(text) else text
        if not body:
            return
        try:
            if not widget.winfo_ismapped():
                return                      # its tab or panel went away
            self._label.configure(text=body)
            self._frame.update_idletasks()
            w, h = self._frame.winfo_reqwidth(), self._frame.winfo_reqheight()
            rx, ry = self.root.winfo_rootx(), self.root.winfo_rooty()
            x = widget.winfo_rootx() - rx
            below = widget.winfo_rooty() - ry + widget.winfo_height() + 4
            x = max(6, min(x, self.root.winfo_width() - w - 6))
            y = below
            if y + h > self.root.winfo_height() - 6:
                y = max(6, widget.winfo_rooty() - ry - h - 4)
            self._frame.place(x=x, y=y)
            self._frame.lift()
        except Exception:
            self.hide()

    def hide(self) -> None:
        self._cancel(self._show_job)
        self._show_job = None
        try:
            self._frame.place_forget()
        except Exception:
            pass

    @property
    def visible(self) -> bool:
        return bool(self._frame.winfo_manager())

    @property
    def text(self) -> str:
        return self._label.cget("text")


class DeployerGUI:
    def __init__(self, root: ctk.CTk):
        self.root = root

        # Load config
        self.cfg = load_cfg()
        self.manager: Optional[DeployManager] = None
        self.running = False
        self.step_vars: dict[int, tk.BooleanVar] = {}
        self.feature_vars: dict[str, ctk.BooleanVar] = {}

        # Widgets whose HEIGHT (not just font) follows the interface scale,
        # as (widget, base height) pairs — see px() and _apply_scale().
        self._sized: list[tuple] = []
        # Answer events waiting on a person; released if the window closes, so
        # a background step never waits on a question that can no longer be
        # answered.
        self._pending: list[threading.Event] = []
        self._notice_job = None
        self._progress = (0, 0)

        ctk.set_appearance_mode(self.cfg.get("ui_theme") or "Dark")
        self.fonts = Fonts(self.cfg.get("ui_scale") or 1.0)

        self.root.title("ESP32 Logger — Deploy & Flash Tool")
        self.root.geometry("1180x820")
        self.root.minsize(940, 640)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        # Setup UI
        self._build_ui()
        self.root.after(150, self._startup_report)

    # ── scale helpers ───────────────────────────────────────────────────────
    def px(self, base: int) -> int:
        """A pixel dimension at the current interface scale."""
        return int(round(base * self.fonts.scale))

    def _sz(self, widget, base_height: int):
        """Register a widget whose height must track the interface scale."""
        self._sized.append((widget, base_height))
        widget.configure(height=self.px(base_height))
        return widget

    def _bump_scale(self, delta: float) -> None:
        self._apply_scale(self.fonts.scale + delta)

    def _apply_scale(self, scale: float) -> None:
        scale = clamp_scale(scale)
        if scale == self.fonts.scale and self.cfg.get("ui_scale") == scale:
            return
        self.fonts.set_scale(scale)
        alive = []
        for widget, base in self._sized:
            try:
                widget.configure(height=self.px(base))
                alive.append((widget, base))
            except Exception:
                pass          # destroyed with its tab, or a transient button
        self._sized = alive
        try:
            self.sidebar.configure(width=self.px(360))
        except Exception:
            pass
        self.scale_label.configure(text=f"{int(self.fonts.scale * 100)}%")
        self._save_setting("ui_scale", None, self.fonts.scale)
        self._notify(f"Interface scale {int(self.fonts.scale * 100)}%.",
                     "info", seconds=4)

    def _on_theme_change(self, choice: str) -> None:
        ctk.set_appearance_mode(choice)
        self._save_setting("ui_theme", None, choice)
        self._apply_log_colors()

    # ── startup ─────────────────────────────────────────────────────────────
    def _startup_report(self) -> None:
        """Say what this copy can see — in the window, not in a dialog.

        Every board list is empty when there is no platformio.ini, and without
        being told, the tool opens with a blank environment dropdown and no
        explanation: it reads as a broken download rather than a tool started
        in the wrong place, which is the most likely first experience of the
        pre-built binary. This used to be a modal warning; it is now the
        status bar plus the log, so it cannot open behind the window.
        """
        if NO_PROJECT:
            self._notify(
                "No platformio.ini found — there are no boards to offer. "
                "See the log for where to put this program.", "error")
            self._log("No PlatformIO project found\n", "error")
            self._log(
                "  Could not find a platformio.ini, so there are no boards to\n"
                "  offer and nothing can be built.\n\n"
                "  Put this program inside your ESP32_Logger checkout (any\n"
                "  subdirectory will do), or start it from that directory,\n"
                "  then reopen it.\n\n"
                f"  Looked upward from:\n    {Path.cwd()}\n"
                f"    {Path(sys.executable).parent}\n")
            return

        self._log(f"Project   {INI.parent}", "info")
        try:
            for line in env_defaults_note(self.cfg):
                self._log(f"  {line}", "info")
        except Exception as exc:           # a half-written ini, a missing board
            self._log(f"  could not summarise the environment: {exc}", "warning")
        if not HAS_PYSERIAL:
            self._log("\n  pyserial is not installed — serial ports cannot be "
                      "listed. Type the port by hand, or: pip install pyserial",
                      "warning")
        self._log("")
        self._notify("Ready. Pick a board, a port and a job, then press Run.",
                     "info")

    # ── layout ──────────────────────────────────────────────────────────────
    def _hint(self, widget, text) -> None:
        """Attach a hover hint. See Balloon — it is a label, not a window."""
        self.balloon.attach(widget, text)

    def _build_ui(self) -> None:
        """Header, status bar, then sidebar + tabs in what is left."""
        self.balloon = Balloon(self)
        self._build_header()
        # Packed before the body so a long status message can never push the
        # bar off the bottom of the window.
        self._build_status_bar()

        body = ctk.CTkFrame(self.root, fg_color="transparent")
        body.pack(side="top", fill="both", expand=True, padx=12, pady=(8, 8))
        body.grid_columnconfigure(1, weight=1)
        body.grid_rowconfigure(0, weight=1)

        self._build_sidebar(body)
        self._build_tabs(body)
        self._refresh_plan()

    def _build_header(self) -> None:
        header = ctk.CTkFrame(self.root, corner_radius=0)
        header.pack(side="top", fill="x")

        ctk.CTkLabel(header, text="ESP32 Logger — Deploy & Flash",
                     font=self.fonts["title"]).pack(side="left", padx=20, pady=14)

        # Readability controls, in the header rather than buried in a settings
        # tab: somebody who cannot read the window cannot go looking for them.
        right = ctk.CTkFrame(header, fg_color="transparent")
        right.pack(side="right", padx=16)

        ctk.CTkLabel(right, text="Text size", font=self.fonts["small"],
                     text_color=COLORS["muted"]).pack(side="left", padx=(0, 8))
        self._sz(ctk.CTkButton(right, text="A −", width=self.px(46),
                               font=self.fonts["bodyb"],
                               command=lambda: self._bump_scale(-SCALE_STEP)),
                 34).pack(side="left", padx=2)
        self.scale_label = ctk.CTkLabel(
            right, text=f"{int(self.fonts.scale * 100)}%",
            font=self.fonts["small"], width=self.px(52))
        self.scale_label.pack(side="left")
        self._sz(ctk.CTkButton(right, text="A +", width=self.px(46),
                               font=self.fonts["bodyb"],
                               command=lambda: self._bump_scale(SCALE_STEP)),
                 34).pack(side="left", padx=2)

        self.theme_menu = self._sz(ctk.CTkOptionMenu(
            right, values=["Dark", "Light", "System"],
            width=self.px(110), font=self.fonts["small"],
            command=self._on_theme_change), 34)
        self.theme_menu.set(self.cfg.get("ui_theme") or "Dark")
        self.theme_menu.pack(side="left", padx=(14, 0))

    def _build_status_bar(self) -> None:
        """The window's only channel for messages and yes/no questions.

        Everything that used to be a messagebox lands here: warnings, results,
        and the erase confirmation, which grows two buttons on the right and
        blocks the worker thread until one is pressed.
        """
        bar = ctk.CTkFrame(self.root, corner_radius=0)
        bar.pack(side="bottom", fill="x")

        # width/height 1, not the CTkFrame default of 200x200: an EMPTY frame
        # falls back to its configured size, and a 200 px tall invisible frame
        # in the status bar pushed a 200 px band of dead space up the window.
        # With buttons in it, geometry propagation sizes it to them.
        self.answer_row = ctk.CTkFrame(bar, fg_color="transparent",
                                       width=1, height=1)
        self.answer_row.pack(side="right", padx=12, pady=8)

        self.status_label = ctk.CTkLabel(
            bar, text="Starting…", font=self.fonts["body"],
            text_color=COLORS["muted"], anchor="w", justify="left",
            wraplength=900)
        self.status_label.pack(side="left", fill="x", expand=True,
                               padx=16, pady=8)
        # Wrap to whatever width the window actually has, so a long message
        # stays inside the bar instead of stretching the window.
        bar.bind("<Configure>",
                 lambda e: self.status_label.configure(
                     wraplength=max(320, e.width - self.px(260))))

    # ── sidebar: the short path ─────────────────────────────────────────────
    def _build_sidebar(self, parent: ctk.CTkFrame) -> None:
        """Board, port and job scroll. RUN does not.

        The action button used to be the last thing in a scrolling column, so
        on a 768-pixel laptop screen the main control of the whole window sat
        below the fold with nothing on screen saying so. It is pinned to the
        bottom of the column now, with what it is about to do written under it.
        """
        column = ctk.CTkFrame(parent, fg_color="transparent")
        column.grid(row=0, column=0, sticky="nsew", padx=(0, 12))
        column.grid_rowconfigure(0, weight=1)
        column.grid_columnconfigure(0, weight=1)

        self.sidebar = ctk.CTkScrollableFrame(column, width=self.px(360))
        self.sidebar.grid(row=0, column=0, sticky="nsew")

        pinned = ctk.CTkFrame(column, fg_color="transparent")
        pinned.grid(row=1, column=0, sticky="ew", pady=(10, 0))

        self._build_board_card(self.sidebar)
        self._build_port_card(self.sidebar)
        self._build_job_card(self.sidebar)
        self._build_run_card(pinned)

    def _card(self, parent, title: str, hint: str = "") -> ctk.CTkFrame:
        """A titled block in the sidebar. Returns the frame to fill.

        An explanation goes on the title as a hover hint rather than under it
        as a paragraph — see Balloon. The ⓘ is there so the hint can be found
        by someone who is not already hovering it.
        """
        card = ctk.CTkFrame(parent)
        card.pack(fill="x", padx=4, pady=(0, 12))
        head = ctk.CTkLabel(card, text=f"{title}  ⓘ" if hint else title,
                            font=self.fonts["h2"], anchor="w", justify="left")
        head.pack(fill="x", padx=14, pady=(12, 0))
        self._hint(head, hint)
        inner = ctk.CTkFrame(card, fg_color="transparent")
        inner.pack(fill="x", padx=14, pady=(8, 14))
        return inner

    def _build_board_card(self, parent) -> None:
        """Which board. A list read from platformio.ini, never typed.

        Free text let a nonexistent env through to pio, which failed minutes
        later with a message that did not say what was wrong.
        """
        inner = self._card(
            parent, "1 · Board",
            "The environments in platformio.ini. Everything else — chip, "
            "upload speed, partition table, the USB IDs the port scan matches "
            "on — follows from this one choice, so nothing else needs typing.")

        names = env_names() or [detect_env()]
        current = self.cfg.get("env") or detect_env()
        if current not in names:
            names = [current] + names       # keep an unknown saved env visible
        self.env_var = ctk.StringVar(value=current)
        self._sz(ctk.CTkOptionMenu(inner, values=names, variable=self.env_var,
                                   font=self.fonts["body"],
                                   dropdown_font=self.fonts["body"],
                                   command=self._on_env_change), 38
                 ).pack(fill="x")

        # What that env actually is. The chip is derived from it, never typed:
        # an env and a chip that could disagree was a way to write a C3
        # bootloader onto an S3.
        self.board_label = ctk.CTkLabel(
            inner, text="", font=self.fonts["small"],
            text_color=COLORS["muted"], wraplength=self.px(300),
            anchor="w", justify="left")
        self.board_label.pack(fill="x", pady=(6, 0))
        # One line on screen — board and chip — with flash size, partition
        # table and where the speeds come from behind it.
        self._hint(self.board_label, self._board_detail)

    def _build_port_card(self, parent) -> None:
        """Which USB port, as a list you pick from.

        This was a pop-up that printed the ranked ports as text and asked you
        to TYPE one of them back in. The list is now the control itself, and it
        is still editable, so a port no scan can see (a network bridge, a
        symlink) can be typed as before.
        """
        inner = self._card(
            parent, "2 · Port",
            "Ranked by the selected board's own USB VID:PID, best match "
            "first (★). Non-USB ports (COM1, /dev/ttyS0) are never listed — "
            "no board sits behind one. The field is editable, for a port no "
            "scan can see.")

        self.port_box = self._sz(ctk.CTkComboBox(
            inner, values=[AUTO_PORT], font=self.fonts["body"],
            dropdown_font=self.fonts["body"], command=self._on_port_pick), 38)
        self.port_box.pack(fill="x")
        self.port_box.bind("<FocusOut>", lambda _: self._save_port())
        self.port_box.bind("<Return>", lambda _: self._save_port())

        row = ctk.CTkFrame(inner, fg_color="transparent")
        row.pack(fill="x", pady=(6, 0))
        self._sz(ctk.CTkButton(row, text="🔄  Rescan ports  (F5)",
                               font=self.fonts["small"],
                               command=self._refresh_ports), 34
                 ).pack(side="left", fill="x", expand=True)

        self.port_note = ctk.CTkLabel(
            inner, text="", font=self.fonts["small"],
            text_color=COLORS["muted"], wraplength=self.px(300),
            anchor="w", justify="left")
        self.port_note.pack(fill="x", pady=(6, 0))
        self._port_note_detail = ""
        self._hint(self.port_note, lambda: self._port_note_detail)
        self._refresh_ports(announce=False)

    def _build_job_card(self, parent) -> None:
        """The presets, as the primary control, with what each one is FOR.

        A preset used to be a bare button captioned "Full flash" next to
        eleven checkboxes of equal weight, which left the actual question —
        which of these do I want — answerable only by someone who already knew
        the step catalogue. The individual steps are still one click away, and
        ticking them still works exactly as it did.
        """
        inner = self._card(
            parent, "3 · What do you want to do?",
            "Each button ticks a set of steps. Hover one to see what it is "
            "for; unfold Customise steps to tick them yourself.")

        # Two columns. One button per row was the only sensible arrangement
        # while each carried a two-line caption; without them the captions are
        # what set the width, and six full-width buttons is a column you
        # scroll to reach the last two.
        grid = ctk.CTkFrame(inner, fg_color="transparent")
        grid.pack(fill="x")
        grid.grid_columnconfigure((0, 1), weight=1, uniform="preset")

        for i, (key, (pname, psteps)) in enumerate(
                (k, v) for k, v in PRESETS.items() if v[1] or k != "N"):
            steps_s = ", ".join(str(n) for n in psteps)
            button = self._sz(ctk.CTkButton(
                grid, text=pname, font=self.fonts["button"],
                command=lambda s=psteps: self._apply_preset(s)), 40)
            button.grid(row=i // 2, column=i % 2, sticky="ew",
                        padx=(0, 4) if i % 2 == 0 else (4, 0), pady=3)
            # What it is for, and which steps that means — the second half was
            # never on screen at all, and is the question the first half raises.
            self._hint(button, f"{PRESET_BLURBS.get(key, '')}\n\n"
                               f"Steps {steps_s}:\n" +
                       "\n".join(f"  {n}.  {step_parts(n)[0]}" for n in psteps))

        # ── Individual steps, folded away rather than removed ───────────────
        self._steps_open = bool(self.cfg.get("steps_panel_open", False))
        self.steps_toggle = self._sz(ctk.CTkButton(
            inner, text="", font=self.fonts["small"], fg_color="transparent",
            border_width=1, anchor="w", command=self._toggle_steps), 34)
        self.steps_toggle.pack(fill="x", pady=(4, 0))

        self.steps_box = ctk.CTkFrame(inner, fg_color="transparent")
        self._build_steps(self.steps_box)
        self._sync_steps_panel()

    def _build_steps(self, parent) -> None:
        """One checkbox per step, grouped by which board it touches.

        The name is split into what the step does and how it does it — the
        second half used to be truncated at 28 characters, which cut
        "pio run -d node… -t upload" down to something that no longer said
        which project it built.
        """
        enabled_steps = set(self.cfg.get("steps", []))
        groups = (("Collector board", range(1, 10)),
                  ("Node board (its own USB port)", range(10, 13)))

        for heading, numbers in groups:
            ctk.CTkLabel(parent, text=heading, font=self.fonts["bodyb"],
                         anchor="w").pack(fill="x", pady=(10, 2))
            for n in numbers:
                if n not in STEP_NAMES:
                    continue
                title, detail = step_parts(n)
                var = tk.BooleanVar(value=n in enabled_steps)
                self.step_vars[n] = var
                check = ctk.CTkCheckBox(
                    parent, text=f"{n}.  {title}", variable=var,
                    font=self.fonts["body"], checkbox_width=self.px(22),
                    checkbox_height=self.px(22),
                    command=self._update_steps)
                check.pack(anchor="w", pady=(3, 0))
                # The command it runs, on hover. Printed under every step, it
                # made a twelve-item list twenty-four lines long.
                self._hint(check, f"Step {n} · {title}\n{detail}" if detail
                                  else f"Step {n} · {title}")

        row = ctk.CTkFrame(parent, fg_color="transparent")
        row.pack(fill="x", pady=(10, 4))
        self._sz(ctk.CTkButton(row, text="Clear all steps",
                               font=self.fonts["small"], fg_color="gray40",
                               command=lambda: self._apply_preset([])), 32
                 ).pack(side="left")

    def _toggle_steps(self) -> None:
        self._steps_open = not self._steps_open
        self._save_setting("steps_panel_open", None, self._steps_open)
        self._sync_steps_panel()

    def _scroll_sidebar(self, fraction: float) -> None:
        """Move the sidebar viewport. Private attribute because
        CTkScrollableFrame exposes no way to move its own."""
        def scroll():
            try:
                self.sidebar._parent_canvas.yview_moveto(fraction)
            except Exception:
                pass
        self.root.after(60, scroll)

    def _sync_steps_panel(self) -> None:
        if self._steps_open:
            self.steps_box.pack(fill="x")
            self.steps_toggle.configure(text="▾  Customise steps")
            # Unfolding a panel that opens below the fold looks like a button
            # that does nothing, so scroll down to it.
            self._scroll_sidebar(1.0)
        else:
            self.steps_box.pack_forget()
            self.steps_toggle.configure(text="▸  Customise steps")
            # And folding it back must return the viewport: the canvas keeps
            # the old offset when its content shrinks, which left the whole
            # sidebar scrolled past its own end and looking empty.
            self._scroll_sidebar(0.0)

    def _build_run_card(self, parent) -> None:
        inner = self._card(parent, "4 · Run")

        self.run_btn = self._sz(ctk.CTkButton(
            inner, text="▶   RUN", font=self.fonts["h2"],
            command=self._on_run), 52)
        self.run_btn.pack(fill="x")

        # What "Run" is about to do, spelled out. The old window said nothing
        # until the log started scrolling, so a preset clicked by accident was
        # discovered by watching a chip erase.
        self.plan_label = ctk.CTkLabel(
            inner, text="", font=self.fonts["small"],
            text_color=COLORS["muted"], anchor="w", justify="left",
            wraplength=self.px(300))
        self.plan_label.pack(fill="x", pady=(8, 0))

        row = ctk.CTkFrame(inner, fg_color="transparent")
        row.pack(fill="x", pady=(10, 0))
        self._sz(ctk.CTkButton(row, text="💾  Save", font=self.fonts["small"],
                               command=self._save_config), 34
                 ).pack(side="left", fill="x", expand=True, padx=(0, 4))
        self._sz(ctk.CTkButton(row, text="📡  WiFi", font=self.fonts["small"],
                               command=self._on_wifi), 34
                 ).pack(side="left", fill="x", expand=True, padx=(4, 0))

    # ── tabs: everything that is not the short path ─────────────────────────
    def _build_tabs(self, parent: ctk.CTkFrame) -> None:
        self.tab_view = ctk.CTkTabview(parent)
        self.tab_view.grid(row=0, column=1, sticky="nsew")
        try:
            # No public setter for the tab strip's font, and 15 px tabs above
            # 16 px content is exactly the mismatch this rewrite is about.
            self.tab_view._segmented_button.configure(font=self.fonts["bodyb"])
        except Exception:
            pass

        self._build_run_tab()
        self._build_settings_tab()
        self._build_build_tab()
        self._build_wifi_tab()
        self._build_help_tab()
        self.tab_view.set(TAB_RUN)

    def _build_run_tab(self) -> None:
        """Progress and output."""
        tab = self.tab_view.add(TAB_RUN)
        tab.grid_rowconfigure(2, weight=1)
        tab.grid_columnconfigure(0, weight=1)

        head = ctk.CTkFrame(tab, fg_color="transparent")
        head.grid(row=0, column=0, sticky="ew", padx=8, pady=(8, 0))
        self.progress_label = ctk.CTkLabel(
            head, text="Idle", font=self.fonts["bodyb"], anchor="w")
        self.progress_label.pack(side="left")
        self._sz(ctk.CTkButton(head, text="Copy log", font=self.fonts["small"],
                               width=self.px(90), command=self._copy_log), 32
                 ).pack(side="right", padx=(6, 0))
        self._sz(ctk.CTkButton(head, text="Clear", font=self.fonts["small"],
                               width=self.px(80), fg_color="gray40",
                               command=self._clear_logs), 32).pack(side="right")

        self.progress = ctk.CTkProgressBar(tab, height=self.px(10))
        self.progress.grid(row=1, column=0, sticky="ew", padx=8, pady=(6, 6))
        self.progress.set(0)

        self.log_text = ctk.CTkTextbox(tab, font=self.fonts["mono"],
                                       wrap="word")
        self.log_text.grid(row=2, column=0, sticky="nsew", padx=8, pady=(0, 8))
        self._apply_log_colors()

    def _apply_log_colors(self) -> None:
        """(Re-)paint the log tags for the current appearance mode.

        CTkTextbox.tag_config delegates to tk.Text, which takes 'foreground'
        and one colour — not a CustomTkinter (light, dark) pair — so the
        palette is chosen here and re-applied whenever the theme changes.
        """
        mode = ctk.get_appearance_mode().lower()
        for tag, color in LOG_COLORS.get(mode, LOG_COLORS["dark"]).items():
            self.log_text.tag_config(tag, foreground=color)

    def _build_settings_tab(self) -> None:
        """Everything the short path does not ask about, still all here."""
        tab = self.tab_view.add(TAB_SETTINGS)
        frame = ctk.CTkScrollableFrame(tab)
        frame.pack(fill="both", expand=True, padx=8, pady=8)

        # ── Network ─────────────────────────────────────────────────────────
        box = self._settings_group(
            frame, "Device address",
            "Where step 8 pushes the web UI over HTTP. The access-point "
            "default is 192.168.4.1; on your own network use the address the "
            "device reports on the serial monitor.")
        ctk.CTkLabel(box, text="Device IP", font=self.fonts["bodyb"],
                     anchor="w").pack(fill="x")
        self.ip_entry = self._sz(ctk.CTkEntry(box, font=self.fonts["body"]), 38)
        self.ip_entry.insert(0, self.cfg.get("device_ip", "192.168.4.1"))
        self.ip_entry.pack(fill="x", pady=(2, 0))
        self.ip_entry.bind("<FocusOut>",
                           lambda _: self._save_setting("device_ip", self.ip_entry))

        # ── Serial speeds ───────────────────────────────────────────────────
        # Both are already stated per environment in platformio.ini, so these
        # fields start from there and only need touching to override — leave
        # one empty and it follows the env again.
        box = self._settings_group(
            frame, "Serial speeds",
            "Leave a field empty to follow platformio.ini. Type a number only "
            "to override this board's own setting.")
        ctk.CTkLabel(box, text="Upload baud", font=self.fonts["bodyb"],
                     anchor="w").pack(fill="x")
        self.baud_entry = self._sz(ctk.CTkEntry(
            box, font=self.fonts["body"],
            placeholder_text="from platformio.ini"), 38)
        self.baud_entry.insert(0, str(self.cfg.get("baud") or ""))
        self.baud_entry.pack(fill="x", pady=(2, 8))
        self.baud_entry.bind("<FocusOut>", lambda _: self._save_setting(
            "baud", self.baud_entry, int_val=True))

        ctk.CTkLabel(box, text="Monitor baud", font=self.fonts["bodyb"],
                     anchor="w").pack(fill="x")
        self.monitor_entry = self._sz(ctk.CTkEntry(
            box, font=self.fonts["body"],
            placeholder_text="from platformio.ini"), 38)
        self.monitor_entry.insert(0, str(self.cfg.get("monitor_speed") or ""))
        self.monitor_entry.pack(fill="x", pady=(2, 0))
        self.monitor_entry.bind("<FocusOut>", lambda _: self._save_setting(
            "monitor_speed", self.monitor_entry, int_val=True))

        # Where those numbers come from, refreshed with the environment.
        self.baud_info_label = ctk.CTkLabel(
            box, text="", font=self.fonts["small"],
            text_color=COLORS["muted"], wraplength=self.px(620),
            anchor="w", justify="left")
        self.baud_info_label.pack(fill="x", pady=(6, 0))

        # ── Web upload ──────────────────────────────────────────────────────
        box = self._settings_group(
            frame, "Web upload over HTTP (step 8)",
            "Which copies of the web assets go to a device that is already "
            "running.")
        self._filter_labels = {v: k for k, v in _UPLOAD_FILTER_LABELS.items()}
        current = self.cfg.get("upload_filter", "all")
        self.filter_var = ctk.StringVar(
            value=_UPLOAD_FILTER_LABELS.get(current, current))
        self._sz(ctk.CTkOptionMenu(
            box, values=[_UPLOAD_FILTER_LABELS[k] for k in _UPLOAD_FILTERS],
            variable=self.filter_var, font=self.fonts["body"],
            dropdown_font=self.fonts["body"],
            command=self._on_filter_change), 38).pack(fill="x")

        self.wipe_var = ctk.BooleanVar(value=self.cfg.get("wipe_before_upload", False))
        ctk.CTkCheckBox(
            box, text="Delete everything in /www before uploading",
            variable=self.wipe_var, font=self.fonts["body"],
            checkbox_width=self.px(22), checkbox_height=self.px(22),
            command=lambda: self._save_setting(
                "wipe_before_upload", None, self.wipe_var.get())
        ).pack(anchor="w", pady=(10, 0))

        # ── USB CDC ─────────────────────────────────────────────────────────
        box = self._settings_group(
            frame, "USB CDC on boot (ESP32-C3 / S3)",
            "Applied at compile time by rewriting the flag in platformio.ini.")
        self.usb_cdc_var = ctk.BooleanVar(value=self.cfg.get("usb_cdc_on_boot", True))
        self.usb_cdc_check = ctk.CTkCheckBox(
            box, text="USB CDC on boot", variable=self.usb_cdc_var,
            font=self.fonts["body"], checkbox_width=self.px(22),
            checkbox_height=self.px(22),
            command=lambda: self._save_setting(
                "usb_cdc_on_boot", None, self.usb_cdc_var.get()))
        self.usb_cdc_check.pack(anchor="w")

        # Filled in by _refresh_env_labels(), which reads the chip family
        # rather than pattern-matching the env NAME for "esp32c3" / "esp32s3"
        # as this used to — a test no board is obliged to pass.
        self.usb_info_label = ctk.CTkLabel(
            box, text="", font=self.fonts["small"],
            text_color=COLORS["muted"], wraplength=self.px(620),
            anchor="w", justify="left")
        self.usb_info_label.pack(fill="x", pady=(6, 0))

        self._refresh_env_labels(sync_cdc=True)

    def _settings_group(self, parent, title: str, hint: str = ""):
        card = ctk.CTkFrame(parent)
        card.pack(fill="x", pady=(0, 12))
        head = ctk.CTkLabel(card, text=f"{title}  ⓘ" if hint else title,
                            font=self.fonts["h2"], anchor="w")
        head.pack(fill="x", padx=16, pady=(14, 0))
        self._hint(head, hint)
        inner = ctk.CTkFrame(card, fg_color="transparent")
        inner.pack(fill="x", padx=16, pady=(10, 16))
        return inner

    def _on_filter_change(self, label: str) -> None:
        self._save_setting("upload_filter", None,
                           self._filter_labels.get(label, "all"))

    def _build_build_tab(self) -> None:
        """Compile-time features, the radio key, and the node target."""
        tab = self.tab_view.add(TAB_BUILD)
        frame = ctk.CTkScrollableFrame(tab)
        frame.pack(fill="both", expand=True, padx=8, pady=8)
        self._build_feature_section(frame)

    def _build_feature_section(self, parent) -> None:
        """Checkboxes for the optional compile-time features.

        The list is read from src/setup.h through tools/features.py, so a
        feature added to the firmware appears here without this file being
        touched — the same arrangement pio_envs.py gives the board dropdown,
        and for the same reason: four hand-maintained copies of one list is
        four things to go stale, and they all did.

        EVERY feature gets a checkbox, and clearing one means it. That is
        newer than it looks: setup.h writes `#ifndef X / #define X`, so a -D
        flag could only ever add, and this list used to hold the off-by-default
        half — BME280 had no switch at all, and neither did the 34 KB of SD
        driver on a device that may never have a card in it. The tools now pass
        FEATURE_SET_EXPLICIT, which skips those defaults, so what is ticked
        here is the whole build.

        A dot marks what a plain `pio run` would have given you. It is the only
        thing the old on/off split still usefully said, and "Default set"
        below restores exactly that.

        The list used to live in a 190 px box nested inside a scrolling panel
        inside a tab — three scrollbars deep, and the only way to read thirty
        features. It has the tab to itself now.
        """
        head = ctk.CTkLabel(parent, text="Build features  ⓘ",
                            font=self.fonts["h1"], anchor="w")
        head.pack(fill="x", pady=(4, 6))
        self._hint(head,
                   "Compiled in via PLATFORMIO_BUILD_FLAGS. No project file "
                   "is edited, so an abandoned deploy leaves the checkout as "
                   "it was.\n\nA • marks a feature a plain `pio run` would "
                   "have given you; \"Default set\" restores exactly those.\n\n"
                   "Hover any feature for what it is.")

        selected = set(m for m in (self.cfg.get("features") or []) if is_known(m))

        # A filter, because thirty checkboxes is a list you scroll rather than
        # read. It matches the macro, the label and the one-line summary, so
        # "i2c", "co2" and "mqtt" all find what you would expect them to.
        filt = ctk.CTkFrame(parent, fg_color="transparent")
        filt.pack(fill="x", pady=(0, 8))
        self.feature_filter = self._sz(ctk.CTkEntry(
            filt, font=self.fonts["body"],
            placeholder_text="Filter — name, bus or description"), 36)
        self.feature_filter.pack(side="left", fill="x", expand=True)
        self.feature_filter.bind("<KeyRelease>",
                                 lambda _: self._apply_feature_filter())
        self._sz(ctk.CTkButton(filt, text="✕", width=self.px(44),
                               font=self.fonts["small"], fg_color="gray40",
                               command=self._clear_feature_filter), 36
                 ).pack(side="left", padx=(6, 0))

        self.feature_count = ctk.CTkLabel(
            parent, text="", font=self.fonts["small"],
            text_color=COLORS["muted"], anchor="w")
        self.feature_count.pack(fill="x", pady=(0, 4))

        # Rows are kept so the filter can hide and re-show them IN ORDER —
        # tkinter appends a re-packed widget at the end, so filtering has to
        # forget everything and pack the survivors back in the original order.
        self._feature_rows: list[tuple[str, ctk.CTkFrame, ctk.CTkFrame]] = []
        self._feature_groups: list[ctk.CTkFrame] = []

        box = ctk.CTkFrame(parent)
        box.pack(fill="x", pady=(0, 6))
        for group, rows in grouped(optional_features()).items():
            gframe = ctk.CTkFrame(box, fg_color="transparent")
            gframe.pack(fill="x")
            self._feature_groups.append(gframe)
            ctk.CTkLabel(gframe, text=group, font=self.fonts["bodyb"],
                         anchor="w").pack(fill="x", padx=14, pady=(12, 2))
            for f in rows:
                var = ctk.BooleanVar(value=f.macro in selected)
                self.feature_vars[f.macro] = var
                label = (f.macro.replace("SENSOR_", "").replace("EXPORT_", "")
                                .replace("_ENABLED", ""))
                if f.enabled:
                    label = f"• {label}"
                row = ctk.CTkFrame(gframe, fg_color="transparent")
                row.pack(fill="x")
                check = ctk.CTkCheckBox(
                    row, text=label, variable=var,
                    command=lambda m=f.macro: self._on_feature_toggle(m),
                    font=self.fonts["body"], checkbox_width=self.px(22),
                    checkbox_height=self.px(22))
                check.pack(anchor="w", padx=(24, 0), pady=(3, 0))
                # Thirty features, each with a line of description under it,
                # is sixty lines to scroll past to reach the ESP-NOW key.
                self._hint(check, f"{f.macro}\n{f.summary}" if f.summary
                                  else f.macro)
                self._feature_rows.append(
                    (f"{f.macro} {label} {f.summary or ''} {group}".lower(),
                     row, gframe))
        ctk.CTkLabel(box, text="", font=self.fonts["small"]).pack()
        self._refresh_feature_count()

        # A starting point. Thirty cleared boxes is what a fresh config now
        # looks like, and the first thing anyone wants from it is "what I
        # would have got anyway, then my changes".
        row = ctk.CTkFrame(parent, fg_color="transparent")
        row.pack(anchor="w", pady=(2, 0))
        self._sz(ctk.CTkButton(row, text="Default set", width=self.px(130),
                               font=self.fonts["small"],
                               command=self._features_default), 34
                 ).pack(side="left", padx=(0, 8))
        self._sz(ctk.CTkButton(row, text="Clear all", width=self.px(120),
                               font=self.fonts["small"], fg_color="gray40",
                               command=self._features_clear), 34).pack(side="left")

        # The rule setup.h enforces with an #error, said here while there is
        # still something to click. A compiler diagnostic is a fine backstop
        # and a poor first contact.
        self.feature_warning = ctk.CTkLabel(
            parent, text="", font=self.fonts["body"],
            text_color=COLORS["warning"], wraplength=self.px(640),
            anchor="w", justify="left")
        self.feature_warning.pack(fill="x", pady=(8, 0))
        self._refresh_feature_warning()

        # The ESP-NOW key. Shown always rather than hidden behind its feature:
        # a field that appears and disappears as a checkbox is clicked is
        # harder to find than one that is simply greyed out by irrelevance,
        # and the warning below is the thing that most needs to be read.
        ctk.CTkLabel(parent, text="ESP-NOW shared key (16 characters)",
                     font=self.fonts["h2"], anchor="w").pack(fill="x", pady=(18, 4))
        keyrow = ctk.CTkFrame(parent, fg_color="transparent")
        keyrow.pack(anchor="w", fill="x")
        self.espnow_lmk_entry = self._sz(ctk.CTkEntry(
            keyrow, width=self.px(300), font=self.fonts["mono"]), 38)
        self.espnow_lmk_entry.insert(0, self.cfg.get("espnow_lmk") or "")
        self.espnow_lmk_entry.pack(side="left")
        self.espnow_lmk_entry.bind("<FocusOut>", lambda _: self._save_espnow_lmk())
        self._sz(ctk.CTkButton(keyrow, text="Generate", width=self.px(120),
                               font=self.fonts["small"],
                               command=self._generate_espnow_key), 38
                 ).pack(side="left", padx=(8, 0))

        self.espnow_lmk_note = ctk.CTkLabel(
            parent, text="", font=self.fonts["small"],
            text_color=COLORS["muted"], wraplength=self.px(640),
            anchor="w", justify="left")
        self.espnow_lmk_note.pack(fill="x", pady=(6, 12))
        self._refresh_espnow_note()

        self._build_node_section(parent)

    def _generate_espnow_key(self) -> None:
        """A fresh key, into the field and into the config.

        Generated rather than invented: a key somebody types is a key somebody
        can remember, and this one is the only thing between the collector's
        pipeline and any ESP-NOW frame in radio range. It goes into whichever
        target this tool builds next — collector or node — so the two sides
        match without anyone copying it between two windows.
        """
        key = generate_espnow_key()
        self.espnow_lmk_entry.delete(0, "end")
        self.espnow_lmk_entry.insert(0, key)
        self._save_espnow_lmk()
        self._notify("A new 16-character ESP-NOW key was generated and saved.",
                     "success", seconds=8)

    def _build_node_section(self, parent) -> None:
        """Which satellite board steps 10 and 11 build and flash.

        Its own project, env and port. A node is a different board on a
        different USB device, and borrowing the collector's port is the
        shortest path to flashing an ESP8266 image at an ESP32-C3.
        """
        node_head = ctk.CTkLabel(parent, text="Node target  ⓘ",
                                 font=self.fonts["h2"], anchor="w")
        node_head.pack(fill="x", pady=(10, 4))
        self._hint(node_head,
                   "Steps 10, 11 and 12 build and flash this project. It gets "
                   "its own env and its own port, because a node is a "
                   "different board on a different USB device — borrowing the "
                   "collector's port is the shortest path to flashing an "
                   "ESP8266 image at an ESP32-C3.\n\nThe ESP-NOW key above "
                   "travels with it, so the node and the collector hold the "
                   "same 16 bytes without either being typed twice.")

        proj = node_project(self.cfg)
        labels = [p.label for p in NODE_PROJECTS.values()]
        self._node_label_to_key = {p.label: k for k, p in NODE_PROJECTS.items()}
        self.node_project_var = ctk.StringVar(value=proj.label)
        self._sz(ctk.CTkOptionMenu(
            parent, values=labels, variable=self.node_project_var,
            font=self.fonts["body"], dropdown_font=self.fonts["body"],
            command=self._on_node_project_change), 38).pack(fill="x")

        self.node_blurb = ctk.CTkLabel(
            parent, text=proj.blurb, font=self.fonts["small"],
            text_color=COLORS["muted"], wraplength=self.px(640),
            anchor="w", justify="left")
        self.node_blurb.pack(fill="x", pady=(4, 0))

        row = ctk.CTkFrame(parent, fg_color="transparent")
        row.pack(fill="x", pady=(8, 0))
        ctk.CTkLabel(row, text="env", font=self.fonts["body"],
                     width=self.px(46), anchor="w").pack(side="left")
        self.node_env_entry = self._sz(ctk.CTkEntry(
            row, width=self.px(180), font=self.fonts["body"],
            placeholder_text=proj.default_env), 38)
        self.node_env_entry.insert(0, self.cfg.get("node_env") or "")
        self.node_env_entry.pack(side="left", padx=(0, 16))
        self.node_env_entry.bind(
            "<FocusOut>", lambda _: self._save_setting("node_env", self.node_env_entry))
        ctk.CTkLabel(row, text="port", font=self.fonts["body"],
                     width=self.px(52), anchor="w").pack(side="left")
        self.node_port_entry = self._sz(ctk.CTkEntry(
            row, width=self.px(180), font=self.fonts["body"],
            placeholder_text="auto-detect"), 38)
        self.node_port_entry.insert(0, self.cfg.get("node_port") or "")
        self.node_port_entry.pack(side="left")
        self.node_port_entry.bind(
            "<FocusOut>", lambda _: self._save_setting("node_port", self.node_port_entry))

        ctk.CTkLabel(parent, text="", font=self.fonts["small"]).pack(pady=(0, 6))

    def _build_wifi_tab(self) -> None:
        """WiFi provisioning, in a tab rather than a modal dialog.

        This was a CTkToplevel with grab_set(): a second window that, on the
        window managers where it opened behind the main one, locked every
        control in the application with no visible cause. It is a form now,
        and the output goes to the same log as everything else.
        """
        tab = self.tab_view.add(TAB_WIFI)
        frame = ctk.CTkScrollableFrame(tab)
        frame.pack(fill="both", expand=True, padx=8, pady=8)

        head = ctk.CTkLabel(frame, text="WiFi provisioning  ⓘ",
                            font=self.fonts["h1"], anchor="w")
        head.pack(fill="x", pady=(4, 12))
        self._hint(head,
                   "Sends the credentials to a device already running this "
                   "firmware, over the USB serial port selected on the left. "
                   "Nothing is written to disk and nothing is recompiled.")

        ctk.CTkLabel(frame, text="Network name (SSID)", font=self.fonts["bodyb"],
                     anchor="w").pack(fill="x")
        self.wifi_ssid = self._sz(ctk.CTkEntry(
            frame, font=self.fonts["body"], placeholder_text="Network name"), 38)
        self.wifi_ssid.pack(fill="x", pady=(2, 12))

        ctk.CTkLabel(frame, text="Password", font=self.fonts["bodyb"],
                     anchor="w").pack(fill="x")
        self.wifi_pass = self._sz(ctk.CTkEntry(
            frame, font=self.fonts["body"], show="•",
            placeholder_text="Password"), 38)
        self.wifi_pass.pack(fill="x", pady=(2, 6))

        self.wifi_show = ctk.BooleanVar(value=False)
        ctk.CTkCheckBox(
            frame, text="Show password", variable=self.wifi_show,
            font=self.fonts["small"], checkbox_width=self.px(20),
            checkbox_height=self.px(20),
            command=lambda: self.wifi_pass.configure(
                show="" if self.wifi_show.get() else "•")).pack(anchor="w")

        self.wifi_btn = self._sz(ctk.CTkButton(
            frame, text="📡   Send credentials over USB",
            font=self.fonts["button"], command=self._start_provision), 46)
        self.wifi_btn.pack(fill="x", pady=(16, 6))

        self.wifi_note = ctk.CTkLabel(
            frame, text="", font=self.fonts["small"],
            text_color=COLORS["muted"], wraplength=self.px(640),
            anchor="w", justify="left")
        self.wifi_note.pack(fill="x")

    def _build_help_tab(self) -> None:
        tab = self.tab_view.add(TAB_HELP)
        # Monospaced because the page is laid out in columns — the keyboard
        # table and the step list only line up in a fixed-width font.
        info_text = ctk.CTkTextbox(tab, font=self.fonts["mono"], wrap="word")
        info_text.pack(fill="both", expand=True, padx=8, pady=8)
        info_text.insert("0.0", """\
ESP32 Logger — Deploy & Flash Tool
──────────────────────────────────

THE SHORT VERSION
  1. Pick your board in "1 · Board". Everything else — chip, upload speed,
     partition table — follows from it; nothing needs typing.
  2. Plug the board in and press "Rescan ports". A ★ marks a port whose USB
     ID matches the board you picked.
  3. Press one of the jobs under "3 · What do you want to do?".
  4. Press RUN. Output appears in this window, under the Run tab.

  Never flashed this board before?  "Full flash".
  Changed the firmware?             "Quick flash".
  Changed only the web UI, device already on WiFi?  "HTTP deploy".

HINTS ON HOVER
  Rest the pointer on a job button, a step, a feature or any heading marked
  ⓘ and a balloon says what it is. That is where the explanations went: as
  printed lines they doubled the height of every list in this window.

NO POP-UPS
  This tool never opens a second window — the hint balloon included; it is a
  label drawn inside this window, not a tooltip window. Warnings, results and
  questions, the erase and bootloader confirmations among them, appear in the
  status bar along the bottom with the buttons to answer them. If a step seems to be waiting,
  look at the bottom of the window.

READABILITY
  A− / A+ in the header resize the whole interface (Ctrl+- / Ctrl++, Ctrl+0
  to reset). The Dark / Light / System menu beside them switches theme. Both
  are remembered in .flash_tool.json.

KEYBOARD
  Ctrl+R   Run the selected steps        F5       Rescan serial ports
  Ctrl+S   Save configuration            Ctrl++   Larger text
  Ctrl+L   Clear the log                 Ctrl+-   Smaller text
                                         Ctrl+0   Reset text size

WHERE THE SETTINGS LIVE
  Settings tab   device IP, upload and monitor baud, HTTP upload filter,
                 USB CDC on boot.
  Build tab      the compile-time feature list read from src/setup.h, the
                 16-character ESP-NOW key, and which satellite node project
                 steps 10–12 build.
  WiFi tab       send SSID and password to a running device over serial.

  Everything is saved to .flash_tool.json in the project root as you change
  it. A value that matches platformio.ini is not written there at all, so
  switching board carries that board's own settings with it — leave a field
  empty to follow the project.

THE STEPS
""" + "\n".join(
            f"  {n:>2}. {title:<24} {detail}"
            for n, (title, detail) in ((n, step_parts(n)) for n in STEP_NAMES)
        ) + """

LOG COLOURS
  green = success    red = error    yellow = warning    grey = information

REQUIREMENTS
  PlatformIO must be installed and on PATH; it is far too large to bundle.
      pip install platformio
  Serial ports are enumerated with pyserial. Without it you can still type a
  port by hand.
""")
        info_text.configure(state="disabled")

    # ── status bar: messages and questions ──────────────────────────────────
    def _notify(self, text: str, level: str = "info",
                seconds: Optional[int] = None) -> None:
        """Say something in the status bar. Safe from any thread."""
        def show():
            if self._notice_job is not None:
                try:
                    self.root.after_cancel(self._notice_job)
                except Exception:
                    pass
                self._notice_job = None
            self.status_label.configure(
                text=text, text_color=COLORS.get(level, COLORS["normal"]))
            if seconds:
                self._notice_job = self.root.after(
                    seconds * 1000,
                    lambda: self.status_label.configure(
                        text="Ready.", text_color=COLORS["muted"]))
        try:
            self.root.after(0, show)
        except Exception:
            pass                            # window already gone

    def _clear_answer_row(self) -> None:
        for child in self.answer_row.winfo_children():
            child.destroy()

    def _ask(self, question: str, yes: str = "Yes", no: str = "Cancel",
             danger: bool = False) -> bool:
        """Ask a yes/no question in the status bar; block until answered.

        Called from the worker thread running the steps, never from the UI
        thread — waiting on the UI thread would deadlock it against itself.
        deploy_core only asks during run_steps(), which this file always runs
        in a thread of its own, and the guard below makes the exception
        visible rather than hanging the window if that ever changes.
        """
        if threading.current_thread() is threading.main_thread():
            self._log(f"\n?  {question}  → declined (nothing can answer from "
                      f"the UI thread)\n", "warning")
            return False

        answered = threading.Event()
        result = {"value": False}
        self._pending.append(answered)

        def choose(value: bool) -> None:
            result["value"] = value
            self._clear_answer_row()
            answered.set()

        def build() -> None:
            self._clear_answer_row()
            self.status_label.configure(text=question,
                                        text_color=COLORS["warning"])
            self._sz(ctk.CTkButton(
                self.answer_row, text=no, font=self.fonts["small"],
                fg_color="gray40", width=self.px(110),
                command=lambda: choose(False)), 34).pack(side="right", padx=(8, 0))
            self._sz(ctk.CTkButton(
                self.answer_row, text=yes, font=self.fonts["button"],
                width=self.px(130),
                fg_color=("#c92a2a", "#a03030") if danger else None,
                hover_color=("#a61e1e", "#7d2323") if danger else None,
                command=lambda: choose(True)), 34).pack(side="right")

        self._log(f"\n?  {question}   [{yes} / {no}] — answer in the status bar "
                  f"at the bottom of the window.", "warning")
        try:
            self.root.after(0, build)
        except RuntimeError:
            # Tkinter refuses cross-thread calls unless the main thread is
            # inside mainloop(). That is only true while the window is
            # closing (or under a test harness that pumps events by hand), and
            # a question nobody can be shown must not become a step that runs
            # unasked.
            self._pending.remove(answered)
            return False
        answered.wait()
        if answered in self._pending:
            self._pending.remove(answered)
        self._log(f"   → {yes if result['value'] else no}\n",
                  "warning" if result["value"] else "info")
        self._notify("Ready.", "muted")
        return result["value"]

    def _confirm_erase(self) -> bool:
        """Erase confirmation — inline, where it cannot hide behind a window."""
        return self._ask(
            "Erase the whole flash? This deletes the configuration, the logs "
            "and the LittleFS filesystem on the device.",
            yes="Erase everything", no="Skip this step", danger=True)

    def _confirm_bootloader(self) -> bool:
        """Bootloader confirmation — the same question, in the same place.

        flash_bootloader.py asks this itself when run from a terminal. It
        cannot ask a windowed build: there is no console behind this window
        and so no stdin, which is why the step used to end on an EOFError
        instead of on an answer. Asked here, the script is told --yes.
        """
        return self._ask(
            "Flash the rollback-enabled bootloader? This overwrites the "
            "bootloader currently on the device.",
            yes="Flash bootloader", no="Skip this step", danger=True)

    def _on_close(self) -> None:
        """Release anything blocked on a question, then close.

        A worker thread waiting on _ask() would otherwise sit on an event
        nobody can set any more. It is a daemon thread, so the process still
        exits — but it would hold the serial port open until it did.
        """
        for event in list(self._pending):
            event.set()
        self.root.destroy()

    # ── ports ───────────────────────────────────────────────────────────────
    def _port_label(self, dev: str, desc: str, match: bool) -> str:
        return f"{'★ ' if match else ''}{dev}  —  {desc}"

    def _port_from_label(self, text: str) -> str:
        """The device path out of a picker label, or out of typed text."""
        text = (text or "").strip()
        if not text or text == AUTO_PORT:
            return ""
        return text.split("—")[0].replace("★", "").strip()

    def _current_port(self) -> str:
        return self._port_from_label(self.port_box.get())

    def _save_port(self) -> None:
        self._save_setting("port", None, self._current_port())

    def _on_port_pick(self, label: str) -> None:
        self._save_port()
        port = self._current_port()
        self._notify(f"Port set to {port}." if port else
                     "Port set to auto-detect: the best USB ID match wins.",
                     "info", seconds=6)

    def _refresh_ports(self, announce: bool = True) -> None:
        """Fill the port list from what is plugged in, ranked by USB ID.

        Ranked by the selected board's own USB VID:PID, best match first.
        Listing every port the OS knows about — including a motherboard's
        COM1 — and letting the user guess is how firmware ends up on the
        ESP8266 node instead of the collector.
        """
        if not HAS_PYSERIAL:
            self._set_port_note(
                "pyserial is missing — type the port by hand.", "warning",
                "The pyserial package enumerates serial ports; without it "
                "this list stays empty and nothing is auto-detected.\n\n"
                "  pip install pyserial")
            if announce:
                self._notify("pyserial is not installed — serial ports cannot "
                             "be listed. pip install pyserial", "warning")
            return

        try:
            ranked = ports_for(self.cfg.get("env") or detect_env())
        except Exception as exc:
            self._set_port_note("Could not list ports.", "error", str(exc))
            return

        values = [AUTO_PORT] + [self._port_label(*r) for r in ranked]
        saved = (self.cfg.get("port") or "").strip()
        devices = [r[0] for r in ranked]
        if saved and saved not in devices:
            values.append(f"{saved}  —  saved, not currently connected")

        self.port_box.configure(values=values)

        # Auto-select only when there is nothing to overrule. Rescanning used
        # to snap the box back to the best USB-ID match, which quietly undid a
        # deliberate choice of the second board on the bench.
        matches = [r for r in ranked if r[2]]
        if len(matches) == 1 and (not saved or saved not in devices):
            chosen = matches[0]
            self.port_box.set(self._port_label(*chosen))
            self._save_port()
        elif saved:
            for r in ranked:
                if r[0] == saved:
                    self.port_box.set(self._port_label(*r))
                    break
            else:
                self.port_box.set(f"{saved}  —  saved, not currently connected")
        else:
            self.port_box.set(AUTO_PORT)

        env = self.cfg.get("env") or detect_env()
        if not ranked:
            self._set_port_note(
                "No USB serial ports found.", "warning",
                "Non-USB ports (COM1, /dev/ttyS0) are not listed: no board "
                "sits behind one.\n\nCheck that the cable carries data — a "
                "charge-only cable powers the board and enumerates nothing.")
            if announce:
                self._notify("No USB serial ports found — check the cable and "
                             "that it is a data cable, not charge-only.",
                             "warning")
        else:
            self._set_port_note(
                f"{len(ranked)} port(s); ★ matches {env}.", "muted",
                "\n".join(f"{'★ ' if m else '   '}{dev}  —  {desc}"
                          for dev, desc, m in ranked))
            if announce:
                self._notify(
                    f"{len(ranked)} port(s) found, {len(matches)} matching "
                    f"{env}." + (f" Selected {self._current_port()}."
                                 if len(matches) == 1 else
                                 " Pick one from the list."),
                    "success" if matches else "info", seconds=8)

    def _set_port_note(self, text: str, level: str, detail: str = "") -> None:
        """One short line under the port box; the long version on hover."""
        self.port_note.configure(text=text, text_color=COLORS[level])
        self._port_note_detail = detail

    def _board_detail(self) -> str:
        """What the one-line board summary leaves out, for its hint."""
        info = env_info(self.cfg.get("env", ""))
        lines = [f"[env:{info.name}]",
                 f"board       {info.board or 'unknown'}",
                 f"chip        {info.chip}",
                 f"flash       {info.flash_size or 'unknown — no board JSON'}",
                 f"partitions  {info.partitions or 'platform default'}",
                 f"filesystem  {info.filesystem or '-'}"]
        try:
            d = defaults_for(info.name)
            lines += [f"upload      {d['baud']} (from {d['baud_src']})",
                      f"monitor     {d['monitor_speed']} (from {d['monitor_src']})"]
        except Exception:
            pass
        return "\n".join(lines)

    # ── environment ─────────────────────────────────────────────────────────
    def _on_env_change(self, name: str) -> None:
        """Environment picked: persist it, then re-derive everything it states.

        A board switch has to carry the board's own settings with it — chip,
        upload speed, monitor speed, USB CDC state. Keeping the previous
        board's numbers is how you flash an S3 with a C3's bootloader.
        Anything the user pinned survives, because save_cfg() only stores a
        derived key when it differs from the env.
        """
        self.cfg["env"] = name
        save_cfg(self.cfg)
        self.cfg = load_cfg()
        self._refresh_env_labels(sync_cdc=True)
        self._refresh_ports(announce=False)
        self._notify(f"Board set to {name}. Upload speed, chip and USB CDC "
                     f"now follow it.", "info", seconds=8)

    def _refresh_env_labels(self, sync_cdc: bool = False) -> None:
        """Show what the selected env resolves to, and whether its USB CDC
        flag can be toggled. Both answers come from platformio.ini and the
        chip family, so a new board needs no change here."""
        info = env_info(self.cfg.get("env", ""))
        board = getattr(self, "board_label", None)
        if board is not None:
            # One line, because the rest of it is one hover away.
            board.configure(
                text=f"{info.board or 'unknown board'} · {info.chip}")
        usb = getattr(self, "usb_info_label", None)
        if usb is not None:
            pins = usb_pins(info.chip)
            if not info.supports_usb_cdc:
                txt = (f"[env:{info.name}] has no -DARDUINO_USB_CDC_ON_BOOT of its own — "
                       "it inherits one. Toggle it in the env it extends.")
            elif pins:
                txt = (f"{pins} are the USB D-/D+ pair on {info.chip}. On: serial console. "
                       "Off: free as GPIO. Applied at compile time.")
            else:
                txt = "Toggle USB CDC on boot (applied at compile time)."
            usb.configure(text=txt)
        check = getattr(self, "usb_cdc_check", None)
        if check is not None:
            check.configure(state="normal" if info.supports_usb_cdc else "disabled")
        # Only re-read the checkbox from the ini when the ENVIRONMENT changed.
        # Doing it on every refresh meant that toggling CDC off and then
        # touching the baud field snapped the checkbox back to the ini value,
        # because editing baud refreshes these labels.
        var = getattr(self, "usb_cdc_var", None)
        if sync_cdc and var is not None and info.usb_cdc_on_boot is not None:
            var.set(info.usb_cdc_on_boot)

        # The two baud fields and the note under them.
        d = defaults_for(info.name) if info.board else None
        note = getattr(self, "baud_info_label", None)
        if d and note is not None:
            note.configure(
                text=(f"Upload {d['baud']} from {d['baud_src']} · "
                      f"monitor {d['monitor_speed']} from {d['monitor_src']}. "
                      f"Leave a field empty to follow the environment."))
        for attr, key in (("baud_entry", "baud"), ("monitor_entry", "monitor_speed")):
            entry = getattr(self, attr, None)
            if entry is None or d is None:
                continue
            entry.delete(0, "end")
            # Show a pinned override; leave it blank when the env answers, so
            # the placeholder says where the number is coming from.
            if self.cfg.get(key) is not None and self.cfg.get(key) != d[key]:
                entry.insert(0, str(self.cfg[key]))

    # ── config plumbing ─────────────────────────────────────────────────────
    def _save_setting(self, key: str, widget=None, val=None, int_val: bool = False) -> None:
        """Save a setting to config (debounced on focus-out)."""
        if widget:
            val = widget.get()
        if int_val:
            # Empty means "follow platformio.ini": save_cfg() writes null and
            # load_cfg() fills the env's value back in. Previously the int
            # conversion was gated on key == "baud", so any other numeric field
            # would have been stored as a string.
            if val in ("", None):
                val = None
            else:
                try:
                    val = int(val)
                except ValueError:
                    self._notify(f"{key}: '{val}' is not a number, so it was "
                                 f"not saved.", "warning", seconds=8)
                    return
        self.cfg[key] = val
        save_cfg(self.cfg)
        if key in ("baud", "monitor_speed"):
            self.cfg = load_cfg()          # re-derive if the field was cleared
            self._refresh_env_labels()

    def _update_steps(self) -> None:
        """Update selected steps from checkboxes."""
        steps = [n for n, var in self.step_vars.items() if var.get()]
        self.cfg["steps"] = sorted(steps)
        save_cfg(self.cfg)
        self._refresh_plan()

    def _apply_preset(self, steps: list[int]) -> None:
        """Apply a preset."""
        for n, var in self.step_vars.items():
            var.set(n in steps)
        self._update_steps()
        if steps:
            self._notify(f"{len(steps)} step(s) selected. Press RUN when the "
                         f"board is connected.", "info", seconds=8)
        else:
            self._notify("All steps cleared.", "info", seconds=6)

    def _refresh_plan(self) -> None:
        """Spell out what RUN would do, and disable it when that is nothing."""
        label = getattr(self, "plan_label", None)
        if label is None:
            return
        steps = sorted(self.cfg.get("steps", []))
        if not steps:
            label.configure(
                text="Nothing selected — pick a job above, or tick steps under "
                     "Customise.", text_color=COLORS["warning"])
            self.run_btn.configure(state="disabled")
            return
        shown = steps[:5]
        lines = "\n".join(f"  {n}.  {step_parts(n)[0]}" for n in shown)
        if len(steps) > len(shown):
            lines += f"\n  … and {len(steps) - len(shown)} more"
        label.configure(text=f"Will run {len(steps)} step(s):\n{lines}",
                        text_color=COLORS["muted"])
        if not self.running:
            self.run_btn.configure(state="normal")

    # ── features ────────────────────────────────────────────────────────────
    def _set_features(self, macros) -> None:
        """Point every checkbox at `macros`, then save and re-validate."""
        wanted = set(macros)
        for m, var in self.feature_vars.items():
            var.set(m in wanted)
        self._save_setting("features", None, sorted(wanted))
        self._refresh_espnow_note()
        self._refresh_feature_warning()
        self._refresh_feature_count()

    def _apply_feature_filter(self) -> None:
        """Show only the features matching the filter box, in list order."""
        query = self.feature_filter.get().strip().lower()
        for _hay, row, _g in self._feature_rows:
            row.pack_forget()
        for group in self._feature_groups:
            group.pack_forget()
        shown = 0
        for group in self._feature_groups:
            rows = [row for hay, row, g in self._feature_rows
                    if g is group and (not query or query in hay)]
            if not rows:
                continue
            group.pack(fill="x")
            for row in rows:
                row.pack(fill="x")
            shown += len(rows)
        self._refresh_feature_count(shown)

    def _clear_feature_filter(self) -> None:
        self.feature_filter.delete(0, "end")
        self._apply_feature_filter()

    def _refresh_feature_count(self, shown: Optional[int] = None) -> None:
        label = getattr(self, "feature_count", None)
        if label is None:
            return
        total = len(self._feature_rows)
        chosen = sum(1 for v in self.feature_vars.values() if v.get())
        seen = total if shown is None else shown
        text = f"{chosen} of {total} features selected"
        if seen != total:
            text += f"  ·  {seen} shown by the filter"
        label.configure(text=text)

    def _features_default(self) -> None:
        self._set_features(f.macro for f in default_on_features())

    def _features_clear(self) -> None:
        self._set_features([])

    def _refresh_feature_warning(self) -> None:
        """Say when the selected set cannot produce a reading."""
        note = getattr(self, "feature_warning", None)
        if note is None:
            return
        chosen = [m for m, v in self.feature_vars.items() if v.get()]
        if has_a_reading_source(chosen):
            note.configure(text="")
        else:
            note.configure(
                text="Nothing to read from. Pick at least one sensor, or a "
                     "remote-node feature to receive readings from another "
                     "board — the firmware refuses to compile otherwise.")

    def _on_feature_toggle(self, macro: str) -> None:
        feats = [m for m, v in self.feature_vars.items() if v.get()]

        # FEATURE_ESPNOW_INGEST implies FEATURE_REMOTE_NODES — setup.h defines
        # it either way. Ticking it here as well keeps the box from describing
        # a build different from the one that will happen.
        if macro == "FEATURE_ESPNOW_INGEST" and macro in feats:
            dep = self.feature_vars.get("FEATURE_REMOTE_NODES")
            if dep is not None and not dep.get():
                dep.set(True)
                feats.append("FEATURE_REMOTE_NODES")

        self._save_setting("features", None, feats)
        self._refresh_espnow_note()
        self._refresh_feature_warning()
        self._refresh_feature_count()

    def _save_espnow_lmk(self) -> None:
        key = self.espnow_lmk_entry.get().strip()
        self._save_setting("espnow_lmk", None, key)
        self._refresh_espnow_note()

    def _refresh_espnow_note(self) -> None:
        note = getattr(self, "espnow_lmk_note", None)
        if note is None:
            return
        on = self.feature_vars.get("FEATURE_ESPNOW_INGEST")
        key = (self.cfg.get("espnow_lmk") or "").strip()

        if on is None or not on.get():
            note.configure(text="Only used with FEATURE_ESPNOW_INGEST.",
                           text_color=COLORS["muted"])
        elif not key:
            note.configure(
                text="Not set — the build falls back to the default in setup.h. "
                     "Change it before this leaves the bench.",
                text_color=COLORS["warning"])
        elif len(key) != 16:
            note.configure(
                text=f"{len(key)} characters — the firmware asserts on exactly 16, "
                     f"so the build will fail.",
                text_color=COLORS["error"])
        else:
            note.configure(
                text="Flash the SAME 16 characters into the node "
                     "(node_espnow/platformio.ini), or nothing will pair.",
                text_color=COLORS["muted"])

    def _on_node_project_change(self, label: str) -> None:
        key = self._node_label_to_key.get(label)
        if not key:
            return
        self._save_setting("node_project", None, key)
        # The env belonged to the old project; keeping it would offer an
        # ESP8266 env for an ESP32 build and fail two steps later.
        self._save_setting("node_env", None, "")
        self.node_env_entry.delete(0, "end")
        proj = NODE_PROJECTS[key]
        self.node_env_entry.configure(placeholder_text=proj.default_env)
        self.node_blurb.configure(text=proj.blurb)
        self._refresh_espnow_note()

    # ── log ─────────────────────────────────────────────────────────────────
    def _clear_logs(self) -> None:
        """Clear log text."""
        self.log_text.delete("1.0", "end")

    def _copy_log(self) -> None:
        """The log to the clipboard — the usual first step of asking for help."""
        text = self.log_text.get("1.0", "end").strip()
        if not text:
            self._notify("The log is empty.", "info", seconds=4)
            return
        self.root.clipboard_clear()
        self.root.clipboard_append(text)
        self._notify(f"{len(text.splitlines())} log line(s) copied to the "
                     f"clipboard.", "success", seconds=6)

    def _colorize_log(self, msg: str) -> tuple[str, str]:
        """Determine log color based on message content."""
        msg_lower = msg.lower()
        if any(x in msg_lower for x in ["error", "failed", "traceback", "exception", "✗"]):
            return msg, "error"
        elif any(x in msg_lower for x in ["success", "✓", "completed", "ready", "done"]):
            return msg, "success"
        elif any(x in msg_lower for x in ["warning", "warn", "note"]):
            return msg, "warning"
        else:
            return msg, "info"

    def _log_output(self, text: str) -> None:
        """The sink handed to DeployManager: insert exactly what it sends.

        deploy_core supplies its own line endings — subprocess lines arrive
        with the newline still attached, and its progress lines deliberately
        end without one so a "… ✓" can complete them. The old callback was
        _log() itself, which appended a newline to both: every message from
        the core came out double-spaced, and every inline ✓ landed on a line
        of its own under the thing it was ticking.
        """
        def append():
            _text, tag = self._colorize_log(text)
            self.log_text.insert("end", text, tag)
            self.log_text.see("end")
        try:
            self.root.after(0, append)
        except Exception:
            pass

    def _log(self, msg: str, tag: Optional[str] = None, end: str = "\n") -> None:
        """Log message to GUI with syntax highlighting (thread-safe)."""
        def append():
            text, guessed = self._colorize_log(msg)
            self.log_text.insert("end", text + end, tag or guessed)
            self.log_text.see("end")
        try:
            self.root.after(0, append)
        except Exception:
            pass

    # ── running ─────────────────────────────────────────────────────────────
    def _set_progress(self, done: int, total: int, text: str) -> None:
        def apply():
            self.progress.set(0 if not total else min(1.0, done / total))
            self.progress_label.configure(text=text)
        try:
            self.root.after(0, apply)
        except Exception:
            pass

    def _on_run(self) -> None:
        """Run selected steps in a background thread."""
        if self.running:
            self._notify("Already running — wait for the current steps to "
                         "finish.", "warning", seconds=6)
            return

        # Sync the entry fields the user may not have blurred out of.
        #
        # This must NOT go through _on_env_change(): that reloads the config
        # from disk, and neither the USB CDC checkbox nor an un-blurred baud
        # entry is on disk — so calling it here silently discarded both, and
        # the GUI's USB CDC toggle could never reach the build. The env is
        # already persisted by the option menu's own callback; all that is
        # needed here is to re-derive the chip from it.
        self._save_port()
        self._save_setting("device_ip", self.ip_entry)
        self._save_setting("baud", self.baud_entry, int_val=True)
        self._save_setting("monitor_speed", self.monitor_entry, int_val=True)
        self.cfg["env"] = self.env_var.get()
        self.cfg["chip"] = chip_for(self.cfg["env"])
        # The checkbox is the live intent; the build step writes it to the ini.
        self.cfg["usb_cdc_on_boot"] = bool(self.usb_cdc_var.get())

        steps = sorted(self.cfg.get("steps", []))
        if not steps:
            self._notify("No steps selected. Pick a job under \"3 · What do "
                         "you want to do?\".", "warning", seconds=10)
            return

        self.running = True
        self.run_btn.configure(state="disabled", text="⏳   Running…")
        self.tab_view.set(TAB_RUN)
        total = len(steps)
        self._set_progress(0, total, f"Starting {total} step(s)…")
        self._notify(f"Running {total} step(s)…", "info")

        def run_in_bg():
            done = [0]
            try:
                self._clear_logs()
                self._log("=" * 60)
                self._log("Deployment Started")
                self._log("=" * 60 + "\n")

                self.manager = DeployManager(self.cfg)

                def started(step, name):
                    self._log(f"\n[{step}] {name}")
                    self._set_progress(
                        done[0], total,
                        f"Step {done[0] + 1} of {total} — {step_parts(step)[0]}")

                def completed(step, rc):
                    done[0] += 1
                    self._log(f"  → Step {step} completed with code {rc}\n")
                    self._set_progress(done[0], total,
                                       f"{done[0]} of {total} step(s) done")

                self.manager.on_step_start = started
                self.manager.on_step_output = self._log_output
                self.manager.on_step_complete = completed

                success = self.manager.run_steps(
                    steps,
                    confirm_erase_callback=self._confirm_erase,
                    confirm_bootloader_callback=self._confirm_bootloader)

                self._log("\n" + "=" * 60)
                if success:
                    self._log("✓ All steps completed successfully!")
                    self._set_progress(total, total, "Finished")
                    self._notify("All steps completed successfully.", "success")
                else:
                    self._log("✗ Some steps failed. See logs above.")
                    self._set_progress(done[0], total, "Failed")
                    self._notify("Some steps failed — see the log above.",
                                 "error")
                self._log("=" * 60)
            except Exception as exc:
                self._log(f"\nERROR: {exc}\n")
                self._set_progress(done[0], total, "Failed")
                self._notify(f"Deployment stopped: {exc}", "error")
            finally:
                self.running = False
                self.root.after(0, lambda: self.run_btn.configure(
                    state="normal", text="▶   RUN"))

        thread = threading.Thread(target=run_in_bg, daemon=True)
        thread.start()

    # ── WiFi provisioning ───────────────────────────────────────────────────
    def _on_wifi(self) -> None:
        """The sidebar button: show the tab, do not open a window."""
        self.tab_view.set(TAB_WIFI)
        self.wifi_ssid.focus()

    def _start_provision(self) -> None:
        ssid = self.wifi_ssid.get().strip()
        password = self.wifi_pass.get().strip()

        if not ssid:
            self.wifi_note.configure(text="Enter the network name first.",
                                     text_color=COLORS["warning"])
            self.wifi_ssid.focus()
            return
        if not password:
            self.wifi_note.configure(
                text="Enter the password. An open network is not supported by "
                     "this path.", text_color=COLORS["warning"])
            self.wifi_pass.focus()
            return

        self.wifi_note.configure(
            text=f"Sending credentials for “{ssid}” over "
                 f"{self._current_port() or 'the auto-detected port'}. Output "
                 f"is in the Run tab.", text_color=COLORS["muted"])
        self.wifi_btn.configure(state="disabled", text="⏳   Provisioning…")
        self.tab_view.set(TAB_RUN)

        def run_provision():
            self._log("\n" + "=" * 60)
            self._log("WiFi Provisioning Started")
            self._log("=" * 60)
            self._log(f"Connecting to: {ssid}\n")
            try:
                manager = DeployManager(self.cfg)
                manager.on_step_output = self._log_output
                success = manager.provision_wifi(
                    input_fn=lambda _: ssid,
                    getpass_fn=lambda _: password,
                )
                self._log("\n" + "=" * 60)
                if success:
                    self._log("✓ WiFi Provisioning successful!")
                    self._notify(f"WiFi credentials for “{ssid}” accepted.",
                                 "success")
                else:
                    self._log("✗ WiFi Provisioning failed. Check device "
                              "connection and credentials.")
                    self._notify("WiFi provisioning failed — check the cable, "
                                 "the port and the password.", "error")
                self._log("=" * 60 + "\n")
            except Exception as exc:
                self._log(f"\n✗ Error: {exc}\n")
                self._notify(f"WiFi provisioning stopped: {exc}", "error")
            finally:
                self.root.after(0, lambda: self.wifi_btn.configure(
                    state="normal", text="📡   Send credentials over USB"))

        threading.Thread(target=run_provision, daemon=True).start()

    def _save_config(self) -> None:
        """Save configuration."""
        save_cfg(self.cfg)
        self._notify("Configuration saved to .flash_tool.json in the project "
                     "root.", "success", seconds=6)

    # ── keyboard ────────────────────────────────────────────────────────────
    def bind_shortcuts(self) -> None:
        r = self.root
        r.bind("<Control-r>", lambda _: None if self.running else self._on_run())
        r.bind("<Control-s>", lambda _: self._save_config())
        r.bind("<Control-l>", lambda _: self._clear_logs())
        r.bind("<F5>", lambda _: self._refresh_ports())
        for seq in ("<Control-plus>", "<Control-equal>", "<Control-KP_Add>"):
            r.bind(seq, lambda _: self._bump_scale(SCALE_STEP))
        for seq in ("<Control-minus>", "<Control-KP_Subtract>"):
            r.bind(seq, lambda _: self._bump_scale(-SCALE_STEP))
        r.bind("<Control-0>", lambda _: self._apply_scale(1.0))


def selftest() -> int:
    """Print what this build can see, and exit. No window is opened.

    Exists so a packaged binary can be CHECKED rather than merely started.
    "The exe launches" says nothing about whether it found platformio.ini,
    bundled pyserial, or can enumerate boards — and those are exactly the
    things a frozen build breaks silently. CI runs this against the artifact
    before publishing it.

    Exit code 1 when no project was found, so a CI step can just assert on it.
    """
    lines = [
        "ESP32 Deploy self-test",
        f"  frozen build : {getattr(sys, 'frozen', False)}",
        f"  executable   : {sys.executable}",
        f"  working dir  : {Path.cwd()}",
        f"  platformio.ini: {INI if not NO_PROJECT else '(NOT FOUND)'}",
        f"  pyserial     : {'yes' if HAS_PYSERIAL else 'NO'}",
    ]
    rc = 0
    try:
        import customtkinter as _ctk
        lines.append(f"  customtkinter: {_ctk.__version__}")
    except Exception as exc:                      # pragma: no cover
        lines.append(f"  customtkinter: FAILED ({exc})")
        rc = 1
    if NO_PROJECT:
        lines.append("  environments : none — no platformio.ini above the "
                     "working directory or the executable")
        rc = 1
    else:
        envs = environments()
        lines.append(f"  environments : {len(envs)}")
        lines += [f"    {e.name:<20} {e.board:<24} {e.chip:<9} "
                  f"{e.flash_size or '?':<6} upload={e.upload_speed}"
                  f"{'' if e.board_json_found else '   [no board JSON]'}"
                  for e in envs]
        # A bare "?" in the flash column is not self-explanatory, and the
        # cause is worth naming: the board definition lives in an installed
        # PlatformIO platform, so on a machine that has not downloaded it the
        # chip is a guess from the board id and the flash size is unknown.
        # That size ends up in the bootloader image header, so it is not
        # cosmetic.
        # The optional feature list, for the same reason the env list is here.
        # features.py reads src/setup.h through the project root, and a frozen
        # build resolves that root differently — __file__ lands in a temporary
        # extraction directory with no project above it. An empty feature list
        # in a shipped executable would look exactly like a project with no
        # optional features, and the first person to notice would be someone
        # who could not find a checkbox they knew should be there.
        try:
            from features import optional_features as _opt
            feats = _opt()
            lines.append(f"  features     : {len(feats)}")
            if not feats:
                lines.append("    none found — src/setup.h unreadable from this build?")
                rc = 1
        except Exception as exc:
            lines.append(f"  features     : FAILED ({exc})")
            rc = 1

        missing = [e.name for e in envs if not e.board_json_found]
        if missing:
            lines.append(
                f"  NOTE: no board definition found for {', '.join(missing)} — "
                "install PlatformIO and let it download the platform "
                "(`pio pkg install`) for exact chip and flash size.")

    report = "\n".join(lines)
    # A windowed PyInstaller build on Windows has no console: sys.stdout is
    # None there and print() raises. So the report is also written beside the
    # executable, which is the only way to read it on the platform where the
    # binary is most likely to be double-clicked.
    try:
        print(report)
    except Exception:
        pass
    try:
        out = Path(sys.executable).parent / "deploy_selftest.txt"
        out.write_text(report + "\n", encoding="utf-8")
    except Exception:
        pass
    return rc


def main() -> int:
    """Entry point."""
    if "--selftest" in sys.argv:
        return selftest()

    root = ctk.CTk()
    app = DeployerGUI(root)
    app.bind_shortcuts()

    root.mainloop()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(1)
