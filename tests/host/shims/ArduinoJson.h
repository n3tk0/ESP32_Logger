#pragma once
// ----------------------------------------------------------------------------
// Host shim for <ArduinoJson.h>: the real library, vendored.
//
// The firmwares get ArduinoJson from PlatformIO's lib_deps (all three pin
// bblanchon/ArduinoJson @ ^7.4.3). The host tests are one g++ call per file
// with no package manager, so the same version is kept here as the
// single-header release, unmodified:
//
//   tests/host/vendor/ArduinoJson-v7.4.3.h
//   https://github.com/bblanchon/ArduinoJson/releases/download/v7.4.3/ArduinoJson-v7.4.3.h
//   sha256 ab5fbb8268b846b5f4bc5a5fee11bb2c96f7b8b846f5bef6540afb6a9cc76a5b
//   MIT licence (in the file's own header)
//
// Vendored rather than fetched in CI so a test run needs no network and
// cannot silently pick up a different version than the devices run. Bump it
// together with lib_deps. Code that reaches this through
// src/nodecfg/NodeConfigJson.h is the same code the node compiles — which is
// the point: the decoder's edge cases are ArduinoJson's edge cases.
//
// Like the rest of shims/, never on a firmware include path.
//
// One warning is silenced, for the library's lines only: at -O1 with the
// sanitizers, GCC 13 reports CollectionIterator::nextId_ as maybe-uninitialized
// inside ArduinoJson's own findKey(). It is a known false positive of the
// optimiser's flow analysis (the member is written by every constructor), it
// is not in code this repository owns, and the host tests are meant to build
// silent so that a real warning in OUR code stands out. PlatformIO compiles
// the library as a dependency and never shows it.
// ----------------------------------------------------------------------------
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include "../vendor/ArduinoJson-v7.4.3.h"
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
