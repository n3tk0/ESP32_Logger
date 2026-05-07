import requests
import sys

BASE = "http://192.168.1.3"
try:
    print("Checking /logs...")
    r = requests.get(f"{BASE}/api/filelist?storage=internal&dir=/logs", timeout=5)
    files = r.json().get("files", [])
    if not files:
        print("No files in /logs")
    for f in files:
        if not f.get("isDir"):
            p = f["path"]
            requests.post(f"{BASE}/delete?path={p}&storage=internal", timeout=5)
            print(f"Deleted {p}")
except Exception as e:
    print(f"Error: {e}")
