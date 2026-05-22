#!/usr/bin/env python3
"""
tools/deploy_gui.py — Modern GUI for ESP32 Logger deployment.

Built with CustomTkinter for a professional, native-looking interface.
Shares deployment logic with deploy.py via deploy_core.py.
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

# ── Theme configuration ─────────────────────────────────────────────────────
ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("blue")


class DeployerGUI:
    def __init__(self, root: ctk.CTk):
        self.root = root
        self.root.title("ESP32 Logger — Deploy & Flash Tool")
        self.root.geometry("1000x700")
        self.root.minsize(800, 600)

        # Load config
        self.cfg = load_cfg()
        self.manager: Optional[DeployManager] = None
        self.running = False
        self.step_vars: dict[int, tk.BooleanVar] = {}

        # Setup UI
        self._build_ui()

    def _build_ui(self) -> None:
        """Build the main UI."""
        # Header
        header = ctk.CTkFrame(self.root, fg_color="#1a1a1a")
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

        # Left sidebar (Settings + Steps)
        self._build_sidebar(main_container)

        # Right side (Tabbed interface)
        self._build_tabs(main_container)

    def _build_sidebar(self, parent: ctk.CTkFrame) -> None:
        """Build left sidebar with settings and step toggles."""
        sidebar = ctk.CTkFrame(parent, width=250, fg_color="#242424")
        sidebar.pack(side="left", fill="both", padx=(0, 10))
        sidebar.pack_propagate(False)

        # ── Settings Section ───────────────────────────────────────────────────
        self._build_settings_section(sidebar)

        # ── Steps Section ──────────────────────────────────────────────────────
        self._build_steps_section(sidebar)

        # ── Presets Section ────────────────────────────────────────────────────
        self._build_presets_section(sidebar)

        # ── Action Buttons Section ─────────────────────────────────────────────
        self._build_actions_section(sidebar)

    def _build_settings_section(self, sidebar: ctk.CTkFrame) -> None:
        """Settings configuration area."""
        sect = ctk.CTkFrame(sidebar, fg_color="#2a2a2a")
        sect.pack(fill="x", padx=10, pady=(10, 0))

        label = ctk.CTkLabel(sect, text="⚙️  Settings", font=("Helvetica", 12, "bold"))
        label.pack(anchor="w", pady=(10, 5))

        # Environment
        ctk.CTkLabel(sect, text="PlatformIO Env:", font=("Helvetica", 10)).pack(anchor="w")
        self.env_entry = ctk.CTkEntry(sect, placeholder_text=detect_env())
        self.env_entry.insert(0, self.cfg.get("env", ""))
        self.env_entry.pack(fill="x", pady=(0, 8))
        self.env_entry.bind("<KeyRelease>", lambda _: self._save_setting("env", self.env_entry))

        # Port
        ctk.CTkLabel(sect, text="Serial Port:", font=("Helvetica", 10)).pack(anchor="w")
        self.port_entry = ctk.CTkEntry(sect, placeholder_text="auto-detect")
        self.port_entry.insert(0, self.cfg.get("port", ""))
        self.port_entry.pack(fill="x", pady=(0, 8))
        self.port_entry.bind("<KeyRelease>", lambda _: self._save_setting("port", self.port_entry))

        # Device IP
        ctk.CTkLabel(sect, text="Device IP:", font=("Helvetica", 10)).pack(anchor="w")
        self.ip_entry = ctk.CTkEntry(sect)
        self.ip_entry.insert(0, self.cfg.get("device_ip", "192.168.4.1"))
        self.ip_entry.pack(fill="x", pady=(0, 8))
        self.ip_entry.bind("<KeyRelease>", lambda _: self._save_setting("device_ip", self.ip_entry))

        # Chip
        ctk.CTkLabel(sect, text="Chip Type:", font=("Helvetica", 10)).pack(anchor="w")
        self.chip_var = ctk.StringVar(value=self.cfg.get("chip", "esp32c3"))
        chip_menu = ctk.CTkOptionMenu(
            sect,
            values=["esp32c3", "esp32c3_supermini", "esp32"],
            variable=self.chip_var,
            command=lambda v: self._save_setting("chip", None, v),
        )
        chip_menu.pack(fill="x", pady=(0, 8))

        # Baud rate
        ctk.CTkLabel(sect, text="Baud Rate:", font=("Helvetica", 10)).pack(anchor="w")
        self.baud_entry = ctk.CTkEntry(sect)
        self.baud_entry.insert(0, str(self.cfg.get("baud", 921600)))
        self.baud_entry.pack(fill="x", pady=(0, 8))
        self.baud_entry.bind("<KeyRelease>", lambda _: self._save_setting("baud", self.baud_entry, int_val=True))

        # Upload filter
        ctk.CTkLabel(sect, text="Upload Filter:", font=("Helvetica", 10)).pack(anchor="w")
        self.filter_var = ctk.StringVar(value=self.cfg.get("upload_filter", "all"))
        filter_menu = ctk.CTkOptionMenu(
            sect,
            values=_UPLOAD_FILTERS,
            variable=self.filter_var,
            command=lambda v: self._save_setting("upload_filter", None, v),
        )
        filter_menu.pack(fill="x", pady=(0, 8))

        # Wipe toggle
        self.wipe_var = ctk.BooleanVar(value=self.cfg.get("wipe_before_upload", False))
        wipe_check = ctk.CTkCheckBox(
            sect,
            text="Wipe /www before upload",
            variable=self.wipe_var,
            command=lambda: self._save_setting("wipe_before_upload", None, self.wipe_var.get()),
        )
        wipe_check.pack(anchor="w", pady=(0, 10))

    def _build_steps_section(self, sidebar: ctk.CTkFrame) -> None:
        """Step toggles."""
        sect = ctk.CTkFrame(sidebar, fg_color="#2a2a2a")
        sect.pack(fill="x", padx=10, pady=10)

        label = ctk.CTkLabel(sect, text="📋 Steps", font=("Helvetica", 12, "bold"))
        label.pack(anchor="w", pady=(10, 5))

        enabled_steps = set(self.cfg.get("steps", []))
        for n, name in STEP_NAMES.items():
            var = tk.BooleanVar(value=n in enabled_steps)
            self.step_vars[n] = var
            check = ctk.CTkCheckBox(
                sect,
                text=f"{n}. {name[:30]}…" if len(name) > 30 else f"{n}. {name}",
                variable=var,
                font=("Helvetica", 9),
                command=self._update_steps,
            )
            check.pack(anchor="w", pady=2)

    def _build_presets_section(self, sidebar: ctk.CTkFrame) -> None:
        """Preset buttons."""
        sect = ctk.CTkFrame(sidebar, fg_color="#2a2a2a")
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

    def _build_actions_section(self, sidebar: ctk.CTkFrame) -> None:
        """Action buttons."""
        sect = ctk.CTkFrame(sidebar, fg_color="#2a2a2a")
        sect.pack(fill="x", padx=10, pady=10)

        self.run_btn = ctk.CTkButton(
            sect,
            text="▶ RUN",
            command=self._on_run,
            font=("Helvetica", 12, "bold"),
            fg_color="#1f8f3b",
            hover_color="#16732e",
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
            fg_color="#4a5f8f",
        )
        wifi_btn.pack(fill="x", pady=3)

    def _build_tabs(self, parent: ctk.CTkFrame) -> None:
        """Tabbed interface on the right."""
        self.tab_view = ctk.CTkTabview(parent, fg_color="#242424")
        self.tab_view.pack(side="right", fill="both", expand=True)

        # Logs tab
        self._build_logs_tab()

        # Summary tab
        self._build_summary_tab()

    def _build_logs_tab(self) -> None:
        """Output/logs viewer."""
        tab = self.tab_view.add("📋 Logs")
        tab.grid_rowconfigure(0, weight=1)
        tab.grid_columnconfigure(0, weight=1)

        self.log_text = ctk.CTkTextbox(
            tab,
            font=("Courier", 10),
            text_color="#aaaaaa",
        )
        self.log_text.grid(row=0, column=0, sticky="nsew", padx=5, pady=5)

        # Clear button
        clear_btn = ctk.CTkButton(
            tab,
            text="Clear",
            command=self._clear_logs,
            width=80,
        )
        clear_btn.grid(row=1, column=0, sticky="e", padx=5, pady=5)

    def _build_summary_tab(self) -> None:
        """Summary/info tab."""
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
""")
        info_text.configure(state="disabled")

    def _save_setting(self, key: str, widget=None, val=None, int_val: bool = False) -> None:
        """Save a setting to config."""
        if widget:
            val = widget.get()
        if key == "baud" and val and int_val:
            try:
                val = int(val)
            except ValueError:
                return
        self.cfg[key] = val
        save_cfg(self.cfg)

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

    def _log(self, msg: str, end: str = "\n") -> None:
        """Log message to GUI."""
        if end == "":
            self.log_text.insert("end", msg)
        else:
            self.log_text.insert("end", msg + end)
        self.log_text.see("end")
        self.root.update()

    def _on_run(self) -> None:
        """Run selected steps in a background thread."""
        if self.running:
            messagebox.showwarning("Already Running", "Steps are already running.")
            return

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
                self.run_btn.configure(state="normal", text="▶ RUN")

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
        """WiFi provisioning (placeholder for now)."""
        messagebox.showinfo(
            "WiFi Provisioning",
            "WiFi provisioning via serial is not yet implemented in the GUI.\n\n"
            "For now, use: python3 deploy.py [W]\n\n"
            "Or you can manually connect the device to WiFi via the web UI.",
        )

    def _save_config(self) -> None:
        """Save configuration."""
        save_cfg(self.cfg)
        messagebox.showinfo("Saved", "Configuration saved to .flash_tool.json")


def main() -> int:
    """Entry point."""
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
