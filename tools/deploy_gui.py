#!/usr/bin/env python3
"""
tools/deploy_gui.py — Modern GUI for ESP32 Logger deployment.

Built with CustomTkinter for a professional, native-looking interface.
Shares deployment logic with deploy.py via deploy_core.py.

Features:
  • Responsive scrollable layout that works on any screen size
  • Dynamic serial port detection with refresh button
  • Rich text logging with syntax highlighting
  • Real WiFi provisioning modal
  • Optimized disk I/O (save on focus-out, not keystroke)
  • Native CustomTkinter theming (no hardcoded colors)
"""

import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import messagebox
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
    import serial.tools.list_ports
    HAS_PYSERIAL = True
except ImportError:
    HAS_PYSERIAL = False

# Add tools dir to path for imports
TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from deploy_core import (
    DeployManager,
    load_cfg,
    save_cfg,
    STEP_NAMES,
    PRESETS,
    detect_port,
    detect_env,
    _UPLOAD_FILTERS,
    _UPLOAD_FILTER_LABELS,
)
from pio_envs import (
    INI, NO_PROJECT, chip_for, defaults_for, env_info, environments, env_names,
    ports_for, usb_pins,
)

# ── Theme configuration ─────────────────────────────────────────────────────
ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("blue")

# Log text tag colors (accessible in dark mode)
LOG_COLORS = {
    "error": "#ff6b6b",      # Red
    "success": "#51cf66",    # Green
    "warning": "#ffd93d",    # Yellow
    "info": "#aaaaaa",       # Gray
}


class DeployerGUI:
    def __init__(self, root: ctk.CTk):
        self.root = root
        self.root.title("ESP32 Logger — Deploy & Flash Tool")
        self.root.geometry("1100x750")
        self.root.minsize(900, 650)

        # Load config
        self.cfg = load_cfg()
        self.manager: Optional[DeployManager] = None
        self.running = False
        self.step_vars: dict[int, tk.BooleanVar] = {}

        # Setup UI
        self._build_ui()
        self.root.after(200, self._warn_if_no_project)

    def _warn_if_no_project(self) -> None:
        """Say so when there is no platformio.ini to read.

        Every board list is empty in that state. Without this the tool opens
        with a blank environment dropdown and no explanation, which reads as a
        broken download rather than as a tool started in the wrong place — the
        most likely first experience of the pre-built binary.
        """
        if not NO_PROJECT:
            return
        messagebox.showwarning(
            "No PlatformIO project found",
            "Could not find a platformio.ini, so there are no boards to offer.\n\n"
            "Put this program inside your ESP32_Logger checkout (any "
            "subdirectory will do), or start it from that directory, then "
            "reopen it.\n\n"
            f"Looked upward from:\n  {Path.cwd()}\n  {Path(sys.executable).parent}")

    def _build_ui(self) -> None:
        """Build the main UI with responsive layout."""
        # Header
        header = ctk.CTkFrame(self.root)
        header.pack(side="top", fill="x", padx=0, pady=0)

        title = ctk.CTkLabel(
            header,
            text="ESP32 Logger — Deploy & Flash Tool",
            font=("Helvetica", 18, "bold"),
        )
        title.pack(padx=20, pady=15)

        # Main content area with sidebar + tabs
        main_container = ctk.CTkFrame(self.root)
        main_container.pack(side="top", fill="both", expand=True, padx=10, pady=10)
        main_container.grid_columnconfigure(1, weight=1)
        main_container.grid_rowconfigure(0, weight=1)

        # Left sidebar (compact, essential settings only)
        self._build_sidebar(main_container)

        # Right side (Tabbed interface)
        self._build_tabs(main_container)

    def _build_sidebar(self, parent: ctk.CTkFrame) -> None:
        """Build left sidebar with essential settings and controls."""
        sidebar = ctk.CTkScrollableFrame(
            parent,
            width=280,
            orientation="vertical"
        )
        sidebar.grid(row=0, column=0, sticky="nsew", padx=(0, 10))

        # ── Settings Section (Essential Only) ─────────────────────────────────
        self._build_settings_section(sidebar)

        # ── Steps Section ──────────────────────────────────────────────────────
        self._build_steps_section(sidebar)

        # ── Presets Section ────────────────────────────────────────────────────
        self._build_presets_section(sidebar)

        # ── Action Buttons Section ─────────────────────────────────────────────
        self._build_actions_section(sidebar)

    def _build_settings_section(self, sidebar: ctk.CTkScrollableFrame) -> None:
        """Essential settings (Environment, Port, Chip). Advanced in Config tab."""
        sect = ctk.CTkFrame(sidebar)
        sect.pack(fill="x", padx=10, pady=(10, 0))

        label = ctk.CTkLabel(sect, text="⚙️  Essential Settings", font=("Helvetica", 12, "bold"))
        label.pack(anchor="w", pady=(10, 5))

        # Environment — a list read from platformio.ini, not a text field.
        # Typed free text let a nonexistent env through to pio, which failed
        # minutes later with a message that did not say what was wrong.
        ctk.CTkLabel(sect, text="PlatformIO Env:", font=("Helvetica", 10)).pack(anchor="w")
        names = env_names() or [detect_env()]
        current = self.cfg.get("env") or detect_env()
        if current not in names:
            names = [current] + names       # keep an unknown saved env visible
        self.env_var = ctk.StringVar(value=current)
        ctk.CTkOptionMenu(
            sect,
            values=names,
            variable=self.env_var,
            command=self._on_env_change,
        ).pack(fill="x", pady=(0, 2))

        # What that env actually is. The chip is derived from it, never typed:
        # an env and a chip that could disagree was a way to write a C3
        # bootloader onto an S3.
        self.board_label = ctk.CTkLabel(
            sect, text="", font=("Helvetica", 8), text_color="gray",
            wraplength=250, justify="left")
        self.board_label.pack(anchor="w", pady=(0, 8))

        # Port with Refresh button
        port_frame = ctk.CTkFrame(sect)
        port_frame.pack(fill="x", pady=(0, 8))

        ctk.CTkLabel(port_frame, text="Serial Port:", font=("Helvetica", 10)).pack(anchor="w")

        port_input_frame = ctk.CTkFrame(port_frame)
        port_input_frame.pack(fill="x")

        self.port_entry = ctk.CTkEntry(port_input_frame, placeholder_text="auto-detect")
        self.port_entry.insert(0, self.cfg.get("port", ""))
        self.port_entry.pack(side="left", fill="x", expand=True, padx=(0, 5))
        self.port_entry.bind("<FocusOut>", lambda _: self._save_setting("port", self.port_entry))

        refresh_btn = ctk.CTkButton(
            port_input_frame,
            text="🔄",
            command=self._refresh_ports,
            width=35,
            font=("Helvetica", 10)
        )
        refresh_btn.pack(side="right")

        # Info: Advanced settings in Configuration tab
        info_label = ctk.CTkLabel(
            sect,
            text="💡 Advanced settings (IP, Baud, Upload Filter, USB CDC) are in the Configuration tab →",
            font=("Helvetica", 8),
            text_color="gray",
            wraplength=250
        )
        info_label.pack(anchor="w", pady=(0, 5))

    def _build_steps_section(self, sidebar: ctk.CTkScrollableFrame) -> None:
        """Step toggles."""
        sect = ctk.CTkFrame(sidebar)
        sect.pack(fill="x", padx=10, pady=10)

        label = ctk.CTkLabel(sect, text="📋 Steps", font=("Helvetica", 12, "bold"))
        label.pack(anchor="w", pady=(10, 5))

        enabled_steps = set(self.cfg.get("steps", []))
        for n, name in STEP_NAMES.items():
            var = tk.BooleanVar(value=n in enabled_steps)
            self.step_vars[n] = var
            check = ctk.CTkCheckBox(
                sect,
                text=f"{n}. {name[:28]}…" if len(name) > 28 else f"{n}. {name}",
                variable=var,
                font=("Helvetica", 9),
                command=self._update_steps,
            )
            check.pack(anchor="w", pady=2)

    def _build_presets_section(self, sidebar: ctk.CTkScrollableFrame) -> None:
        """Preset buttons."""
        sect = ctk.CTkFrame(sidebar)
        sect.pack(fill="x", padx=10, pady=10)

        label = ctk.CTkLabel(sect, text="⚡ Presets", font=("Helvetica", 12, "bold"))
        label.pack(anchor="w", pady=(10, 5))

        for key, (pname, psteps) in PRESETS.items():
            def _preset(steps=psteps):
                self._apply_preset(steps)

            btn = ctk.CTkButton(
                sect,
                text=pname,
                command=_preset,
                font=("Helvetica", 10),
            )
            btn.pack(fill="x", pady=3)

    def _build_actions_section(self, sidebar: ctk.CTkScrollableFrame) -> None:
        """Action buttons."""
        sect = ctk.CTkFrame(sidebar)
        sect.pack(fill="x", padx=10, pady=10)

        self.run_btn = ctk.CTkButton(
            sect,
            text="▶ RUN",
            command=self._on_run,
            font=("Helvetica", 12, "bold"),
        )
        self.run_btn.pack(fill="x", pady=5)

        save_btn = ctk.CTkButton(
            sect,
            text="💾 Save Config",
            command=self._save_config,
            font=("Helvetica", 10),
        )
        save_btn.pack(fill="x", pady=3)

        wifi_btn = ctk.CTkButton(
            sect,
            text="📡 WiFi Provision",
            command=self._on_wifi,
            font=("Helvetica", 10),
        )
        wifi_btn.pack(fill="x", pady=3)

    def _build_tabs(self, parent: ctk.CTkFrame) -> None:
        """Tabbed interface on the right."""
        self.tab_view = ctk.CTkTabview(parent)
        self.tab_view.grid(row=0, column=1, sticky="nsew")

        # Logs tab
        self._build_logs_tab()

        # Configuration tab (advanced settings)
        self._build_config_tab()

        # Info tab
        self._build_info_tab()

    def _build_logs_tab(self) -> None:
        """Output/logs viewer with syntax highlighting."""
        tab = self.tab_view.add("📋 Logs")
        tab.grid_rowconfigure(0, weight=1)
        tab.grid_columnconfigure(0, weight=1)

        self.log_text = ctk.CTkTextbox(
            tab,
            font=("Courier", 10),
        )
        self.log_text.grid(row=0, column=0, sticky="nsew", padx=5, pady=5)

        # Configure text tags for syntax highlighting
        # CTkTextbox.tag_config delegates to tk.Text which uses 'foreground', not 'text_color'
        for tag, color in LOG_COLORS.items():
            self.log_text.tag_config(tag, foreground=color)

        # Clear button
        clear_btn = ctk.CTkButton(
            tab,
            text="Clear",
            command=self._clear_logs,
            width=80,
        )
        clear_btn.grid(row=1, column=0, sticky="e", padx=5, pady=5)

    def _build_config_tab(self) -> None:
        """Configuration tab with advanced settings."""
        tab = self.tab_view.add("⚙️ Configuration")

        # Create scrollable frame for settings
        settings_frame = ctk.CTkScrollableFrame(tab, orientation="vertical")
        settings_frame.pack(fill="both", expand=True, padx=10, pady=10)

        # Device IP
        ctk.CTkLabel(settings_frame, text="Device IP:", font=("Helvetica", 10, "bold")).pack(anchor="w", pady=(10, 2))
        self.ip_entry = ctk.CTkEntry(settings_frame)
        self.ip_entry.insert(0, self.cfg.get("device_ip", "192.168.4.1"))
        self.ip_entry.pack(fill="x", pady=(0, 10))
        self.ip_entry.bind("<FocusOut>", lambda _: self._save_setting("device_ip", self.ip_entry))

        # Upload and monitor baud. Both are already stated per environment in
        # platformio.ini, so these fields start from there and only need
        # touching to override — leave one empty and it follows the env again.
        ctk.CTkLabel(settings_frame, text="Upload Baud:",
                     font=("Helvetica", 10, "bold")).pack(anchor="w", pady=(10, 2))
        self.baud_entry = ctk.CTkEntry(settings_frame, placeholder_text="from platformio.ini")
        self.baud_entry.insert(0, str(self.cfg.get("baud") or ""))
        self.baud_entry.pack(fill="x", pady=(0, 2))
        self.baud_entry.bind("<FocusOut>", lambda _: self._save_setting("baud", self.baud_entry, int_val=True))

        ctk.CTkLabel(settings_frame, text="Monitor Baud:",
                     font=("Helvetica", 10, "bold")).pack(anchor="w", pady=(10, 2))
        self.monitor_entry = ctk.CTkEntry(settings_frame, placeholder_text="from platformio.ini")
        self.monitor_entry.insert(0, str(self.cfg.get("monitor_speed") or ""))
        self.monitor_entry.pack(fill="x", pady=(0, 2))
        self.monitor_entry.bind(
            "<FocusOut>",
            lambda _: self._save_setting("monitor_speed", self.monitor_entry, int_val=True))

        # Where those numbers come from, refreshed with the environment.
        self.baud_info_label = ctk.CTkLabel(
            settings_frame, text="", font=("Helvetica", 8), text_color="gray",
            wraplength=400, justify="left")
        self.baud_info_label.pack(anchor="w", pady=(0, 10))

        # Upload filter
        ctk.CTkLabel(settings_frame, text="Upload Filter:", font=("Helvetica", 10, "bold")).pack(anchor="w", pady=(10, 2))
        self.filter_var = ctk.StringVar(value=self.cfg.get("upload_filter", "all"))
        filter_menu = ctk.CTkOptionMenu(
            settings_frame,
            values=_UPLOAD_FILTERS,
            variable=self.filter_var,
            command=lambda v: self._save_setting("upload_filter", None, v),
        )
        filter_menu.pack(fill="x", pady=(0, 10))

        # Wipe toggle
        self.wipe_var = ctk.BooleanVar(value=self.cfg.get("wipe_before_upload", False))
        wipe_check = ctk.CTkCheckBox(
            settings_frame,
            text="Wipe /www before upload",
            variable=self.wipe_var,
            command=lambda: self._save_setting("wipe_before_upload", None, self.wipe_var.get()),
            font=("Helvetica", 10)
        )
        wipe_check.pack(anchor="w", pady=(10, 10))

        # USB CDC toggle (ESP32-C3, S3)
        ctk.CTkLabel(settings_frame, text="USB CDC Configuration:", font=("Helvetica", 10, "bold")).pack(anchor="w", pady=(10, 5))

        self.usb_cdc_var = ctk.BooleanVar(value=self.cfg.get("usb_cdc_on_boot", True))
        self.usb_cdc_check = ctk.CTkCheckBox(
            settings_frame,
            text="USB CDC on boot",
            variable=self.usb_cdc_var,
            command=lambda: self._save_setting("usb_cdc_on_boot", None, self.usb_cdc_var.get()),
            font=("Helvetica", 10)
        )
        self.usb_cdc_check.pack(anchor="w", pady=(0, 5))

        # Filled in by _refresh_env_labels(), which reads the chip family
        # rather than pattern-matching the env NAME for "esp32c3" / "esp32s3"
        # as this used to — a test no board is obliged to pass.
        self.usb_info_label = ctk.CTkLabel(
            settings_frame,
            text="",
            font=("Helvetica", 8),
            text_color="gray",
            wraplength=400
        )
        self.usb_info_label.pack(anchor="w", pady=(0, 10), padx=(20, 0))
        self._refresh_env_labels(sync_cdc=True)

    def _build_info_tab(self) -> None:
        """Info/help tab."""
        tab = self.tab_view.add("ℹ️ Info")

        info_text = ctk.CTkTextbox(tab, font=("Helvetica", 10))
        info_text.pack(fill="both", expand=True, padx=10, pady=10)
        info_text.insert("0.0", """\
ESP32 Logger — Deploy & Flash Tool
───────────────────────────────────

A comprehensive tool for building, flashing, and deploying
firmware to the ESP32 Logger with web UI.

Features:
  • Build web assets (minify & gzip)
  • Flash bootloader
  • Compile firmware
  • Flash to device
  • Upload LittleFS / web files
  • HTTP deployment to running device
  • Serial monitor
  • WiFi provisioning

Keyboard Shortcuts:
  Ctrl+R   Run selected steps
  Ctrl+S   Save configuration
  Ctrl+L   Clear logs

Configuration:
  Settings are saved to .flash_tool.json in the project root.
  They persist between sessions.

  Essential settings are in the left sidebar.
  Advanced settings (IP, Baud, USB CDC) are in the ⚙️ Configuration tab.

Log Colors:
  🟢 Green  = Success
  🔴 Red    = Error
  🟡 Yellow = Warning
  ⚪ Gray   = Info
""")
        info_text.configure(state="disabled")

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

    def _refresh_env_labels(self, sync_cdc: bool = False) -> None:
        """Show what the selected env resolves to, and whether its USB CDC
        flag can be toggled. Both answers come from platformio.ini and the
        chip family, so a new board needs no change here."""
        info = env_info(self.cfg.get("env", ""))
        board = getattr(self, "board_label", None)
        if board is not None:
            board.configure(
                text=f"{info.board or 'unknown board'} · {info.chip} · "
                     f"{info.flash_size or 'flash ?'} · {info.partitions or 'default parts'}")
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

    def _apply_preset(self, steps: list[int]) -> None:
        """Apply a preset."""
        for n, var in self.step_vars.items():
            var.set(n in steps)
        self._update_steps()

    def _clear_logs(self) -> None:
        """Clear log text."""
        self.log_text.delete("1.0", "end")

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

    def _log(self, msg: str, end: str = "\n") -> None:
        """Log message to GUI with syntax highlighting (thread-safe)."""
        def append():
            text, tag = self._colorize_log(msg)
            self.log_text.insert("end", text + end, tag)
            self.log_text.see("end")
        self.root.after(0, append)

    def _refresh_ports(self) -> None:
        """Refresh and display available serial ports."""
        if not HAS_PYSERIAL:
            messagebox.showerror(
                "Missing Dependency",
                "The 'pyserial' package is required for serial port detection.\n\n"
                "Please install it using:\n  pip install pyserial"
            )
            return

        # Ranked by the selected board's own USB VID:PID, best match first.
        # Listing every port the OS knows about — including a motherboard's
        # COM1 — and letting the user guess is how firmware ends up on the
        # ESP8266 node instead of the collector.
        ranked = ports_for(self.cfg.get("env") or detect_env())

        if not ranked:
            messagebox.showinfo(
                "No Ports",
                "No USB serial ports detected.\n\n"
                "Non-USB ports (COM1, /dev/ttyS0) are not listed: no board is "
                "behind one.")
            return

        matches = [r for r in ranked if r[2]]
        if len(matches) == 1:
            selected = matches[0][0]
        elif len(ranked) == 1:
            selected = ranked[0][0]
        else:
            listing = "\n".join(
                f"{'* ' if match else '  '}{dev}  —  {desc}"
                for dev, desc, match in ranked)
            dialog = ctk.CTkInputDialog(
                text=f"Ports (* matches this board):\n\n{listing}\n\nEnter port name:",
                title="Select Serial Port"
            )
            selected = dialog.get_input()
            if not selected:
                return

        self.port_entry.delete(0, "end")
        self.port_entry.insert(0, selected)
        self._save_setting("port", self.port_entry)
        messagebox.showinfo("Port Selected", f"Selected: {selected}")

    def _on_run(self) -> None:
        """Run selected steps in a background thread."""
        if self.running:
            messagebox.showwarning("Already Running", "Steps are already running.")
            return

        # Sync the entry fields the user may not have blurred out of.
        #
        # This must NOT go through _on_env_change(): that reloads the config
        # from disk, and neither the USB CDC checkbox nor an un-blurred baud
        # entry is on disk — so calling it here silently discarded both, and
        # the GUI's USB CDC toggle could never reach the build. The env is
        # already persisted by the option menu's own callback; all that is
        # needed here is to re-derive the chip from it.
        self._save_setting("port", self.port_entry)
        self._save_setting("device_ip", self.ip_entry)
        self._save_setting("baud", self.baud_entry, int_val=True)
        self._save_setting("monitor_speed", self.monitor_entry, int_val=True)
        self.cfg["env"] = self.env_var.get()
        self.cfg["chip"] = chip_for(self.cfg["env"])
        # The checkbox is the live intent; the build step writes it to the ini.
        self.cfg["usb_cdc_on_boot"] = bool(self.usb_cdc_var.get())

        steps = self.cfg.get("steps", [])
        if not steps:
            messagebox.showwarning("No Steps", "Please select at least one step.")
            return

        self.running = True
        self.run_btn.configure(state="disabled", text="⏸ Running…")

        def run_in_bg():
            try:
                self._clear_logs()
                self._log("=" * 60)
                self._log("Deployment Started")
                self._log("=" * 60 + "\n")

                self.manager = DeployManager(self.cfg)
                self.manager.on_step_start = lambda s, n: self._log(f"\n[{s}] {n}")
                self.manager.on_step_output = self._log
                self.manager.on_step_complete = lambda s, rc: self._log(
                    f"  → Step {s} completed with code {rc}\n"
                )

                success = self.manager.run_steps(steps, confirm_erase_callback=self._confirm_erase)

                self._log("\n" + "=" * 60)
                if success:
                    self._log("✓ All steps completed successfully!")
                else:
                    self._log("✗ Some steps failed. See logs above.")
                self._log("=" * 60)
            except Exception as exc:
                self._log(f"\nERROR: {exc}\n")
            finally:
                self.running = False
                self.root.after(0, lambda: self.run_btn.configure(state="normal", text="▶ RUN"))

        thread = threading.Thread(target=run_in_bg, daemon=True)
        thread.start()

    def _confirm_erase(self) -> bool:
        """Confirmation dialog for chip erase."""
        response = messagebox.askyesno(
            "Erase Flash",
            "This will erase ALL data on the chip:\n"
            "  • Configuration\n"
            "  • Logs\n"
            "  • LittleFS filesystem\n\n"
            "Continue?",
        )
        return response

    def _on_wifi(self) -> None:
        """Real WiFi provisioning modal with active provisioning."""
        # Create modal dialog
        dialog = ctk.CTkToplevel(self.root)
        dialog.title("WiFi Provisioning")
        dialog.geometry("400x280")
        dialog.resizable(False, False)

        # Make dialog modal
        dialog.transient(self.root)
        dialog.grab_set()

        # Center on parent
        dialog.update_idletasks()
        x = self.root.winfo_x() + (self.root.winfo_width() - dialog.winfo_width()) // 2
        y = self.root.winfo_y() + (self.root.winfo_height() - dialog.winfo_height()) // 2
        dialog.geometry(f"+{x}+{y}")

        # Header
        header = ctk.CTkLabel(
            dialog,
            text="📡 WiFi Provisioning",
            font=("Helvetica", 14, "bold")
        )
        header.pack(pady=(15, 10), padx=20)

        # Info
        info = ctk.CTkLabel(
            dialog,
            text="Connect device via USB, then enter WiFi credentials:",
            font=("Helvetica", 9),
            text_color="gray"
        )
        info.pack(anchor="w", padx=20, pady=(0, 10))

        # SSID field
        ctk.CTkLabel(dialog, text="SSID:", font=("Helvetica", 10)).pack(anchor="w", padx=20, pady=(10, 2))
        ssid_entry = ctk.CTkEntry(dialog, placeholder_text="Network name")
        ssid_entry.pack(fill="x", padx=20, pady=(0, 10))

        # Password field
        ctk.CTkLabel(dialog, text="Password:", font=("Helvetica", 10)).pack(anchor="w", padx=20, pady=(10, 2))
        password_entry = ctk.CTkEntry(dialog, placeholder_text="Password", show="•")
        password_entry.pack(fill="x", padx=20, pady=(0, 15))

        # Buttons
        button_frame = ctk.CTkFrame(dialog)
        button_frame.pack(fill="x", padx=20, pady=10)

        def provision():
            ssid = ssid_entry.get().strip()
            password = password_entry.get().strip()

            if not ssid:
                messagebox.showwarning("Missing SSID", "Please enter the WiFi network name.")
                return

            if not password:
                messagebox.showwarning("Missing Password", "Please enter the WiFi password.")
                return

            dialog.destroy()

            # Run provisioning in background thread
            def run_provision():
                self._log("\n" + "=" * 60)
                self._log("WiFi Provisioning Started")
                self._log("=" * 60)
                self._log(f"Connecting to: {ssid}\n")

                try:
                    manager = DeployManager(self.cfg)
                    manager.on_step_output = self._log

                    # Call provision_wifi with lambda callbacks that use entered credentials
                    success = manager.provision_wifi(
                        input_fn=lambda _: ssid,
                        getpass_fn=lambda _: password
                    )

                    self._log("\n" + "=" * 60)
                    if success:
                        self._log("✓ WiFi Provisioning successful!")
                    else:
                        self._log("✗ WiFi Provisioning failed. Check device connection and credentials.")
                    self._log("=" * 60 + "\n")
                except Exception as exc:
                    self._log(f"\n✗ Error: {exc}\n")

            thread = threading.Thread(target=run_provision, daemon=True)
            thread.start()

        cancel_btn = ctk.CTkButton(button_frame, text="Cancel", command=dialog.destroy)
        cancel_btn.pack(side="right", padx=(5, 0))

        provision_btn = ctk.CTkButton(
            button_frame,
            text="Start Provisioning",
            command=provision
        )
        provision_btn.pack(side="right")

        # Focus on SSID field
        ssid_entry.focus()

    def _save_config(self) -> None:
        """Save configuration."""
        save_cfg(self.cfg)
        messagebox.showinfo("Saved", "Configuration saved to .flash_tool.json")


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

    # Keyboard shortcuts
    root.bind("<Control-r>", lambda _: app._on_run() if not app.running else None)
    root.bind("<Control-s>", lambda _: app._save_config())
    root.bind("<Control-l>", lambda _: app._clear_logs())

    root.mainloop()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(1)
