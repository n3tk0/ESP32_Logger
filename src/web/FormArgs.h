// ============================================================================
// src/web/FormArgs.h — reading a POSTed form field (or a query parameter)
// once.
//
// The handlers used to write every field as
//     if (r->hasParam("k", true)) x = r->getParam("k", true)->value()…;
// which searches the parameter list twice and spells the key twice. getParam
// alone answers both questions — nullptr is "not sent" — so each field is one
// lookup:
//     if (const String* v = formArg(r, "k")) x = v->toInt();
// ============================================================================
#pragma once

#include <ESPAsyncWebServer.h>

// The value the form sent for `key`, or nullptr when it sent none.
inline const String* formArg(AsyncWebServerRequest* r, const char* key) {
    const AsyncWebParameter* p = r->getParam(key, true);
    return p ? &p->value() : nullptr;
}

// The same for a query-string parameter (?key=…).
inline const String* queryArg(AsyncWebServerRequest* r, const char* key) {
    const AsyncWebParameter* p = r->getParam(key);
    return p ? &p->value() : nullptr;
}

// The query parameter's value, or `def` when the request did not send it.
String queryOr(AsyncWebServerRequest* r, const char* key, const String& def);

// Copies the field into dst (always terminated) when the form sent it.
bool formCopy(AsyncWebServerRequest* r, const char* key, char* dst, size_t n);

// formArg() as a C string, for appliers that take a getter and a context
// (applyNetworkForm, kdConfigFromForm). ctx is the AsyncWebServerRequest*.
const char* formParam(void* ctx, const char* key);
