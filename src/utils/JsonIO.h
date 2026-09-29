// ============================================================================
// src/utils/JsonIO.h — every JSON read from a file and every JSON write to a
// String or a char buffer, through the two ArduinoJson paths the image already
// carries.
//
// WHY THESE EXIST
// ---------------
// ArduinoJson's parser is a template on its input and its serialiser a
// template on its output. Each new type handed to deserializeJson() or
// serializeJson() compiles the whole engine once more: a File as input cost
// 3.5 KB, a String and a char[] as output 3.3 KB between them. The firmware
// keeps exactly one of each:
//   * input:  (const char*, length)  — HTTP bodies, and files through here
//   * output: Print&                 — responses, files, and the two below
// tools/check_flash_trims.py reads the linked ELF and fails the build when a
// third type appears.
//
// deserializeJsonFile() reads the whole file into a heap buffer first, so a
// parse needs the file's size in free heap on top of the document for as
// long as it runs. The files read this way are configuration (a few KB);
// anything over maxBytes is refused with NoMemory rather than buffered.
// ============================================================================
#pragma once

#include <ArduinoJson.h>
#include <FS.h>

// Files larger than this are refused (NoMemory) instead of being buffered.
constexpr size_t JSON_FILE_MAX = 32 * 1024;

DeserializationError deserializeJsonFile(JsonDocument& doc, fs::File& f,
                                         size_t maxBytes = JSON_FILE_MAX);
DeserializationError deserializeJsonFile(JsonVariant dst, fs::File& f,
                                         size_t maxBytes = JSON_FILE_MAX);
DeserializationError deserializeJsonFile(JsonDocument& doc, fs::File& f,
                                         const JsonDocument& filter,
                                         size_t maxBytes = JSON_FILE_MAX);

// serializeJson(v, out) — appends to `out`. Returns the bytes written.
size_t jsonToString(JsonVariantConst v, String& out);

// serializeJson(v, buf, cap) — at most cap-1 bytes and a terminator, and the
// count written. A document that does not fit is cut short, exactly like
// the call it replaces, so callers that must not send a truncated document
// still compare against measureJson() first.
size_t jsonToBuf(JsonVariantConst v, char* buf, size_t cap);
