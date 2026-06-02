#include "Utils.h"
#include <FS.h>
#include <vector>
#include "../modules/UsbCdcModule.h"

// getVersionString() is defined inline in Config.h – removed from here.

String buildPath(const String& dir, const String& name) {
    if (dir == "/" || dir.isEmpty()) return "/" + name;
    return dir + "/" + name;
}

// Segment-aware path sanitiser.
// Walk the input component by component; reject any ".." segment outright
// (instead of the previous naive substring replace, which "....//" defeats).
// Strip "." and empty segments; reject control chars, backslash, NUL.
// Returns "" on unsafe input — callers MUST check and 400.
String sanitizePath(const String& path) {
    if (path.isEmpty()) return "";
    if (path.length() > 256) return "";   // hard cap

    for (size_t i = 0; i < path.length(); i++) {
        char c = path[i];
        // Reject ASCII control characters (< 0x20), DEL (0x7f), backslash and
        // NUL.  Bytes 0x80-0xFF (UTF-8 multi-byte lead/continuation) are
        // permitted by design — LittleFS stores paths as raw bytes and UTF-8
        // filenames are valid; if that changes, add `|| (unsigned char)c > 0x7e`.
        // (AUDIT 7.2)
        if (c == '\\' || c == '\0' || (unsigned char)c < 0x20 || c == 0x7f) {
            return "";
        }
    }

    String src = path;
    if (!src.startsWith("/")) src = "/" + src;

    String out = "";
    int start = 0;                         // points at a '/'
    while (start < (int)src.length()) {
        int slash = src.indexOf('/', start + 1);
        if (slash < 0) slash = src.length();
        String seg = src.substring(start + 1, slash);
        if (seg == "..") return "";        // traversal attempt
        if (seg.length() > 0 && seg != ".") {
            out += "/";
            out += seg;
        }
        start = slash;
    }
    if (out.isEmpty()) out = "/";
    return out;
}

String sanitizeFilename(const String& filename) {
    if (filename.isEmpty()) return "";
    // 96-char cap gives headroom for the longest generated names:
    // prefix + device-id + timestamp + ".txt" can reach ~50-60 chars.
    // (AUDIT 7.3)
    if (filename.length() > 96) return "";
    if (filename == "." || filename == "..") return "";

    for (size_t i = 0; i < filename.length(); i++) {
        char c = filename[i];
        if (c == '/' || c == '\\' || c == '\0' || (unsigned char)c < 0x20 || c == 0x7f) {
            return "";
        }
    }
    return filename;
}

bool isPathProtected(const String& path) {
    if (path.isEmpty()) return false;
    if (path == "/config.bin")              return true;
    if (path == "/bootcount.bin")           return true;
    if (path == "/reset_log.txt")           return true;
    if (path == "/board_profile.txt")       return true;  // R11
    if (path == "/platform_config.json")    return true;  // R5 — reveals MQTT/OSM secrets
    if (path == "/alerts.json")             return true;  // R5
    if (path == "/config/modules.json")     return true;  // R5 phase 3
    if (path == "/config.tmp")              return true;  // atomic-write scratch
    if (path == "/platform_config.tmp")     return true;
    if (path == "/board_profile.tmp")       return true;
    if (path.startsWith("/_setup/") || path == "/_setup") return true;
    return false;
}

// Diagnostic files that are write-protected (isPathProtected=true) but safe
// to download — they contain no secrets, only crash/debug info the user needs.
bool isPathDownloadAllowed(const String& path) {
    if (path == "/reset_log.txt")        return true;
    if (path == "/board_profile.txt")    return true;
    if (path == "/alerts.json")          return true;
    return false;
}

// Iterative deletion using an explicit work-stack on the heap.
// The previous recursive version called itself on every sub-directory, which
// risked blowing the ~4 KB AsyncTCP worker stack on deep trees.  This version
// uses post-order traversal: a directory is marked once its children have been
// pushed and gets rmdir'd on the second visit.
bool deleteRecursive(fs::FS& fs, const String& path) {
    struct Pending { String path; bool listed; };
    std::vector<Pending> stack;
    stack.push_back({ path, false });

    bool overallOk = true;

    while (!stack.empty()) {
        // Abort on suspiciously deep trees to prevent OOM on the AsyncTCP
        // worker stack (typically ~4 KB).  (AUDIT 7.4)
        if (stack.size() > 256) return false;

        Pending cur = stack.back();   // peek

        File entry = fs.open(cur.path);
        if (!entry) {
            stack.pop_back();
            // Unknown / already-gone — treat as success so a partial tree
            // can still be cleaned up.
            continue;
        }

        bool isDir = entry.isDirectory();
        entry.close();

        if (!isDir) {
            stack.pop_back();
            if (!fs.remove(cur.path)) overallOk = false;
            continue;
        }

        if (cur.listed) {
            // Children already processed; remove the directory itself.
            stack.pop_back();
            if (!fs.rmdir(cur.path)) overallOk = false;
            continue;
        }

        // Mark this directory and queue all its children for processing.
        stack.back().listed = true;
        File dir = fs.open(cur.path);
        if (dir && dir.isDirectory()) {
            while (File c = dir.openNextFile()) {
                String name = String(c.name());
                stack.push_back({
                    name.startsWith("/") ? name : buildPath(cur.path, name),
                    false
                });
                c.close();
            }
        }
        if (dir) dir.close();
    }

    return overallOk;
}

// ---------------------------------------------------------------------------
// urlEncode — percent-encode a string per RFC 3986 unreserved set.
// Inputs from AsyncWebParameter::value() are already url-decoded; re-encode
// here so the rebuilt query string round-trips characters like &, =, space,
// and Unicode bytes correctly.  Pre-allocates with an 8-char headroom which
// matches the typical worst-case for short ASCII params.
// ---------------------------------------------------------------------------
String urlEncode(const String& v) {
    String out;
    out.reserve(v.length() + 8);
    for (size_t i = 0; i < v.length(); i++) {
        char c = v[i];
        bool unreserved = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) { out += c; continue; }
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
        out += buf;
    }
    return out;
}

// ============================================================================
// PIN VALIDATION (Pillar 4.2 / 4.11)
// Centralized validation for sensor pins that integrates USB CDC detection
// ============================================================================

bool validatePin(int pin, const String& usage) {
    // Check for valid pin range. ESP32-C3 has 22 GPIO (0-21), S3 has 48 (0-47).
    // Note: validateAttachPin() performs stricter checks against the board profile's maxGpio
    if (pin < 0 || pin >= 48) {
        Serial.printf("[validatePin] INVALID: Pin %d out of range (usage: %s)\n", pin, usage.c_str());
        return false;
    }

    // ── USB CDC Conflict Detection ─────────────────────────────────────────
    // If USB CDC is enabled, pins 18/19 (ESP32-C3) or 19/20 (ESP32-S3) are locked
    if (usbCdc.isUsbPinLocked(pin)) {
        Serial.printf("[validatePin] CONFLICT: Pin %d reserved for USB CDC (usage: %s)\n", pin, usage.c_str());
        Serial.printf("              USB pins on this board: %s\n", usbCdc.getUsbPins().c_str());
        Serial.printf("              Disable USB CDC in deploy tool before using these pins\n");
        return false;
    }

    // ── Allowed ────────────────────────────────────────────────────────────
    // Board profile validation in validateAttachPin() handles strap pins and other
    // board-specific restrictions, so we don't duplicate those checks here
    Serial.printf("[validatePin] OK: Pin %d valid for %s\n", pin, usage.c_str());
    return true;
}
