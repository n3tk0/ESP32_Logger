"""
Pre-build script: copy the Arduino sketch from the project root into src/
so PlatformIO's default Arduino sketch discovery finds it.

Why this is needed:
  Arduino IDE expects the main sketch (e.g. ESP_Logger.ino) in the project
  root.  PlatformIO expects source files in src/ and its build_src_filter
  does not reliably pick up .ino files outside of src_dir.  Copying at
  build time keeps both tool-chains happy without duplicating the file in
  git.

The copied src/<sketch>.ino is listed in .gitignore.  Any historical sibling
copy (e.g. src/Logger.ino from before the rename) is removed so the linker
doesn't see two `setup()` definitions.
"""

Import("env")  # type: ignore  # provided by PlatformIO
import glob
import os
import shutil

project_dir = env["PROJECT_DIR"]
src_dir     = env["PROJECT_SRC_DIR"]

# Pick the first .ino in the project root.  Renames (Logger.ino →
# ESP_Logger.ino) just work without editing this script.
candidates = sorted(glob.glob(os.path.join(project_dir, "*.ino")))
# Skip stray reference sketches we don't want as the entry point.
candidates = [c for c in candidates
              if os.path.basename(c).lower() != "full_logger.ino"]

if not candidates:
    print("[copy_sketch] WARNING: no .ino sketch found in project root")
else:
    sketch_src  = candidates[0]
    sketch_name = os.path.basename(sketch_src)
    sketch_dst  = os.path.join(src_dir, sketch_name)

    os.makedirs(src_dir, exist_ok=True)

    # Drop any older sketches in src/ — leftovers from a rename would
    # double-define setup()/loop() and break the link.
    for stale in glob.glob(os.path.join(src_dir, "*.ino")):
        if os.path.basename(stale) != sketch_name:
            print(f"[copy_sketch] removing stale {stale}")
            os.remove(stale)

    need_copy = (
        not os.path.exists(sketch_dst)
        or os.path.getmtime(sketch_src) > os.path.getmtime(sketch_dst)
    )
    if need_copy:
        shutil.copy2(sketch_src, sketch_dst)
        print(f"[copy_sketch] {sketch_src} -> {sketch_dst}")
