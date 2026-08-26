#!/usr/bin/env python3
"""
flash_bootloader.py — Flash a rollback-enabled bootloader to an ESP32 device.

Uses pre-built bootloader binaries from tools/bootloader/.
These are compiled by the GitHub Actions CI with
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y via ESP-IDF.

Usage:
    python flash_bootloader.py                  # auto-detect port & chip
    python flash_bootloader.py --port COM3      # explicit port
    python flash_bootloader.py --chip esp32     # explicit chip
    python flash_bootloader.py --list-ports     # show available ports

Requirements:
    pip install esptool
"""

import argparse
import os
import sys
import subprocess
import glob

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BOOTLOADER_DIR = os.path.join(SCRIPT_DIR, "bootloader")

# Bootloader flash addresses by chip family.
#
# Keyed by CHIP, not by board: the bootloader offset is a property of the
# silicon (0x0 on the RISC-V parts and the S3, 0x1000 on the original ESP32),
# and every board of a family shares it. "esp32c3_supermini" is an ENVIRONMENT
# name that ended up here as though it were a chip; it is kept only as an
# alias so an older saved config still resolves.
CHIP_CONFIG = {
    "esp32c3": {
        "bootloader_addr": "0x0",
        "partition_addr":  "0x8000",
        "flash_mode":      "dio",
        "flash_freq":      "80m",
        "flash_size":      "4MB",
        "esptool_chip":    "esp32c3",
        "native_usb":      True,
    },
    "esp32s3": {
        "bootloader_addr": "0x0",
        "partition_addr":  "0x8000",
        "flash_mode":      "dio",
        "flash_freq":      "80m",
        "flash_size":      "8MB",
        "esptool_chip":    "esp32s3",
        "native_usb":      True,
    },
    "esp32": {
        "bootloader_addr": "0x1000",
        "partition_addr":  "0x8000",
        "flash_mode":      "dio",
        "flash_freq":      "40m",
        "flash_size":      "4MB",
        "esptool_chip":    "esp32",
        "native_usb":      False,
    },
}

# Board/env names that are not chips. Resolved before CHIP_CONFIG is consulted.
CHIP_ALIASES = {
    "esp32c3_supermini": "esp32c3",
    "xiao_esp32c3":      "esp32c3",
    "lolin_c3_pico":     "esp32c3",
    "xiao_esp32s3":      "esp32s3",
    "esp32s3_n16r8":     "esp32s3",
}


def find_serial_ports():
    """Return list of likely ESP32 serial ports."""
    patterns = []
    if sys.platform == "linux":
        patterns = ["/dev/ttyACM*", "/dev/ttyUSB*"]
    elif sys.platform == "darwin":
        patterns = ["/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.SLAB*"]
    elif sys.platform == "win32":
        # On Windows, list COM ports via esptool or serial
        try:
            import serial.tools.list_ports
            return [p.device for p in serial.tools.list_ports.comports()
                    if "USB" in (p.description or "") or "CP210" in (p.description or "")
                    or "CH340" in (p.description or "") or "JTAG" in (p.description or "")]
        except ImportError:
            return [f"COM{i}" for i in range(1, 20)]

    ports = []
    for pat in patterns:
        ports.extend(sorted(glob.glob(pat)))
    return ports


def detect_chip(port):
    """Try to auto-detect chip type via esptool chip_id."""
    try:
        result = subprocess.run(
            [sys.executable, "-m", "esptool", "--port", port, "chip_id"],
            capture_output=True, text=True, timeout=10
        )
        output = result.stdout + result.stderr
        # Chip family is all that matters here; which board carries it does
        # not change the bootloader offset.
        if "ESP32-C3" in output:
            return "esp32c3"
        elif "ESP32-S3" in output:
            # This used to answer "esp32c3" with the note "(RISC-V)". The S3 is
            # Xtensa, not RISC-V, and the answer would have written a C3
            # bootloader to an S3 — an image the ROM cannot start.
            return "esp32s3"
        elif "ESP32" in output:
            return "esp32"
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    return None


def resolve_chip(name):
    """Map a board/env name to its chip family; pass a chip through unchanged."""
    return CHIP_ALIASES.get(name, name)


def find_binary(chip, name):
    """Locate a pre-built binary for the given chip."""
    search_chip = resolve_chip(chip)

    path = os.path.join(BOOTLOADER_DIR, search_chip, name)
    if os.path.isfile(path):
        return path
    # Fallback: check flat directory
    flat = os.path.join(BOOTLOADER_DIR, f"{search_chip}_{name}")
    if os.path.isfile(flat):
        return flat
    return None


def flash(port, chip, bootloader_only=False, baud=921600):
    """Flash bootloader (and optionally partition table) via esptool."""
    chip = resolve_chip(chip)
    cfg = CHIP_CONFIG.get(chip)
    if not cfg:
        print(f"ERROR: Unsupported chip '{chip}'. Supported: {list(CHIP_CONFIG.keys())}"
              f" (board names accepted: {list(CHIP_ALIASES.keys())})")
        return False

    bl_bin = find_binary(chip, "bootloader.bin")
    if not bl_bin:
        print(f"ERROR: No bootloader.bin found for {chip} in {BOOTLOADER_DIR}/{chip}/")
        print("Run the GitHub Actions workflow to build it, or place it manually.")
        if chip == "esp32s3":
            print("NOTE: .github/workflows/build-bootloader.yml does not build an S3")
            print("      bootloader yet — only the C3 and the original ESP32.")
        return False

    pt_bin = find_binary(chip, "partition-table.bin")

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", cfg["esptool_chip"],
        "--port", port,
        "--baud", str(baud),
        "write_flash",
        "--flash_mode", cfg["flash_mode"],
        "--flash_freq", cfg["flash_freq"],
        "--flash_size", cfg["flash_size"],
        cfg["bootloader_addr"], bl_bin,
    ]

    if pt_bin and not bootloader_only:
        cmd.extend([cfg["partition_addr"], pt_bin])

    print(f"Chip:       {chip}")
    print(f"Port:       {port}")
    print(f"Bootloader: {bl_bin} @ {cfg['bootloader_addr']}")
    if pt_bin and not bootloader_only:
        print(f"Partitions: {pt_bin} @ {cfg['partition_addr']}")
    print()

    # Keyed off the silicon, not off one board name: every C3 and S3 board in
    # this project talks over native USB Serial/JTAG, and they all share this
    # failure mode. The Super Mini was simply the first one anybody hit it on.
    if cfg["native_usb"]:
        print("NOTE for boards with native USB (all C3 and S3 targets here):")
        print("  Auto-reset into the bootloader can fail over USB Serial/JTAG.")
        print("  If flashing hangs at 'Connecting...', hold the BOOT button,")
        print("  click the RST button, and then release BOOT.")
        print()

    # Safety prompt
    answer = input("Flash bootloader? This overwrites the existing bootloader. [y/N] ")
    if answer.strip().lower() != "y":
        print("Aborted.")
        return False

    print("\nFlashing...\n")
    result = subprocess.run(cmd)
    if result.returncode == 0:
        print("\nDone. Bootloader flashed successfully.")
        print("Now upload your firmware as usual (Arduino IDE / PlatformIO).")
        print("After the next OTA update, rollback-on-crash will be active.")
        return True
    else:
        print(f"\nERROR: esptool exited with code {result.returncode}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Flash a rollback-enabled bootloader to an ESP32 device."
    )
    parser.add_argument("--port", "-p", help="Serial port (auto-detect if omitted)")
    parser.add_argument("--chip", "-c", choices=list(CHIP_CONFIG.keys()),
                        help="Chip type (auto-detect if omitted)")
    parser.add_argument("--baud", "-b", type=int, default=921600, help="Flash baud rate")
    parser.add_argument("--bootloader-only", action="store_true",
                        help="Flash bootloader only (skip partition table)")
    parser.add_argument("--list-ports", action="store_true", help="List serial ports and exit")
    args = parser.parse_args()

    if args.list_ports:
        ports = find_serial_ports()
        if ports:
            print("Available serial ports:")
            for p in ports:
                print(f"  {p}")
        else:
            print("No serial ports found.")
        return

    # Check esptool is installed
    try:
        subprocess.run([sys.executable, "-m", "esptool", "version"],
                       capture_output=True, timeout=5)
    except (subprocess.TimeoutExpired, FileNotFoundError):
        print("ERROR: esptool not found. Install it with: pip install esptool")
        sys.exit(1)

    # Resolve port
    port = args.port
    if not port:
        ports = find_serial_ports()
        if len(ports) == 1:
            port = ports[0]
            print(f"Auto-detected port: {port}")
        elif len(ports) > 1:
            print("Multiple serial ports found:")
            for i, p in enumerate(ports):
                print(f"  [{i}] {p}")
            try:
                idx = int(input("Select port number: "))
                port = ports[idx]
            except (ValueError, IndexError):
                print("Invalid selection.")
                sys.exit(1)
        else:
            print("ERROR: No serial ports detected. Use --port to specify manually.")
            sys.exit(1)

    # Resolve chip
    chip = args.chip
    if not chip:
        print(f"Detecting chip on {port}...")
        chip = detect_chip(port)
        if chip:
            print(f"Detected: {chip}")
        else:
            print("Could not auto-detect chip. Use --chip esp32c3 or --chip esp32")
            sys.exit(1)

    success = flash(port, chip, bootloader_only=args.bootloader_only, baud=args.baud)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
