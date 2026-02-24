#include "Utils.h"
#include <FS.h>

String formatFileSize(uint64_t bytes) {
    if (bytes >= 1073741824ULL) return String(bytes / 1073741824.0, 2) + " GB";
    if (bytes >= 1048576)       return String(bytes / 1048576.0, 1) + " MB";
    if (bytes >= 1024)          return String(bytes / 1024.0, 1) + " KB";
    return String((unsigned long)bytes) + " B";
}

String getVersionString() {
    char buf[16];
    snprintf(buf, sizeof(buf), "v%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    return String(buf);
}

String buildPath(const String& dir, const String& name) {
    if (dir == "/" || dir.isEmpty()) return "/" + name;
    return dir + "/" + name;
}

String sanitizePath(const String& path) {
    String safe = path;
    safe.replace("..", "");
    safe.replace("//", "/");
    if (!safe.startsWith("/")) safe = "/" + safe;
    while (safe.length() > 1 && safe.endsWith("/"))
        safe = safe.substring(0, safe.length() - 1);
    return safe;
}

String sanitizeFilename(const String& filename) {
    String safe = filename;
    safe.replace("..", "");
    while (safe.indexOf("//") >= 0) safe.replace("//", "/");
    return safe;
}

bool deleteRecursive(fs::FS& fs, const String& path) {
    File dir = fs.open(path, FILE_READ);
    if (!dir || !dir.isDirectory()) return fs.remove(path);

    while (File entry = dir.openNextFile()) {
        String entryName = String(entry.name());
        String childPath = entryName.startsWith("/") ? entryName : buildPath(path, entryName);
        bool isDir = entry.isDirectory();
        entry.close();

        if (isDir) deleteRecursive(fs, childPath);
        else       fs.remove(childPath);
    }

    dir.close();
    return fs.rmdir(path);
}
