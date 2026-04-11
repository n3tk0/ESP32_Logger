"""
Pre-build script: copy Logger.ino from the project root into src/ so
PlatformIO's default Arduino sketch discovery finds it.

Why this is needed:
  Arduino IDE expects the main sketch (Logger.ino) in the project root.
  PlatformIO expects source files in src/ and its build_src_filter does
  not reliably pick up .ino files outside of src_dir.  Copying at build
  time keeps both tool-chains happy without duplicating the file in git.

The copied src/Logger.ino is listed in .gitignore.
"""

Import("env")  # type: ignore  # provided by PlatformIO
import os
import shutil

project_dir = env["PROJECT_DIR"]
src_dir     = env["PROJECT_SRC_DIR"]

sketch_src = os.path.join(project_dir, "Logger.ino")
sketch_dst = os.path.join(src_dir, "Logger.ino")

os.makedirs(src_dir, exist_ok=True)

if not os.path.exists(sketch_src):
    print("[copy_sketch] WARNING: Logger.ino not found in project root")
else:
    need_copy = (
        not os.path.exists(sketch_dst)
        or os.path.getmtime(sketch_src) > os.path.getmtime(sketch_dst)
    )
    if need_copy:
        shutil.copy2(sketch_src, sketch_dst)
        print(f"[copy_sketch] {sketch_src} -> {sketch_dst}")
