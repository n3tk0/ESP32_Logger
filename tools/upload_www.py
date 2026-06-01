#!/usr/bin/env python3
"""Upload dist/www/*.gz files to ESP32 Water Logger via HTTP."""
import requests, os, sys, time

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://192.168.1.3"
DIST = os.path.join(os.path.dirname(__file__), "..", "dist", "www")

# Connectivity check
try:
    r = requests.get(f"{BASE}/api/sysinfo", timeout=3)
    print(f"Device online ({r.status_code})")
except Exception as e:
    print(f"NOT reachable: {e}")
    sys.exit(1)

# Mutating endpoints (/upload, /delete, /mkdir) require the per-boot CSRF
# token as a ?csrf= param, else they 403. Fetch it once and pass it along.
CSRF = ""
try:
    CSRF = requests.get(f"{BASE}/api/csrf-token", timeout=3).json().get("token", "") or ""
    print("Fetched CSRF token" if CSRF else "WARN: no CSRF token returned")
except Exception as e:
    print(f"WARN: could not fetch CSRF token ({e}); requests may 403")

# Clean old www files
print("Cleaning /www/...")
for sub in ["/", "/js/", "/pages/"]:
    try:
        r = requests.get(f"{BASE}/api/filelist?storage=internal&dir=/www{sub}", timeout=5)
        for f in r.json().get("files", []):
            if not f.get("isDir"):
                p = f["path"]
                params = {"path": p, "storage": "internal"}
                if CSRF:
                    params["csrf"] = CSRF
                requests.post(f"{BASE}/delete", params=params, timeout=5)
                print(f"  DEL {p}")
    except:
        pass
time.sleep(0.5)

# Create subdirectories first
print("Creating directories...")
for d in ["/www", "/www/js", "/www/pages"]:
    parts = d.strip("/").split("/")
    name = parts[-1]
    parent = "/" + "/".join(parts[:-1]) if len(parts) > 1 else "/"
    try:
        params = {"name": name, "dir": parent, "storage": "internal"}
        if CSRF:
            params["csrf"] = CSRF
        r = requests.post(f"{BASE}/mkdir", params=params, timeout=5)
        print(f"  MKDIR {d} -> {r.status_code}")
    except Exception as e:
        print(f"  MKDIR {d} -> {e}")
time.sleep(0.3)

# Upload .gz + non-compressible files
print("Uploading...")
for root, dirs, files in os.walk(DIST):
    for fname in sorted(files):
        if not fname.endswith(".gz") and not fname.endswith(".ico"):
            continue
        if "platform_config" in fname:
            continue
        fpath = os.path.join(root, fname)
        reldir = os.path.relpath(root, DIST).replace("\\", "/")
        upload_dir = "/www/" if reldir == "." else "/www/" + reldir + "/"
        size = os.path.getsize(fpath)
        print(f"  UP {upload_dir}{fname} ({size}B)", end=" ", flush=True)
        # Send path as query param (more reliable than form data for ESPAsyncWebServer)
        with open(fpath, "rb") as f:
            params = {"path": upload_dir, "storage": "internal"}
            if CSRF:
                params["csrf"] = CSRF
            r = requests.post(
                f"{BASE}/upload",
                params=params,
                files={"file": (fname, f)},
                timeout=30,
            )
            print(r.status_code)
        time.sleep(0.2)

print("All done!")
