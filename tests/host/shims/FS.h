#pragma once
// Host shim for <FS.h> — minimal fs::FS / fs::File with stub methods.
//
// Just enough for filesystem-touching firmware code (e.g. Utils.cpp's
// deleteRecursive) to COMPILE and LINK in host tests.  The methods are inert:
// the host tests never exercise real filesystem behaviour — they target the
// pure logic in the same translation unit (path sanitisers, etc.).
#include <Arduino.h>

namespace fs {

class File {
public:
    explicit operator bool() const { return false; }
    bool        isDirectory() const { return false; }
    File        openNextFile()      { return File(); }
    const char* name()        const { return ""; }
    void        close()             {}
};

class FS {
public:
    File open(const String&)               { return File(); }
    File open(const String&, const char*)  { return File(); }
    bool exists(const String&)             { return false; }
    bool remove(const String&)             { return true; }
    bool mkdir(const String&)              { return true; }
    bool rmdir(const String&)              { return true; }
};

} // namespace fs

// Arduino's <FS.h> re-exports these into the global namespace; mirror that so
// firmware code using unqualified `File` / `FS` compiles.
using fs::File;
using fs::FS;
