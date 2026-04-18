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