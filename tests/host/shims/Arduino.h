#pragma once
// ----------------------------------------------------------------------------
// Minimal host shim for <Arduino.h>.
//
// Used ONLY by the desktop unit tests (tests/host/*) so that headers which do
// `#include <Arduino.h>` — e.g. src/core/SensorTypes.h — compile with a normal
// g++ toolchain.  It deliberately provides only the C stdlib symbols the pure
// logic under test relies on (fixed-width ints, snprintf, memset, strncpy,
// strcmp, math).  It is NOT a full Arduino emulation and must never be added
// to the firmware include path.
// ----------------------------------------------------------------------------
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
