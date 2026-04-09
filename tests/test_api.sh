#!/usr/bin/env bash
# tests/test_api.sh
#
# Integration tests for the ESP32 Logger REST API.
# Runs a suite of HTTP checks against a live device (or mock server).
#
# Usage:
#   ./tests/test_api.sh                       # defaults to http://waterlogger.local
#   ./tests/test_api.sh http://192.168.1.42   # explicit device IP
#   ./tests/test_api.sh --mock                # start a local mock server on port 8080
#
# Dependencies: curl, jq (optional for JSON assertions)

set -euo pipefail

BASE_URL="${1:-http://waterlogger.local}"
MOCK_MODE=false
PASS=0
FAIL=0

if [[ "${1:-}" == "--mock" ]]; then
    MOCK_MODE=true
    BASE_URL="http://127.0.0.1:8080"
fi

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

pass() { echo -e "  ${GREEN}✓${NC} $1"; ((PASS++)); }
fail() { echo -e "  ${RED}✗${NC} $1"; ((FAIL++)); }
info() { echo -e "  ${YELLOW}●${NC} $1"; }

assert_status() {
    local name="$1" url="$2" expected="${3:-200}" method="${4:-GET}" body="${5:-}"
    local args=(-s -o /dev/null -w "%{http_code}" -m 10)
    if [[ "$method" == "POST" ]]; then
        args+=(-X POST)
        [[ -n "$body" ]] && args+=(-H "Content-Type: application/json" -d "$body")
    fi
    local code
    code=$(curl "${args[@]}" "$url" 2>/dev/null) || code="000"
    if [[ "$code" == "$expected" ]]; then
        pass "$name → HTTP $code"
    else
        fail "$name → expected $expected, got $code ($url)"
    fi
}

assert_json_field() {
    local name="$1" url="$2" field="$3"
    if ! command -v jq &>/dev/null; then
        info "$name — skipped (jq not installed)"
        return
    fi
    local val
    val=$(curl -sf -m 10 "$url" 2>/dev/null | jq -r "$field" 2>/dev/null) || val=""
    if [[ -n "$val" && "$val" != "null" ]]; then
        pass "$name → $field = $val"
    else
        fail "$name → field '$field' missing or null in $url"
    fi
}

# ── Mock server (Python) ──────────────────────────────────────────────────────
start_mock() {
    python3 - &
    MOCK_PID=$!
    sleep 1
}

mock_server() {
cat << 'PYEOF'
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

RESPONSES = {
    "/api/status":  {"version":"test","device":"MockLogger","chip":"ESP32-C3","fsUsed":4096,"fsTotal":1048576},
    "/api/live":    {"time":"12:00:00","state":"IDLE","heap":200000},
    "/api/sensors": {"sensors":[{"id":"test","type":"bme280","enabled":True,"status":"ok","error_count":0}]},
    "/api/diag":    {"free_heap":200000},
    "/api/filelist":{"files":[],"currentFile":""},
}

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args): pass  # suppress access log
    def do_GET(self):
        path = self.path.split("?")[0]
        body = json.dumps(RESPONSES.get(path, {"error":"not found"})).encode()
        code = 200 if path in RESPONSES else 404
        self.send_response(code); self.send_header("Content-Type","application/json")
        self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        body = json.dumps({"ok":True}).encode()
        self.send_response(200); self.send_header("Content-Type","application/json")
        self.end_headers(); self.wfile.write(body)

HTTPServer(("127.0.0.1", 8080), Handler).serve_forever()
PYEOF
}

if $MOCK_MODE; then
    echo "Starting mock API server on port 8080…"
    mock_server | start_mock
    trap "kill $MOCK_PID 2>/dev/null; exit" INT TERM EXIT
fi

# ── Test suite ────────────────────────────────────────────────────────────────
echo ""
echo "Testing: $BASE_URL"
echo "────────────────────────────────────────"

echo ""
echo "▶ Core endpoints"
assert_status "GET /api/status"             "$BASE_URL/api/status"
assert_status "GET /api/live"               "$BASE_URL/api/live"
assert_status "GET /api/sensors"            "$BASE_URL/api/sensors"
assert_status "GET /api/diag"               "$BASE_URL/api/diag"
assert_status "GET /api/filelist"           "$BASE_URL/api/filelist?storage=internal&dir=/"

echo ""
echo "▶ JSON fields"
assert_json_field "status.version"          "$BASE_URL/api/status"   ".version"
assert_json_field "status.chip"             "$BASE_URL/api/status"   ".chip"
assert_json_field "status.fsUsed"           "$BASE_URL/api/status"   ".fsUsed"
assert_json_field "live.state"              "$BASE_URL/api/live"     ".state"
assert_json_field "sensors array"           "$BASE_URL/api/sensors"  ".sensors | length"

echo ""
echo "▶ POST endpoints"
assert_status "POST /api/config/platform"   "$BASE_URL/api/config/platform"   200 POST
assert_status "POST /api/mqtt/ha_discovery" "$BASE_URL/api/mqtt/ha_discovery" 200 POST

echo ""
echo "▶ Expected 404s"
assert_status "GET /api/nonexistent"        "$BASE_URL/api/nonexistent" 404

echo ""
echo "▶ Sensor read_now"
assert_status "GET /api/sensors/read_now (no id)" \
    "$BASE_URL/api/sensors/read_now" 400

echo ""
echo "════════════════════════════════════════"
echo -e "Results: ${GREEN}${PASS} passed${NC}  ${RED}${FAIL} failed${NC}"

if $MOCK_MODE; then
    kill $MOCK_PID 2>/dev/null || true
fi
[[ $FAIL -eq 0 ]]
