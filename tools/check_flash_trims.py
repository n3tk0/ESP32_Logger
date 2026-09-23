#!/usr/bin/env python3
"""
check_flash_trims.py — the flash an image saved stays saved.

WHY THIS EXISTS
---------------
Four savings on the 4 MB C3 image (~53 KB with every feature on) leave no
trace in the source a reviewer would notice going away. Each is undone by an
ordinary-looking edit, and the only symptom is an image that grew:

  1. The big globals live in .bss. RemoteIngest, TrendRing and ReadingCache
     hold kilobytes of arrays and a portMUX spinlock whose unlocked state is
     NOT zero. Initialise that spinlock in the member declaration again and
     the compiler emits the whole object as initialised data — stored in the
     image and copied to RAM at boot. For remoteIngest alone that is 5.8 KB.

  2. LOGGER_TERSE_TLS_ERRORS: src/core/IdfTrims.c defines mbedtls_strerror so
     that mbedTLS's error.c, and its ~15 KB of sentences, is never linked.

  3. LOGGER_NO_COREDUMP: the same file defines the two IDF core dump entry
     points, so libespcoredump (~12 KB) is never linked on a partition table
     that has nowhere to write a core dump.

2 and 3 work because the linker only takes a member out of an archive for a
symbol nobody has defined yet. Anything in the build that references another
symbol from those members pulls them back in, and the definitions here then
save nothing — without an error.

  4. One copy of ArduinoJson's parser and one of its stream serialiser.
     Both are templates on their input and output type, so every new type
     passed to deserializeJson()/serializeJson() compiles the whole engine
     again — seven parser copies and four serialiser copies cost ~12 KB
     before they were folded. Memory input goes in as (const char*, length),
     files as a File, and anything that prints goes out as a Print&.

So this reads the linked ELF, not the source:
  * remoteIngest / trendRing / readingCache, where linked, are .bss symbols
  * with LOGGER_TERSE_TLS_ERRORS in the env's flags, mbedtls_high_level_strerr
    (error.c's table walker) is absent
  * with LOGGER_NO_COREDUMP, esp_core_dump_write_elf (libespcoredump) is absent
  * ArduinoJson's JsonDeserializer and JsonSerializer are instantiated only
    for the input and output types in JSON_READERS / JSON_WRITERS

Usage:
    python3 tools/check_flash_trims.py --env xiao_esp32c3
    python3 tools/check_flash_trims.py --env xiao_esp32c3 --elf path/to/firmware.elf

Needs a completed `pio run -e <env>`. nm is taken from the PlatformIO toolchain
of the env's chip.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from pio_envs import _build_flags, _sections, chip_for  # noqa: E402

BSS_GLOBALS = ("remoteIngest", "trendRing", "readingCache")

# flag -> (a symbol only the replaced archive member defines, what it costs)
TRIMS = {
    "LOGGER_TERSE_TLS_ERRORS": ("mbedtls_high_level_strerr",
                                "mbedTLS error.c (~15.6 KB)"),
    "LOGGER_NO_COREDUMP":      ("esp_core_dump_write_elf",
                                "libespcoredump (~11.9 KB)"),
}


# The one type each kind of call should go through; see 4. above. A new one
# means a call site passed something else — cast or convert it at the call
# (a String as .c_str(), .length(); a stream as static_cast<Print&>) rather
# than growing these lists.
JSON_READERS = {"BoundedReader<char const*, void>", "Reader<fs::File, void>"}
JSON_WRITERS = {"Writer<Print, void>", "Writer<String, void>",
                "StaticStringWriter", "DummyWriter"}
JSON_READER_RE = re.compile(r"JsonDeserializer<ArduinoJson::\w+::detail::"
                            r"((?:Bounded)?Reader<[^<>]*>)")
JSON_WRITER_RE = re.compile(r"(?:JsonSerializer|TextFormatter)<ArduinoJson::"
                            r"\w+::detail::(Writer<[^<>]*>|\w+Writer)\s*>")


def find_nm(chip: str) -> str:
    pkg = Path.home() / ".platformio" / "packages"
    name = "toolchain-riscv32-esp" if chip in ("esp32c3", "esp32c6", "esp32h2") \
        else f"toolchain-xtensa-{chip}"
    prefix = "riscv32-esp-elf" if name.startswith("toolchain-riscv32") \
        else f"xtensa-{chip}-elf"
    nm = pkg / name / "bin" / f"{prefix}-nm"
    if not nm.is_file():
        sys.exit(f"FAIL: {nm} not found — run `pio run -e <env>` first")
    return str(nm)


def flag_on(flags: str, macro: str) -> bool:
    m = re.search(r"-D" + macro + r"(?:=(\S+))?", flags)
    return bool(m) and (m.group(1) or "1") != "0"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", required=True)
    ap.add_argument("--elf")
    a = ap.parse_args()

    elf = Path(a.elf) if a.elf else ROOT / ".pio" / "build" / a.env / "firmware.elf"
    if not elf.is_file():
        print(f"FAIL: {elf} not found — build the env first")
        return 1

    nm = find_nm(chip_for(a.env))
    out = subprocess.run([nm, elf], check=True,
                         capture_output=True, text=True).stdout
    demangled = subprocess.run([nm, "-C", elf], check=True,
                               capture_output=True, text=True).stdout
    kind = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            kind[parts[2]] = parts[1]

    flags = _build_flags(_sections(), a.env)
    bad = []

    for g in BSS_GLOBALS:
        k = kind.get(g)
        if k is not None and k not in ("b", "B"):
            bad.append(f"{g} is a '{k}' symbol, not .bss: a member with a "
                       f"non-zero initialiser (a portMUX_INITIALIZER_UNLOCKED?) "
                       f"put the whole object into the image")

    for macro, (sym, what) in TRIMS.items():
        if flag_on(flags, macro) and sym in kind:
            bad.append(f"{macro} is set but {sym} is linked: something "
                       f"references {what} again, so the trim saves nothing")

    readers = set(JSON_READER_RE.findall(demangled))
    writers = set(JSON_WRITER_RE.findall(demangled))
    for t in sorted(readers - JSON_READERS):
        bad.append(f"ArduinoJson's parser is compiled for {t} too: pass "
                   f"memory as (const char*, length) — see JSON_READERS")
    for t in sorted(writers - JSON_WRITERS):
        bad.append(f"ArduinoJson's serialiser is compiled for {t} too: pass "
                   f"a stream as static_cast<Print&> — see JSON_WRITERS")

    for b in bad:
        print(f"FAIL: {b}")
    if not bad:
        on = [m for m in TRIMS if flag_on(flags, m)]
        print(f"OK: {a.env}: {', '.join(BSS_GLOBALS)} in .bss; "
              f"trims holding: {', '.join(on) or 'none set'}; "
              f"ArduinoJson: {len(readers)} parser(s), {len(writers)} writer(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
