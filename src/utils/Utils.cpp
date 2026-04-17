#include "Utils.h"
#include <FS.h>
#include <vector>

// getVersionString() is defined inline in Config.h – removed from here.

String formatFileSize(uint64_t bytes) {
    if (bytes >= 1073741824ULL) return String(bytes / 1073741824.0, 2) + " GB";
    if (bytes >= 1048576)       return String(bytes / 1048576.0, 1) + " MB";
    if (bytes >= 1024)          return String(bytes / 1024.0, 1) + " KB";
    return String((unsigned long)bytes) + " B";
}

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
    if (filename.length() > 64) return "";
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
    if (path == "/config.bin")      return true;
    if (path == "/bootcount.bin")   return true;
    if (path == "/reset_log.txt")   return true;
    if (path.startsWith("/_setup/") || path == "/_setup") return true;
    return false;
}

bool deleteRecursive(fs::FS& fs, const String& path) {
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return fs.remove(path);
    }

    std::vector<String> entries;
    while (File entry = dir.openNextFile()) {
        String entryName = String(entry.name());
        entries.push_back(entryName.startsWith("/") ? entryName : buildPath(path, entryName));
        entry.close();
    }
    dir.close();

    for (const String& childPath : entries) {
        File child = fs.open(childPath);
        bool isDir = child && child.isDirectory();
        if (child) child.close();
        
        if (isDir) {
            deleteRecursive(fs, childPath);
        } else {
            fs.remove(childPath);
        }
    }

    return fs.rmdir(path);
}