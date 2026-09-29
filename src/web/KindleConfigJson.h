// ============================================================================
// src/web/KindleConfigJson.h — the Kindle appearance settings, in and out.
//
// Four places read or write config.kindle field by field:
//   * GET  /api/kindle/config   (snake_case keys, effective defaults filled in)
//   * POST /api/kindle/config   (the same keys, as a form)
//   * the settings file's "kindle" section (camelCase keys, the stored bytes)
//   * importing that section
//
// Each used to be its own list, and the lists drifted: the settings file had
// no `lang` and no `layoutMode`, so a restored backup quietly put the page
// back in the firmware's language and its automatic layout. The plain fields
// are now one table (KindleConfigJson.cpp) that all four walk; what is left
// written out here and there is what really differs — rotation in degrees,
// the packed week/rule/size bytes, and the defaults the API shows.
// ============================================================================
#pragma once

#include <ArduinoJson.h>
#include "../core/Config.h"

// A form field's value by name, or nullptr when the form did not send it.
typedef const char* (*KdFormGet)(void* ctx, const char* key);

void kdConfigToApi(const KindleConfig& k, JsonObject out);
void kdConfigFromForm(KindleConfig& k, KdFormGet get, void* ctx);

// The settings file's section. FromFile leaves absent keys alone (a file from
// an older firmware restores what it knew about) and clamps the result.
void kdConfigToFile(const KindleConfig& k, JsonObject out);
void kdConfigFromFile(KindleConfig& k, JsonObjectConst in);
