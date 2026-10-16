// ============================================================================
// src/utils/JsonCopy.h
//
// jsonCopy(r.sensorId) — a char array handed to ArduinoJson so that the
// document keeps a copy of it.
//
// ArduinoJson 7.3+ takes any `const char[N]` lvalue for a string literal and
// stores only a pointer to it. Through a const reference, a struct's char
// array is exactly that type: when the struct is freed or rewritten before
// the document is serialized, the reply carries whatever that memory holds by
// then. /api/latest freed its readings before sending and its replies came
// out longer or shorter than measureJson() said (2026-10-16).
//
// Through a `const char*` the string is copied. This takes only an array, so
// a field that is not one fails to compile instead of becoming a pointer.
// tools/check_json_char_arrays.py fails on a char-array member put into JSON
// without it.
// ============================================================================
#pragma once

#include <stddef.h>

template <size_t N>
inline const char* jsonCopy(const char (&s)[N]) { return s; }
