import requests
import sys

BASE = "http://192.168.1.3"
try:
    # /delete requires the per-boot CSRF token as a ?csrf= param, else it 403s.
    csrf = ""
    try:
        csrf = requests.get(f"{BASE}/api/csrf-token", timeout=3).json().get("token", "") or ""
    except Exception as e:
        print(f"WARN: could not fetch CSRF token ({e}); deletes may 403")

    print("Checking /logs...")
    r = requests.get(f"{BASE}/api/filelist?storage=internal&dir=/logs", timeout=5)
    files = r.json().get("files", [])
    if not files:
        print("No files in /logs")
    for f in files:
        if not f.get("isDir"):
            p = f["path"]
            params = {"path": p, "storage": "internal"}
            if csrf:
                params["csrf"] = csrf
            requests.post(f"{BASE}/delete", params=params, timeout=5)
            print(f"Deleted {p}")
except Exception as e:
    print(f"Error: {e}")
